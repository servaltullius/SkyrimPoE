#include "PCH.h"
#include "runtime/SimpleImeBridge.h"

#include <array>
#include <atomic>
#include <string>

namespace SkyrimPoE::Runtime::SimpleImeBridge
{
    namespace
    {
        // SimpleIME v2.2.1 메뉴(깊이 13) 바로 아래에 두어 SimpleIME가 조합 완료 후 내려보내는 문자 이벤트만 수신한다.
        constexpr std::int8_t kSinkMenuDepth = 12;
        constexpr float kRepeatDelay = 0.45f;
        constexpr float kRepeatInterval = 0.05f;

        TextCallback g_onText = nullptr;
        KeyCallback g_onKey = nullptr;
        bool g_available = false;
        bool g_registered = false;
        std::atomic<bool> g_textEntry{ false };
        std::uint32_t g_pendingHighSurrogate = 0;

        // 프레임마다 물어 둔 SimpleIME 조합 상태 최근 2프레임 이력([0]이 최신)
        std::array<std::atomic<bool>, 2> g_composingHistory{};
        std::array<float, 256> g_nextRepeat{};

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

        void ForwardChar(std::uint32_t code)
        {
            if (code >= 0xD800 && code <= 0xDBFF) {
                g_pendingHighSurrogate = code;
                return;
            }
            std::uint32_t codePoint = code;
            if (code >= 0xDC00 && code <= 0xDFFF) {
                if (g_pendingHighSurrogate == 0) {
                    return;
                }
                codePoint = 0x10000 + ((g_pendingHighSurrogate - 0xD800) << 10) + (code - 0xDC00);
            }
            g_pendingHighSurrogate = 0;
            if (codePoint < 0x20 || codePoint == 0x7F) {
                return;
            }
            std::string text;
            AppendUtf8(text, codePoint);
            if (g_onText) {
                g_onText(text.c_str());
            }
        }

        const char* EditKeyName(RE::GFxKey::Code code) noexcept
        {
            switch (code) {
            case RE::GFxKey::kBackspace: return "Backspace";
            case RE::GFxKey::kDelete:    return "Delete";
            case RE::GFxKey::kReturn:    return "Enter";
            case RE::GFxKey::kEscape:    return "Escape";
            case RE::GFxKey::kLeft:      return "ArrowLeft";
            case RE::GFxKey::kRight:     return "ArrowRight";
            case RE::GFxKey::kHome:      return "Home";
            case RE::GFxKey::kEnd:       return "End";
            default:                     return nullptr;
            }
        }

        RE::GFxKey::Code EditKeyFromScanCode(std::uint32_t scanCode) noexcept
        {
            switch (scanCode) {
            case 0x0E: return RE::GFxKey::kBackspace;
            case 0xD3: return RE::GFxKey::kDelete;
            case 0x1C:
            case 0x9C: return RE::GFxKey::kReturn;
            case 0x01: return RE::GFxKey::kEscape;
            case 0xCB: return RE::GFxKey::kLeft;
            case 0xCD: return RE::GFxKey::kRight;
            case 0xC7: return RE::GFxKey::kHome;
            case 0xCF: return RE::GFxKey::kEnd;
            default:   return RE::GFxKey::kVoidSymbol;
            }
        }

        enum class ImeState : std::uint8_t
        {
            kIdle,
            kComposing,
            kUnknown,
        };

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

        void SampleComposing()
        {
            const bool composing = AskSimpleIme() == ImeState::kComposing;
            const bool previous = g_composingHistory[0].load(std::memory_order_acquire);
            g_composingHistory[1].store(previous, std::memory_order_release);
            g_composingHistory[0].store(composing, std::memory_order_release);
        }

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
                return a_message.menu == kMenuName ? RE::UI_MESSAGE_RESULTS::kHandled : RE::UI_MESSAGE_RESULTS::kPassOn;
            }

            void AdvanceMovie(float, std::uint32_t) override {}

            void PostDisplay() override
            {
                if (g_textEntry.load(std::memory_order_acquire)) {
                    SampleComposing();
                }
            }

            void RefreshPlatform() override {}

        private:
            static RE::UI_MESSAGE_RESULTS OnScaleformEvent(RE::UIMessage& a_message)
            {
                using Result = RE::UI_MESSAGE_RESULTS;
                if (!g_textEntry.load(std::memory_order_acquire)) {
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
                return Result::kPassOn;
            }
        };

        bool IsSimpleImeLoaded() noexcept
        {
            return REX::W32::GetModuleHandleW(L"SimpleIME.dll") != nullptr;
        }
    }

    Mode ParseMode(std::string_view modeStr) noexcept
    {
        if (modeStr == "on") return Mode::kOn;
        if (modeStr == "off") return Mode::kOff;
        return Mode::kAuto;
    }

    void Initialize(Mode mode, TextCallback onText, KeyCallback onKey)
    {
        g_onText = onText;
        g_onKey = onKey;

        const bool hasSimpleIme = IsSimpleImeLoaded();
        g_available = (mode == Mode::kOn) || (mode == Mode::kAuto && hasSimpleIme);
        SKSE::log::info("SimpleImeBridge: SimpleIME.dll={}, mode={}, enabled={}.",
            hasSimpleIme,
            mode == Mode::kOn ? "on" : (mode == Mode::kOff ? "off" : "auto"),
            g_available);
        if (!g_available || g_registered) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            SKSE::log::error("SimpleImeBridge: RE::UI unavailable; bridge disabled.");
            g_available = false;
            return;
        }
        ui->Register(kMenuName, SinkMenu::Create);
        g_registered = true;
        SKSE::log::info("SimpleImeBridge: registered '{}' at depth {}.", kMenuName, static_cast<int>(kSinkMenuDepth));
    }

    bool Available() noexcept
    {
        return g_available && g_registered;
    }

    bool TextEntryActive() noexcept
    {
        return g_textEntry.load(std::memory_order_acquire);
    }

    void SetTextEntry(bool active)
    {
        if (!Available() || g_textEntry.load(std::memory_order_acquire) == active) {
            return;
        }
        auto* controlMap = RE::ControlMap::GetSingleton();
        auto* queue = RE::UIMessageQueue::GetSingleton();
        if (!controlMap || !queue) {
            SKSE::log::error("SimpleImeBridge: ControlMap or UIMessageQueue unavailable.");
            return;
        }
        g_textEntry.store(active, std::memory_order_release);
        g_pendingHighSurrogate = 0;
        g_nextRepeat.fill(0.0f);
        g_composingHistory[0].store(false, std::memory_order_release);
        g_composingHistory[1].store(false, std::memory_order_release);

        controlMap->AllowTextInput(active);
        queue->AddMessage(kMenuName, active ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide, nullptr);
        SKSE::log::info("SimpleImeBridge: textEntry={} (ControlMap::textEntryCount={}).",
            active, static_cast<int>(controlMap->GetRuntimeData().textEntryCount));
    }

    void OnGameKey(std::uint32_t scanCode, bool pressed, float heldSeconds)
    {
        if (!g_textEntry.load(std::memory_order_acquire) || scanCode >= g_nextRepeat.size()) {
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
        const bool first = heldSeconds <= 0.0f;
        if (!first && heldSeconds < g_nextRepeat[scanCode]) {
            return;
        }
        g_nextRepeat[scanCode] = first ? kRepeatDelay : heldSeconds + kRepeatInterval;

        const char* name = EditKeyName(code);
        const auto now = AskSimpleIme();
        const bool recently = g_composingHistory[0].load(std::memory_order_acquire) ||
                              g_composingHistory[1].load(std::memory_order_acquire);
        const bool composing = (now == ImeState::kComposing) || recently;
        if (!composing && name && g_onKey) {
            g_onKey(name);
        }
    }
}
