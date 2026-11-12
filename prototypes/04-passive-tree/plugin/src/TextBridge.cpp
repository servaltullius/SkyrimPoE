#include "TextBridge.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <array>
#include <atomic>
#include <string>

namespace TextBridge
{
    namespace
    {
        constexpr auto kMenuName = "PoEProtoTreeTextSink";
        // 받는 메뉴의 깊이. SimpleIME의 메뉴(13)보다 아래에 두어, SimpleIME가 걸러 낸 뒤의 글자만 받는다.
        // SimpleIME는 자기가 보낸 글자를 보통의 글자 이벤트로 바꿔 아래로 흘려보내고, 입력기가 켜져 있는 동안 게임이 만든 날것의 글자는 막는다
        // (SimpleIME v2.2.1의 ImeMenu.cpp). 깊이 12에서 글자가 오는 것은 6차 시험에서 확인했다.
        constexpr std::int8_t kSinkMenuDepth = 12;
        // SimpleIME 2.x의 메뉴 이름(SimpleIME의 include/menu/MenuNames.h, v2.2.1). 조합 중인지 물어볼 때 찾는다.
        constexpr auto kSimpleImeMenuName = "SimpleImeMenu";

        Options       g_options;
        TextCallback  g_onText = nullptr;
        KeyCallback   g_onKey = nullptr;
        bool          g_available = false;
        bool          g_registered = false;
        // 메인 스레드에서 바꾸고, 메뉴의 ProcessMessage(같은 스레드)와 로그에서 읽는다.
        std::atomic<bool> g_textEntry{ false };
        // UTF-16의 앞쪽 대행 문자를 받고 뒤쪽을 기다리는 중이면 그 값.
        std::uint32_t g_pendingHighSurrogate = 0;

        // 프레임마다 물어 둔 "SimpleIME가 조합 중인가"의 최근 두 답([0]이 가장 최근). 그리는 쪽에서 쓰고 입력 쪽에서 읽는다.
        std::array<std::atomic<bool>, 2> g_composingHistory{};
        // 누르고 있는 키의 다음 되풀이 시각(누른 뒤 흐른 초). 스캔 코드로 찾는다.
        std::array<float, 256> g_nextRepeat{};
        constexpr float kRepeatDelay = 0.45f;
        constexpr float kRepeatInterval = 0.05f;

        void AppendUtf8(std::string& out, std::uint32_t codePoint)
        {
            if (codePoint < 0x80) {
                out.push_back(static_cast<char>(codePoint));
            } else if (codePoint < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            } else if (codePoint < 0x10000) {
                out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
        }

        // 글자 하나(UTF-16 단위 또는 코드 포인트)를 화면으로 넘긴다. 제어 문자는 넘기지 않는다(편집 키는 키 이벤트로 받는다).
        void ForwardChar(std::uint32_t code)
        {
            if (code >= 0xD800 && code <= 0xDBFF) {
                g_pendingHighSurrogate = code;
                return;
            }
            std::uint32_t codePoint = code;
            if (code >= 0xDC00 && code <= 0xDFFF) {
                if (g_pendingHighSurrogate == 0) {
                    return;  // 짝이 없는 뒤쪽 대행 문자
                }
                codePoint = 0x10000 + ((g_pendingHighSurrogate - 0xD800) << 10) + (code - 0xDC00);
            }
            g_pendingHighSurrogate = 0;
            if (codePoint < 0x20 || codePoint == 0x7F) {
                return;
            }
            std::string text;
            AppendUtf8(text, codePoint);
            if (g_options.logEvents) {
                SKSE::log::info("[입력 다리] 글자 U+{:04X} '{}'", codePoint, text);
            }
            if (g_onText) {
                g_onText(text.c_str());
            }
        }

        // 화면으로 넘길 편집 키의 이름. 넘기지 않는 키는 nullptr.
        const char* EditKeyName(RE::GFxKey::Code code)
        {
            switch (code) {
            case RE::GFxKey::kBackspace:
                return "Backspace";
            case RE::GFxKey::kDelete:
                return "Delete";
            case RE::GFxKey::kReturn:
                return "Enter";
            case RE::GFxKey::kEscape:
                return "Escape";
            case RE::GFxKey::kLeft:
                return "ArrowLeft";
            case RE::GFxKey::kRight:
                return "ArrowRight";
            case RE::GFxKey::kHome:
                return "Home";
            case RE::GFxKey::kEnd:
                return "End";
            default:
                return nullptr;
            }
        }

        // 편집 키의 스캔 코드(키보드 DirectInput)를 Scaleform의 키 코드로. 편집 키가 아니면 kVoidSymbol.
        RE::GFxKey::Code EditKeyFromScanCode(std::uint32_t scanCode)
        {
            switch (scanCode) {
            case 0x0E:
                return RE::GFxKey::kBackspace;
            case 0xD3:
                return RE::GFxKey::kDelete;
            case 0x1C:
            case 0x9C:  // 숫자판의 Enter
                return RE::GFxKey::kReturn;
            case 0x01:
                return RE::GFxKey::kEscape;
            case 0xCB:
                return RE::GFxKey::kLeft;
            case 0xCD:
                return RE::GFxKey::kRight;
            case 0xC7:
                return RE::GFxKey::kHome;
            case 0xCF:
                return RE::GFxKey::kEnd;
            default:
                return RE::GFxKey::kVoidSymbol;
            }
        }

        enum class ImeState
        {
            kIdle,       // 조합 중이 아니다
            kComposing,  // 조합 중이다(SimpleIME가 키 이벤트를 막는다)
            kUnknown,    // 물어보지 못했다(메뉴가 없거나 메시지를 만들지 못함)
        };

        // SimpleIME의 메뉴에 아무 뜻 없는 키 이벤트(키 코드 없음)를 직접 건네, 지금 키 이벤트를 막는지(조합 중인지)를 본다.
        // 게임이 메뉴들에 메시지를 돌릴 때 부르는 것과 같은 함수다. 키 코드가 없는 이벤트라 SimpleIME 쪽에 남는 것은 없다
        // (SimpleIME는 받은 키를 자기 화면의 키 상태로 옮기는데, 모르는 키는 옮기지 않는다. v2.2.1의 ImeMenu.cpp).
        // 메시지 데이터는 게임의 것을 빌려 만들고, 큐에 넣지 않았으므로 여기서 돌려준다.
        ImeState AskSimpleIme()
        {
            auto* ui = RE::UI::GetSingleton();
            const auto menu = ui ? ui->GetMenu(kSimpleImeMenuName) : nullptr;
            const auto* strings = RE::InterfaceStrings::GetSingleton();
            auto* factories = RE::MessageDataFactoryManager::GetSingleton();
            if (!menu || !strings || !factories) {
                return ImeState::kUnknown;
            }
            const auto* creator = factories->GetCreator<RE::BSUIScaleformData>(strings->bsUIScaleformData);
            auto* data = creator ? creator->Create() : nullptr;
            if (!data) {
                return ImeState::kUnknown;
            }
            RE::GFxKeyEvent event(RE::GFxEvent::EventType::kKeyDown, RE::GFxKey::kVoidSymbol, 0, 0, RE::GFxSpecialKeysState{}, 0);
            data->scaleformEvent = &event;
            RE::UIMessage message{};
            message.menu = strings->topMenu;
            message.type = RE::UI_MESSAGE_TYPE::kScaleformEvent;
            message.data = data;
            const auto result = menu->ProcessMessage(message);
            data->scaleformEvent = nullptr;
            delete data;
            return result == RE::UI_MESSAGE_RESULTS::kPassOn ? ImeState::kIdle : ImeState::kComposing;
        }

        // 프레임마다 한 번(우리 메뉴가 그려질 차례에). 조합 상태를 물어 최근 두 답을 갱신한다.
        void SampleComposing()
        {
            const bool composing = AskSimpleIme() == ImeState::kComposing;
            const bool previous = g_composingHistory[0].load();
            if (g_options.logEvents && composing != previous) {
                SKSE::log::info("[입력 다리] SimpleIME 조합 {}", composing ? "시작" : "끝");
            }
            g_composingHistory[1].store(previous);
            g_composingHistory[0].store(composing);
        }

        // 그리는 것이 없는 메뉴. 메뉴들을 따라 내려오는 Scaleform 이벤트를 받는 자리로만 쓴다.
        // 무비가 없으므로 무비를 건드리는 기본 동작(AdvanceMovie, PostDisplay, RefreshPlatform)을 모두 비워 둔다.
        class SinkMenu final : public RE::IMenu
        {
        public:
            static RE::IMenu* Create()
            {
                auto* menu = new SinkMenu();
                menu->menuFlags.set(RE::UI_MENU_FLAGS::kCustomRendering, RE::UI_MENU_FLAGS::kAllowSaving);
                menu->depthPriority = kSinkMenuDepth;
                return menu;
            }

            RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override
            {
                if (a_message.type == RE::UI_MESSAGE_TYPE::kScaleformEvent) {
                    return OnScaleformEvent(a_message);
                }
                // 이 메뉴 앞으로 온 것(열기, 닫기 등)은 처리한 것으로 하고, 남의 것은 그대로 흘려보낸다.
                return a_message.menu == kMenuName ? RE::UI_MESSAGE_RESULTS::kHandled : RE::UI_MESSAGE_RESULTS::kPassOn;
            }

            void AdvanceMovie(float, std::uint32_t) override {}
            // 화면에 그리는 것은 없다. 프레임마다 불리는 자리라, 여기서 조합 상태를 물어 둔다.
            void PostDisplay() override
            {
                if (g_textEntry.load()) {
                    SampleComposing();
                }
            }
            void RefreshPlatform() override {}

        private:
            static RE::UI_MESSAGE_RESULTS OnScaleformEvent(RE::UIMessage& a_message)
            {
                using Result = RE::UI_MESSAGE_RESULTS;
                if (!g_textEntry.load()) {
                    return Result::kPassOn;
                }
                const auto* data = static_cast<RE::BSUIScaleformData*>(a_message.data);
                const auto* event = data ? data->scaleformEvent : nullptr;
                if (!event) {
                    return Result::kPassOn;
                }
                if (event->type == RE::GFxEvent::EventType::kCharEvent) {
                    ForwardChar(static_cast<const RE::GFxCharEvent*>(event)->wCharCode);
                    return Result::kHandled;
                }
                // 키 이벤트는 여기서 쓰지 않는다. 편집 키는 게임의 키 입력에서 받는다(OnGameKey).
                return Result::kPassOn;
            }
        };

        bool SimpleImeLoaded()
        {
            return REX::W32::GetModuleHandleW(L"SimpleIME.dll") != nullptr;
        }
    }

    void Initialize(const Options& options, TextCallback onText, KeyCallback onKey)
    {
        g_options = options;
        g_onText = onText;
        g_onKey = onKey;

        const bool simpleIme = SimpleImeLoaded();
        g_available = options.mode == Mode::kOn || (options.mode == Mode::kAuto && simpleIme);
        SKSE::log::info("[입력 다리] SimpleIME {}, 설정 {}, 다리 {}", simpleIme ? "있음" : "없음",
            options.mode == Mode::kOn ? "on" : (options.mode == Mode::kOff ? "off" : "auto"), g_available ? "씀" : "안 씀");
        if (!g_available) {
            return;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            SKSE::log::error("[입력 다리] UI를 찾지 못해 메뉴를 등록하지 못함. 다리를 쓰지 않습니다.");
            g_available = false;
            return;
        }
        ui->Register(kMenuName, SinkMenu::Create);
        g_registered = true;
        SKSE::log::info("[입력 다리] 받는 메뉴 등록: {}, 깊이 {}", kMenuName, static_cast<int>(kSinkMenuDepth));
    }

    bool Available()
    {
        return g_available && g_registered;
    }

    bool TextEntryActive()
    {
        return g_textEntry.load();
    }

    void SetTextEntry(bool active)
    {
        if (!Available() || g_textEntry.load() == active) {
            return;
        }
        auto* controlMap = RE::ControlMap::GetSingleton();
        auto* queue = RE::UIMessageQueue::GetSingleton();
        if (!controlMap || !queue) {
            SKSE::log::error("[입력 다리] ControlMap이나 메시지 큐를 찾지 못해 글자 입력 상태를 바꾸지 못함");
            return;
        }
        g_textEntry.store(active);
        g_pendingHighSurrogate = 0;
        g_nextRepeat.fill(0.0f);
        g_composingHistory[0].store(false);
        g_composingHistory[1].store(false);
        // 게임의 글자 입력 허용은 횟수로 센다. 켠 만큼만 끈다(위에서 같은 값의 거듭 호출을 걸렀다).
        controlMap->AllowTextInput(active);
        queue->AddMessage(kMenuName, active ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
        SKSE::log::info("[입력 다리] 글자 입력칸 {} (게임의 글자 입력 수 {})", active ? "열림" : "닫힘",
            static_cast<int>(controlMap->GetRuntimeData().textEntryCount));
    }

    void OnGameKey(std::uint32_t scanCode, bool pressed, float heldSeconds)
    {
        if (!g_textEntry.load() || scanCode >= g_nextRepeat.size()) {
            return;
        }
        const auto code = EditKeyFromScanCode(scanCode);
        if (code == RE::GFxKey::kVoidSymbol) {
            return;
        }
        if (!pressed) {
            g_nextRepeat[scanCode] = 0.0f;
            return;
        }
        // 처음 눌렸을 때 한 번, 그 뒤로는 누르고 있는 동안 되풀이한다.
        const bool first = heldSeconds <= 0.0f;
        if (!first && heldSeconds < g_nextRepeat[scanCode]) {
            return;
        }
        g_nextRepeat[scanCode] = first ? kRepeatDelay : heldSeconds + kRepeatInterval;
        const char* name = EditKeyName(code);
        // 지금 조합 중이거나, 직전 두 프레임 안에 조합 중이었으면 이 키는 입력기의 것이다.
        // 직전 프레임까지 보는 것은, 이 키가 조합의 마지막 낱자를 지워 조합이 방금 끝났을 수 있기 때문이다.
        const auto now = AskSimpleIme();
        const bool recently = g_composingHistory[0].load() || g_composingHistory[1].load();
        const bool composing = now == ImeState::kComposing || recently;
        if (g_options.logEvents) {
            SKSE::log::info("[입력 다리] 편집 키 0x{:02X} {}{} -> {}", scanCode, name ? name : "?", first ? "" : " (되풀이)",
                composing ? (now == ImeState::kComposing ? "조합 중이라 넘기지 않음" : "방금까지 조합 중이었어서 넘기지 않음") :
                (now == ImeState::kUnknown ? "SimpleIME의 메뉴에 묻지 못해 그대로 화면으로 넘김" : "화면으로 넘김"));
        }
        if (!composing && name && g_onKey) {
            g_onKey(name);
        }
    }
}
