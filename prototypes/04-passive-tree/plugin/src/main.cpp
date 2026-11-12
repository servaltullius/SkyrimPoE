// PoE Proto 4: 패시브 트리 화면을 Prisma UI 뷰로 띄우는 시험용 플러그인.
// 뷰를 하나 만들고, 키로 열고 닫고, 화면이 보내는 측정 결과를 로그에 남긴다.
// 화면이 열려 있는 동안에는 키보드 입력이 게임과 다른 모드로 새지 않게 막고, SimpleIME가 있으면 글자 입력을 이어 준다(TextBridge).
// 키를 누를 때마다 "닫힘 → 게임을 멈추고 열기 → 게임을 돌린 채 열기 → 닫힘"으로 넘어가며,
// 단계마다 처음 열릴 때 화면이 측정을 스스로 돌리고, 멈춘 단계의 측정이 끝나면 화면이 다음 단계로 넘겨 달라고 알려 온다.
// 게임을 한 번 켜서 두 경우의 숫자를 모두 얻으려는 것이다. 측정 도중에 넘어가면 화면이 하던 측정을 버리고 새 단계에서 다시 잰다.

#include "PrismaUIApi.h"
#include "TextBridge.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <bitset>
#include <charconv>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <string_view>

namespace
{
    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoTree.ini";
    constexpr auto kViewPath = "PoEProtoTree/index.html";
    constexpr auto kReportListener = "poeTreeReport";    // 화면 → DLL: 측정 결과와 오류
    constexpr auto kCommandListener = "poeTreeCommand";  // 화면 → DLL: "next:<보낼 때의 단계>", "close", "text:on", "text:off"
    constexpr auto kShowFunction = "poeTreeOnShow";      // DLL → 화면: 열렸음을 알림(인자는 단계 이름)
    constexpr auto kHideFunction = "poeTreeOnHide";      // DLL → 화면: 닫혔음을 알림(화면이 하던 일을 멈춘다)
    constexpr auto kOptionFunction = "poeTreeSetOption"; // DLL → 화면: "이름=값" 꼴의 설정 하나
    constexpr auto kTextFunction = "poeTreeText";        // DLL → 화면: 입력 다리로 받은 글자(UTF-8)
    constexpr auto kKeyFunction = "poeTreeKey";          // DLL → 화면: 입력 다리로 받은 편집 키의 이름

    struct Settings
    {
        std::uint32_t toggleKey = 0x40;  // 키보드 DirectInput 스캔 코드. 0x40 = F6
        bool          autoBench = true;  // 단계마다 처음 열릴 때 화면이 측정을 스스로 돌린다
        bool          logSearchInput = false;  // 검색창에 들어온 글자와 조합 상태를 로그에 남긴다(입력 문제를 가를 때)
        TextBridge::Mode imeBridge = TextBridge::Mode::kAuto;  // SimpleIME와 함께 쓰는 입력 다리
    };

    enum class Stage
    {
        kClosed,
        kPaused,  // 게임을 멈추고 연 상태
        kLive,    // 게임을 돌린 채 연 상태
    };

    Settings                      g_settings;
    PRISMA_UI_API::IVPrismaUI1*   g_api = nullptr;
    PRISMA_UI_API::PrismaView     g_view = 0;
    std::atomic<bool>             g_domReady{ false };
    Stage                         g_stage = Stage::kClosed;  // 메인 스레드에서만 읽고 쓴다
    bool                          g_searchFocused = false;   // 화면의 검색창에 포커스가 있는가. 메인 스레드에서만 읽고 쓴다
    bool                          g_inputHooked = false;     // 입력 이벤트를 돌리는 자리에 끼어들었는가
    // 눌림을 다른 쪽에 넘기지 않은 키(스캔 코드). 그 키를 뗄 때까지의 이벤트를 모두 넘기지 않는다. 메인 스레드에서만 읽고 쓴다
    std::bitset<256>              g_hiddenKeys;

    bool InitializeLogging()
    {
        auto directory = SKSE::log::log_directory();
        if (!directory) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(*directory, error);
        if (error) {
            return false;
        }
        try {
            const auto path = *directory / "PoEProtoTree.log";
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
            auto logger = std::make_shared<spdlog::logger>("poe_proto_tree", std::move(sink));
            spdlog::set_default_logger(std::move(logger));
            spdlog::set_level(spdlog::level::info);
            spdlog::flush_on(spdlog::level::info);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

    std::string_view Trim(std::string_view text)
    {
        constexpr std::string_view kSpace = " \t\r\n";
        const auto first = text.find_first_not_of(kSpace);
        if (first == std::string_view::npos) {
            return {};
        }
        return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
    }

    // true/false(또는 1/0)를 읽는다. 다른 값이면 경고를 남기고 target을 그대로 둔다.
    void ReadBool(std::string_view key, std::string_view value, bool& target)
    {
        if (value == "true" || value == "1") {
            target = true;
        } else if (value == "false" || value == "0") {
            target = false;
        } else {
            SKSE::log::warn("설정: {} '{}'는 true 또는 false가 아니어서 기본값 유지", key, value);
        }
    }

    // "키 = 값" 줄만 읽는 작은 INI. 모르는 키와 잘못된 값은 경고를 남기고 기본값을 둔다.
    Settings LoadSettings()
    {
        Settings settings;
        std::ifstream file(kConfigPath);
        if (!file) {
            SKSE::log::warn("설정 파일을 열지 못해 기본값을 씀: {}", kConfigPath);
            return settings;
        }
        std::string line;
        while (std::getline(file, line)) {
            std::string_view text = line;
            if (text.starts_with("\xEF\xBB\xBF")) {
                text.remove_prefix(3);
            }
            if (const auto comment = text.find(';'); comment != std::string_view::npos) {
                text = text.substr(0, comment);
            }
            text = Trim(text);
            if (text.empty() || text.front() == '[') {
                continue;
            }
            const auto equals = text.find('=');
            if (equals == std::string_view::npos) {
                SKSE::log::warn("설정: '{}' 줄을 읽지 못함", text);
                continue;
            }
            const auto key = Trim(text.substr(0, equals));
            const auto value = Trim(text.substr(equals + 1));
            if (key == "ToggleKey") {
                std::uint32_t code = 0;
                const bool hex = value.starts_with("0x") || value.starts_with("0X");
                const auto digits = hex ? value.substr(2) : value;
                const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
                if (result.ec == std::errc{} && result.ptr == digits.data() + digits.size() && code >= 1 && code <= 255) {
                    settings.toggleKey = code;
                } else {
                    SKSE::log::warn("설정: ToggleKey '{}'는 1~255의 키 코드가 아니어서 기본값 유지", value);
                }
            } else if (key == "AutoBench") {
                ReadBool(key, value, settings.autoBench);
            } else if (key == "LogSearchInput") {
                ReadBool(key, value, settings.logSearchInput);
            } else if (key == "ImeBridge") {
                if (value == "auto") {
                    settings.imeBridge = TextBridge::Mode::kAuto;
                } else if (value == "on") {
                    settings.imeBridge = TextBridge::Mode::kOn;
                } else if (value == "off") {
                    settings.imeBridge = TextBridge::Mode::kOff;
                } else {
                    SKSE::log::warn("설정: ImeBridge '{}'는 auto, on, off 가운데 하나가 아니어서 기본값 유지", value);
                }
            } else {
                SKSE::log::warn("설정: 알 수 없는 키 '{}'", key);
            }
        }
        return settings;
    }

    void Notify(const char* message)
    {
        RE::SendHUDMessage::ShowHUDMessage(message);
        SKSE::log::info("{}", message);
    }

    const char* StageName(Stage stage)
    {
        switch (stage) {
        case Stage::kPaused:
            return "paused";
        case Stage::kLive:
            return "live";
        default:
            return "closed";
        }
    }

    bool ViewUsable()
    {
        return g_api && g_view != 0 && g_api->IsValid(g_view);
    }

    // 메인 스레드에서. 뷰를 닫는다. 이미 닫혀 있으면 아무 일도 하지 않는다.
    void CloseView()
    {
        // 검색창에 포커스를 둔 채로 닫아도 게임의 글자 입력 상태가 남지 않게 먼저 되돌린다(남으면 게임의 단축키가 먹지 않는다).
        TextBridge::SetTextEntry(false);
        g_searchFocused = false;
        if (!ViewUsable() || g_stage == Stage::kClosed) {
            g_stage = Stage::kClosed;
            return;
        }
        if (g_api->HasFocus(g_view)) {
            g_api->Unfocus(g_view);
        }
        // Prisma UI는 숨긴 뷰의 스크립트도 게임의 프레임마다 돌리고, 그 시간만큼 게임의 프레임이 늦어진다.
        // 그래서 숨기기 전에 화면에 알려 측정과 화면 갱신을 스스로 멈추게 한다.
        g_api->InteropCall(g_view, kHideFunction, "");
        g_api->Hide(g_view);
        g_stage = Stage::kClosed;
        Notify("[PoE Proto 4] 트리 화면을 닫음");
    }

    void CreateView();
    bool InstallInputHook();

    // 메인 스레드에서. 다음 단계로 넘어간다: 닫힘 → 멈추고 열기 → 돌린 채 열기 → 닫힘.
    void AdvanceStage()
    {
        if (!ViewUsable()) {
            // 게임 데이터가 불린 직후에는 Prisma UI가 아직 준비되지 않았을 수 있다. 키를 누를 때 한 번 더 만들어 본다.
            CreateView();
            Notify(ViewUsable() ? "[PoE Proto 4] 화면을 만드는 중. 잠시 뒤 다시 눌러 주세요" :
                                  "[PoE Proto 4] Prisma UI 뷰를 만들지 못함(로그 확인)");
            return;
        }
        if (!g_domReady.load()) {
            Notify("[PoE Proto 4] 화면을 아직 불러오는 중");
            return;
        }
        const auto* player = RE::PlayerCharacter::GetSingleton();
        if (g_stage == Stage::kClosed && (!player || !player->Is3DLoaded())) {
            Notify("[PoE Proto 4] 게임 안에서만 열 수 있음");
            return;
        }

        if (g_stage == Stage::kLive) {
            CloseView();
            return;
        }
        const bool pause = g_stage == Stage::kClosed;
        if (!g_inputHooked) {
            // 처음 열 때 끼어든다. 같은 자리에 끼어드는 다른 모드(휠러 같은 ImGui 화면들)는 나중에 끼어든 쪽이 입력을 먼저 받으므로,
            // 게임을 시작할 때 끼어들면 그 모드들이 우리보다 먼저 키를 받는다(10차 시험: 트리 화면에서 G를 누르자 휠러가 열림).
            g_inputHooked = InstallInputHook();
        }
        if (g_api->HasFocus(g_view)) {
            g_api->Unfocus(g_view);
        }
        g_api->Show(g_view);
        const bool focused = g_api->Focus(g_view, pause, false);
        g_stage = pause ? Stage::kPaused : Stage::kLive;
        SKSE::log::info("[단계] {}: Focus(pauseGame={}) 반환 {}, 숨김 {}", StageName(g_stage), pause, focused,
            g_api->IsHidden(g_view));
        Notify(pause ? "[PoE Proto 4] 트리 화면: 게임을 멈추고 엶" : "[PoE Proto 4] 트리 화면: 게임을 돌린 채 엶");
        g_api->InteropCall(g_view, kOptionFunction, g_settings.logSearchInput ? "logSearchInput=true" : "logSearchInput=false");
        // 화면에 어느 단계인지 알린다. AutoBench가 꺼져 있으면 단계 이름 앞에 '-'를 붙여 측정을 건너뛰게 한다.
        const std::string argument = std::string(g_settings.autoBench ? "" : "-") + StageName(g_stage);
        g_api->InteropCall(g_view, kShowFunction, argument.c_str());
    }

    void QueueOnMain(void (*task)())
    {
        if (const auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask(task);
        }
    }

    // 화면이 보낸 측정 결과와 오류. 여러 줄이면 줄마다 남긴다. Prisma UI의 스레드에서 불릴 수 있으므로 로그만 쓴다.
    void OnReport(const char* argument)
    {
        const std::string_view text = argument ? argument : "";
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto end = text.find('\n', start);
            const auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (!line.empty()) {
                SKSE::log::info("[화면] {}", line);
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
    }

    // 화면이 보낸 "next:<단계>"를 메인 스레드에서 처리한다. 화면이 그 명령을 보낼 때의 단계가 지금 단계와 같을 때만 넘긴다.
    // 화면의 명령은 한두 프레임 늦게 도착한다. 그 사이에 사용자가 F6으로 넘기거나 닫았다면, 늦게 온 명령이
    // 닫은 화면을 다시 열거나 단계를 하나 더 넘기지 않게 하려는 것이다.
    void AdvanceFrom(Stage sentAt)
    {
        if (g_stage != sentAt) {
            SKSE::log::info("화면의 '다음 단계' 명령을 버림: 보낼 때 {}, 지금 {}", StageName(sentAt), StageName(g_stage));
            return;
        }
        AdvanceStage();
    }

    // 화면의 버튼과, 측정이 끝났을 때 화면이 스스로 보내는 "다음 단계". Prisma UI의 스레드에서 불릴 수 있으므로 일은 메인 스레드로 넘긴다.
    void OnCommand(const char* argument)
    {
        const std::string_view command = argument ? argument : "";
        if (command == "next:paused") {
            QueueOnMain([] { AdvanceFrom(Stage::kPaused); });
        } else if (command == "next:live") {
            QueueOnMain([] { AdvanceFrom(Stage::kLive); });
        } else if (command == "close") {
            QueueOnMain([] { CloseView(); });
        } else if (command == "text:on") {
            // 검색창이 포커스를 받았다. 닫힌 뒤에 늦게 도착한 것이면 켜지 않는다.
            QueueOnMain([] {
                if (g_stage != Stage::kClosed) {
                    g_searchFocused = true;
                    TextBridge::SetTextEntry(true);
                }
            });
        } else if (command == "text:off") {
            QueueOnMain([] {
                g_searchFocused = false;
                TextBridge::SetTextEntry(false);
            });
        } else {
            SKSE::log::warn("화면이 모르는 명령을 보냄: '{}'", command);
        }
    }

    // 입력 다리가 받은 글자와 편집 키를 화면으로 넘긴다. 메뉴의 메시지 처리(메인 스레드)에서 불린다.
    void OnBridgeText(const char* utf8Text)
    {
        if (ViewUsable() && g_stage != Stage::kClosed) {
            g_api->InteropCall(g_view, kTextFunction, utf8Text);
        }
    }

    void OnBridgeKey(const char* keyName)
    {
        if (ViewUsable() && g_stage != Stage::kClosed) {
            g_api->InteropCall(g_view, kKeyFunction, keyName);
        }
    }

    void CreateView()
    {
        g_api = PRISMA_UI_API::RequestPluginAPI();
        if (!g_api) {
            SKSE::log::error("Prisma UI의 인터페이스를 얻지 못함. PrismaUI.dll이 로드돼 있는지 확인하세요.");
            return;
        }
        g_view = g_api->CreateView(kViewPath, [](PRISMA_UI_API::PrismaView view) {
            g_domReady.store(true);
            SKSE::log::info("화면을 불러옴(DOM 준비됨): view={}", view);
        });
        if (g_view == 0 || !g_api->IsValid(g_view)) {
            SKSE::log::error("Prisma UI 뷰를 만들지 못함: view={}, path={}", g_view, kViewPath);
            g_view = 0;
            return;
        }
        g_api->RegisterJSListener(g_view, kReportListener, OnReport);
        g_api->RegisterJSListener(g_view, kCommandListener, OnCommand);
        g_api->Hide(g_view);
        SKSE::log::info("Prisma UI 뷰를 만듦: view={}, path={}, 순서 {}", g_view, kViewPath, g_api->GetOrder(g_view));
    }

    // 메인 스레드에서(게임의 입력 처리 중). 이번에 들어온 입력 이벤트에서 우리가 쓸 키를 처리한다.
    void HandleInput(RE::InputEvent* events)
    {
        constexpr std::uint32_t kEscape = 0x01;
        for (auto* event = events; event; event = event->next) {
            const auto* button = event->AsButtonEvent();
            if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                continue;
            }
            const auto code = button->GetIDCode();
            if (code == g_settings.toggleKey) {
                if (button->IsDown()) {
                    QueueOnMain([] { AdvanceStage(); });
                }
            } else if (g_stage == Stage::kClosed) {
                continue;
            } else if (TextBridge::TextEntryActive()) {
                // 글자 입력칸이 열린 동안의 편집 키(지우기, 방향키, Enter, Esc 등)는 입력 다리가 화면으로 넘긴다.
                // 글자는 SimpleIME가 보내 주지만 이런 키는 게임의 입력으로만 오기 때문이다.
                if (g_settings.logSearchInput && button->IsDown()) {
                    // 한/영 전환 같은 키가 눌렸는지를 로그로 가를 수 있게 남긴다.
                    SKSE::log::info("[입력 다리] 게임 입력의 키 0x{:02X}", code);
                }
                TextBridge::OnGameKey(code, button->IsPressed(), button->HeldDuration());
            } else if (code == kEscape && button->IsDown() && !g_searchFocused) {
                // 검색창 밖에서의 Esc는 화면을 닫는다. Prisma UI가 키를 직접 받는 환경에서는 화면도 같은 일을 하지만,
                // SimpleIME가 켜져 있으면 화면에는 키가 가지 않으므로 여기서도 닫는다(두 번 닫아도 한 번만 반영된다).
                QueueOnMain([] { CloseView(); });
            }
        }
    }

    // 이 이벤트를 다른 쪽에 넘기지 않을 것인가. 트리 화면이 열려 있는 동안 새로 눌린 키는, 눌림부터 뗌까지를 통째로 넘기지 않는다.
    // 눌림만 빼고 뗌을 넘기면 뗄 때 반응하는 단축키가 그대로 돌고, 화면을 닫은 뒤에도 아직 누르고 있는 키가 게임에 들어간다.
    // 화면을 열기 전부터 누르고 있던 키는 건드리지 않는다(눌림이 이미 넘어갔으므로 뗌도 넘겨야 눌린 채로 남지 않는다).
    bool HideFromOthers(const RE::InputEvent& event)
    {
        if (event.GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
            return false;
        }
        const bool open = g_stage != Stage::kClosed;
        if (const auto* button = event.AsButtonEvent()) {
            const auto code = button->GetIDCode();
            if (code >= g_hiddenKeys.size()) {
                return false;
            }
            if (g_hiddenKeys.test(code)) {
                if (!button->IsPressed()) {
                    g_hiddenKeys.reset(code);
                }
                return true;
            }
            if (open && button->IsDown()) {
                g_hiddenKeys.set(code);
                return true;
            }
            return false;
        }
        return open && event.GetEventType() == RE::INPUT_EVENT_TYPE::kChar;
    }

    // 게임이 입력 이벤트를 듣는 쪽들(게임의 메뉴·조작 처리, SKSE의 Papyrus 키 이벤트, 다른 플러그인)에게 돌리는 자리에 끼어든다.
    // 우리가 쓸 키를 먼저 처리하고, 트리 화면이 열려 있으면 키보드 입력을 빼고 돌린다.
    // 빼지 않으면 검색창에 친 글자가 다른 모드의 단축키로도 들어가 그 모드의 창이 열린다(9차 시험: '='를 치자 다른 화면이 포커스를 가져감).
    // Prisma UI는 화면에 포커스가 있어도 이 이벤트들을 막지 않는다. 마우스와 게임패드는 Prisma UI가 이 길로 받으므로 건드리지 않는다.
    struct InputDispatchHook
    {
        static void thunk(RE::BSTEventSource<RE::InputEvent*>* a_source, RE::InputEvent* const* a_events)
        {
            if (!a_events || !*a_events) {
                func(a_source, a_events);
                return;
            }
            HandleInput(*a_events);
            if (g_stage == Stage::kClosed && g_hiddenKeys.none()) {
                func(a_source, a_events);
                return;
            }
            // 남길 이벤트만 다시 잇는다. 이 목록은 게임이 입력을 읽을 때마다 새로 만드는 것이라 이어 붙인 것이 다음으로 넘어가지 않는다.
            RE::InputEvent* head = nullptr;
            RE::InputEvent* tail = nullptr;
            for (auto* event = *a_events; event;) {
                auto* next = event->next;
                if (!HideFromOthers(*event)) {
                    if (tail) {
                        tail->next = event;
                    } else {
                        head = event;
                    }
                    tail = event;
                }
                event = next;
            }
            if (tail) {
                tail->next = nullptr;
            }
            RE::InputEvent* const remaining = head;
            func(a_source, &remaining);
        }

        static inline REL::Relocation<decltype(thunk)> func;
    };

    // BSInputDeviceManager::PollInputDevices 안에서 이벤트를 돌리는 호출. 여러 UI 플러그인이 같은 자리에 끼어든다.
    // 실행 파일이 암호화돼 있어 미리 확인하지 못했으므로, 실행 중에 그 자리가 호출 명령(E8)인지 보고 아니면 끼어들지 않는다.
    // 메인 스레드에서 부른다(그 호출도 메인 스레드에서 실행되므로, 고치는 동안 실행 중일 수 없다).
    bool InstallInputHook()
    {
        const REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(67315, 68617), 0x7B };
        const auto address = target.address();
        if (*reinterpret_cast<const std::uint8_t*>(address) != 0xE8) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                SKSE::log::error("입력 이벤트를 돌리는 호출을 찾지 못함(그 자리가 호출 명령이 아님). 키 입력을 막지 않고 읽기만 합니다.");
            }
            return false;
        }
        InputDispatchHook::func = SKSE::GetTrampoline().write_call<5>(address, InputDispatchHook::thunk);
        SKSE::log::info("입력 이벤트를 돌리는 호출에 끼어듦: 트리 화면이 열려 있는 동안 키보드 입력을 다른 쪽에 넘기지 않습니다.");
        return true;
    }

    // 끼어들기 전(트리 화면을 처음 열기 전)과 끼어들지 못했을 때 키 입력을 읽는다. 읽기만 하고 막지는 못한다.
    class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static InputSink& Get()
        {
            static InputSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            // 끼어든 뒤에는 그쪽에서 처리한다. 여기서도 하면 같은 키를 두 번 처리한다.
            if (a_events && !g_inputHooked) {
                HandleInput(*a_events);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        InputSink() = default;
    };

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (!message) {
            return;
        }
        switch (message->type) {
        case SKSE::MessagingInterface::kInputLoaded:
            if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
                input->AddEventSink(&InputSink::Get());
                SKSE::log::info("입력 싱크 등록(읽기 전용)");
            } else {
                SKSE::log::error("입력 장치 관리자를 찾지 못함. 키로 열 수 없습니다.");
            }
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            TextBridge::Initialize({ .mode = g_settings.imeBridge, .logEvents = g_settings.logSearchInput }, OnBridgeText, OnBridgeKey);
            CreateView();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            // 세이브를 불러오는 동안 화면이 입력을 쥐고 있지 않게 한다.
            if (g_stage != Stage::kClosed) {
                CloseView();
            }
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse, { .trampoline = true, .trampolineSize = 14 });
    InitializeLogging();

    g_settings = LoadSettings();
    SKSE::log::info("설정: ToggleKey=0x{:02X} AutoBench={} LogSearchInput={}", g_settings.toggleKey, g_settings.autoBench,
        g_settings.logSearchInput);

    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("SKSE 메시지 리스너를 등록하지 못함. 화면을 만들 수 없습니다.");
        return true;
    }
    SKSE::log::info("PoEProtoTree 준비 완료");
    return true;
}
