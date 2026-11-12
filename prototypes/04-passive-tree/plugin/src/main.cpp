// PoE Proto 4: 패시브 트리 화면을 Prisma UI 뷰로 띄우는 시험용 플러그인.
// 하는 일은 셋뿐이다. 뷰를 하나 만들고, 키로 열고 닫고, 화면이 보내는 측정 결과를 로그에 남긴다.
// 키를 누를 때마다 "닫힘 → 게임을 멈추고 열기 → 게임을 돌린 채 열기 → 닫힘"으로 넘어가며,
// 단계마다 처음 열릴 때 화면이 측정을 스스로 돌리고, 멈춘 단계의 측정이 끝나면 화면이 다음 단계로 넘겨 달라고 알려 온다.
// 게임을 한 번 켜서 두 경우의 숫자를 모두 얻으려는 것이다. 측정 도중에 넘어가면 화면이 하던 측정을 버리고 새 단계에서 다시 잰다.

#include "PrismaUIApi.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
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
    constexpr auto kCommandListener = "poeTreeCommand";  // 화면 → DLL: "next:<보낼 때의 단계>", "close"
    constexpr auto kShowFunction = "poeTreeOnShow";      // DLL → 화면: 열렸음을 알림(인자는 단계 이름)
    constexpr auto kHideFunction = "poeTreeOnHide";      // DLL → 화면: 닫혔음을 알림(화면이 하던 일을 멈춘다)
    constexpr auto kOptionFunction = "poeTreeSetOption"; // DLL → 화면: "이름=값" 꼴의 설정 하나

    struct Settings
    {
        std::uint32_t toggleKey = 0x40;  // 키보드 DirectInput 스캔 코드. 0x40 = F6
        bool          autoBench = true;  // 단계마다 처음 열릴 때 화면이 측정을 스스로 돌린다
        bool          logSearchInput = false;  // 검색창에 들어온 글자와 조합 상태를 로그에 남긴다(입력 문제를 가를 때)
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
        } else {
            SKSE::log::warn("화면이 모르는 명령을 보냄: '{}'", command);
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

    // 키 입력을 읽기만 한다. 이벤트를 고치거나 멈추지 않는다.
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
            if (!a_events) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto* event = *a_events; event; event = event->next) {
                const auto* button = event->AsButtonEvent();
                if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) {
                    continue;
                }
                if (button->GetIDCode() == g_settings.toggleKey && button->IsDown()) {
                    QueueOnMain([] { AdvanceStage(); });
                }
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
    SKSE::Init(skse);
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
