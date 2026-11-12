#include "Runtime/Hooks.h"

#include "Runtime/Channel.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace poe
{
    namespace
    {
        // IAnimationGraphManagerHolder::NotifyAnimationGraph. PlayerCharacter의 네 번째 가상 함수 표, 1번 칸.
        // OAR과 Animation Injector도 같은 칸을 쓰며, 서로 앞사람의 함수를 불러 주는 사슬이 된다.
        struct NotifyHook
        {
            static bool thunk(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event)
            {
                if (!Channel::IsSending() && Channel::Get().OnNotify(a_event)) {
                    return false;
                }
                return func(a_this, a_event);
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // Actor::Update. SE와 AE 모두 0xAD번 칸.
        struct UpdateHook
        {
            static void thunk(RE::PlayerCharacter* a_this, float a_delta)
            {
                func(a_this, a_delta);
                Channel::Get().Tick();
            }
            static inline REL::Relocation<decltype(thunk)> func;
        };

        // 입력을 읽기만 한다. 이벤트를 고치거나 멈추지 않는다.
        class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            static InputSink& Get()
            {
                static InputSink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events,
                RE::BSTEventSource<RE::InputEvent*>*) override
            {
                if (!a_events) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                for (auto* event = *a_events; event; event = event->next) {
                    const auto* button = event->AsButtonEvent();
                    if (!button) {
                        continue;
                    }
                    // 누름, 누르고 있음, 뗌을 모두 넘긴다. 뗌은 누른 시간이 0으로 기록된 것도 포함한다.
                    Channel::Get().OnButton(button->GetDevice(), button->GetIDCode(), button->IsPressed(),
                        button->IsDown());
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            InputSink() = default;
        };
    }

    void InstallPlayerHooks()
    {
        REL::Relocation<std::uintptr_t> graphHolder{ RE::VTABLE_PlayerCharacter[3] };
        NotifyHook::func = graphHolder.write_vfunc(0x1, NotifyHook::thunk);

        REL::Relocation<std::uintptr_t> actor{ RE::VTABLE_PlayerCharacter[0] };
        UpdateHook::func = actor.write_vfunc(0xAD, UpdateHook::thunk);

        SKSE::log::info("플레이어 훅 설치: NotifyAnimationGraph, Update");
    }

    void InstallInputSink()
    {
        auto* input = RE::BSInputDeviceManager::GetSingleton();
        if (!input) {
            SKSE::log::error("입력 장치 관리자를 찾지 못해 버튼 상태를 읽을 수 없음. 채널링이 시작되지 않습니다.");
            return;
        }
        input->AddEventSink(&InputSink::Get());
        SKSE::log::info("입력 싱크 등록(읽기 전용)");
    }
}
