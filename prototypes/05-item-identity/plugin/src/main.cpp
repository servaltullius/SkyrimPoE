// PoE Proto 5: 아이템의 동일성과 저장 왕복을 관찰하는 시험용 플러그인.
// 키를 한 번 누르면 정해진 절차를 스스로 차례로 돌린다. 시험용 아이템에 표시를 붙이고, 상자로 옮기고, 바닥에 버렸다 줍고,
// 장착했다 풀고, 저장하고 불러온다. 단계마다 인벤토리의 상태를 로그에 남겨, 어떤 표시가 어떤 경로에서 남는지를 본다.
// 여기서 정하려는 것은 "어픽스가 붙은 아이템을 무엇으로 다시 찾을 것인가"이고, 이 플러그인은 그 판단에 쓸 사실을 모은다.
//
// 6차부터: 바닥을 거친 아이템을 버리기 전의 아이템과 잇는 방법을 견준다. 줍는 순간에는 고유 ID 변경 이벤트가 오지 않아
// (4차 시험) 이벤트만으로는 이어지지 않는다. 내려놓는 길 둘, 줍는 길 둘을 섞어 다섯 개를 내려놓았다 하나씩 줍고,
// 줍기마다 "어느 표시가 돌아왔는가"와 "인벤토리의 어느 묶음이 그것인가"를 여러 방법으로 답하게 한 뒤 정답과 견준다.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoItems.ini";
    // 시험에 쓰는 폼(Skyrim.esm, houseCARL로 대조): 무기, 방어구, 잡화 하나씩과 빈 상자.
    constexpr RE::FormID kIronSword = 0x00012EB7;    // IronSword
    constexpr RE::FormID kIronHelmet = 0x00012E4D;   // ArmorIronHelmet
    constexpr RE::FormID kIronIngot = 0x0005ACE4;    // IngotIron
    constexpr RE::FormID kEmptyChest = 0x000F8478;   // TreasChestSmallEMPTYNoRespawn
    constexpr RE::FormID kSkeever = 0x00023AB7;      // EncSkeever (지켜보기 시험의 시체로 씀. houseCARL로 대조)
    // 시험이 만드는 세이브의 이름. 플레이하던 세이브는 건드리지 않는다.
    constexpr auto kBaselineSave = "PoEProto5_baseline";
    constexpr auto kMarkedSave = "PoEProto5_marked";
    constexpr auto kWatchSave = "PoEProto5_watch";  // 지켜보기 시험의 저장 왕복이 쓰는 세이브
    // 코세이브의 플러그인 표식과 레코드.
    constexpr std::uint32_t kCosaveId = 'PPI5';
    constexpr std::uint32_t kRecordMarks = 'MARK';
    constexpr std::uint32_t kRecordVersion = 2;  // 2: 바닥에 있는 표시도 적는다(6차부터)
    // 지켜보기 시험의 기록. 자동 시험의 기록('MARK')은 "플레이어, 시험 상자, 바닥" 셋 가운데 어디인지를 적지만, 이 기록은
    // 결정 59의 키를 그대로 적는다: 표시마다 (가진 쪽의 폼 ID, 번호). 가진 쪽은 상인의 상자, 동료, 시체처럼 무엇이든 될 수 있고,
    // 불러올 때 폼 ID를 게임에 다시 물어 푼다. 본 모드가 코세이브에 적을 모양과 같다.
    constexpr std::uint32_t kRecordWatch = 'WKEY';
    constexpr std::uint32_t kWatchRecordVersion = 1;
    // 바닥의 참조에 직접 남기는 고유 ID의 번호가 시작하는 곳("번호 남기기"). 표시 번호를 더해 쓴다.
    constexpr std::uint16_t kStampBase = 0x7F00;
    // 어느 표시인지 묻는 물음의 답이 표시 번호가 아닐 때의 값.
    constexpr int kNoAnswer = -1;   // 답을 내지 못함
    constexpr int kAmbiguous = -2;  // 후보가 여럿이라 고르지 않음
    // 방법의 이름 하나. 이 방법은 이벤트가 올 때만 답이 생기므로, 판정 때 답이 있었는지를 이름으로 찾는다.
    constexpr std::string_view kMethodUidEvent = "고유 ID 변경 이벤트(번호 남기기)";
    // 서로를 가려내려고 붙이는 숨은 꼬리표(단련 수치). 이름이 바뀌는 문턱(1.1)보다 한참 작아 겉으로는 보이지 않는다.
    constexpr float kTagA = 1.0011f;
    constexpr float kTagB = 1.0012f;
    constexpr auto  kNameTag = "PoE5 name tag";
    // 고유 ID만 남길 아이템에 잠깐 붙였다 떼는 이름. 부가 데이터가 있어야 주울 때 따로 떨어진 묶음이 되기 때문이다.
    constexpr auto  kTempNamePrefix = "PoE5 tmp ";
    // 불러오기를 요청한 뒤 끝났다는 알림을 기다리는 한도.
    constexpr auto  kLoadTimeout = std::chrono::seconds(180);
    constexpr auto  kTick = std::chrono::milliseconds(50);  // 시험 진행을 깨우는 간격
    constexpr int   kKeptLogs = 3;                         // 남겨 두는 지난 로그의 수
    constexpr std::string_view kStepMarker = "=== ";       // 단계가 시작될 때 로그에 남는 줄의 표지. 이것이 있는 로그가 시험 결과가 든 로그다

    struct Settings
    {
        std::uint32_t startKey = 0x44;  // 키보드 DirectInput 스캔 코드. 0x44 = F10 (F9는 게임의 빠른 불러오기라 쓰지 않는다)
        bool          saveLoad = true;  // 저장하고 불러오는 단계까지 돌린다
        bool          hookPickUp = true;  // 플레이어의 줍기 함수에 끼어들어, 주운 참조와 그때 생긴 묶음을 직접 본다
        bool          watch = false;      // 참이면 자동 단계 대신 "지켜보기" 시험을 돌린다(Scenario = watch)
    };

    // 시험 아이템 하나를 가리키는 이름표와, 그것을 인벤토리에서 다시 찾는 방법.
    enum class Mark : std::uint8_t
    {
        kSwordUid,      // 철 검: 엔진의 고유 ID만
        kSwordUidTagA,  // 철 검: 고유 ID + 꼬리표 A
        kSwordUidTagB,  // 철 검: 고유 ID + 꼬리표 B
        kSwordName,     // 철 검: 표시 이름만
        kHelmetUid,     // 철 투구: 고유 ID만
        kIngotUid,      // 철 주괴: 고유 ID만
        kCount,
    };

    constexpr std::array<const char*, static_cast<std::size_t>(Mark::kCount)> kMarkNames{
        "검/고유ID", "검/고유ID+꼬리표A", "검/고유ID+꼬리표B", "검/이름", "투구/고유ID", "주괴/고유ID",
    };

    // 코세이브에 쓰는 표시 하나의 기록.
    enum : std::uint8_t
    {
        kInPlayer = 0,
        kInChest = 1,
        kOnGround = 2,
    };

    struct SavedMark
    {
        std::uint8_t  mark = 0;
        std::uint8_t  where = kInPlayer;
        std::uint16_t uniqueId = 0;   // 바닥에 있으면 그 참조에 남긴 번호(안 남겼으면 0)
        RE::FormID    uniqueBase = 0;
        RE::FormID    chestRef = 0;
        RE::FormID    groundRef = 0;  // 바닥에 있을 때 그 참조
    };

    // 표시한 아이템을 가리키는 키: 가진 쪽의 폼 ID와 그 안에서의 번호. 엔진의 ExtraUniqueID가 담는 값과 같은 모양이다.
    // 엔진이 번호를 바꿀 때 보내는 이벤트(TESUniqueIDChangeEvent)로 따라간다.
    // 바닥에 내려놓으면 엔진이 (바닥의 참조, 0)으로 바꿨다고 알려 온다. 그때는 world가 참이고 owner가 그 참조의 폼 ID다.
    struct Key
    {
        RE::FormID    owner = 0;
        std::uint16_t uid = 0;
        bool          valid = false;
        bool          world = false;
    };

    struct Found
    {
        RE::TESObjectREFR* owner = nullptr;
        RE::ExtraDataList* extra = nullptr;
        const char*        where = "";
    };

    // 바닥에 내려놓는 길과 줍는 길. 어느 것이 인벤토리 메뉴와 조준해서 줍기가 쓰는 길인지는 확인하지 못했다(README).
    enum class DropPath : std::uint8_t
    {
        kRemoveItem,  // TESObjectREFR::RemoveItem(이유 kDropping). 5차까지 쓰던 길
        kDropObject,  // Actor::DropObject
    };

    enum class PickPath : std::uint8_t
    {
        kPickUpObject,  // Actor::PickUpObject를 직접 부름. 5차까지 쓰던 길
        kActivate,      // 바닥의 참조를 플레이어가 활성화(TESObjectREFR::ActivateRef)
    };

    // 바닥의 참조에 고유 ID(그 참조의 폼 ID, 우리 번호)를 남기는 때.
    enum class Stamp : std::uint8_t
    {
        kNone,
        kAfterDrop,  // 내려놓는 함수가 돌아온 직후(7차까지의 방식)
        kInEvent,    // 엔진이 보내는 고유 ID 변경 이벤트 안에서. 본 모드가 손으로 버린 아이템에 대해 할 수 있는 가장 이른 때다
    };

    // "어느 표시가 돌아왔는가"에 대한 방법 하나의 답.
    struct MarkClaim
    {
        const char* method;
        int         mark;  // 표시 번호, 또는 kNoAnswer, kAmbiguous
    };

    // "인벤토리의 어느 묶음이 그것인가"에 대한 방법 하나의 답.
    struct ListClaim
    {
        const char*        method;
        RE::ExtraDataList* list;  // 고르지 못했으면 nullptr
        std::string        why;   // 고르지 못한 까닭
    };

    // 지금 보고 있는 줍기 하나. 시험 단계가 시작한 줍기는 무엇을 줍는지 알고 있어 정답과 견줄 수 있다.
    struct Pickup
    {
        bool          automated = false;  // 시험 단계가 시작한 줍기
        bool          inHook = false;     // 줍기 함수 안에 있는 동안
        bool          hookSaw = false;    // 이 줍기가 줍기 함수를 거쳤다(끼어들기가 켜져 있을 때)
        bool          eventSeen = false;  // 플레이어에게 들어왔다는 컨테이너 이벤트가 왔다
        bool          laterDone = false;  // "줍기 뒤" 관찰을 이미 했다
        bool          hasPending = false; // 줍기 함수가 고른 답을 아직 키에 옮기지 않았다
        bool          stamped = false;    // 번호를 남겨 둔 참조를 줍는 줍기
        bool          linked = false;     // 잇는 절차가 이미 키를 이었다
        int           activated = -1;     // 활성화로 주웠을 때 그 반환값(1, 0). 활성화가 아니면 -1
        int           pendingMark = kNoAnswer;
        RE::ExtraDataList* pendingList = nullptr;
        std::uint32_t generation = 0;     // 줍기마다 하나씩 는다. 뒤늦게 도는 작업이 다른 줍기의 것을 건드리지 않게 한다
        int           expected = kNoAnswer;
        RE::FormID    base = 0;
        int           countBefore = 0;
        int           markByEventRef = kNoAnswer;
        std::vector<RE::ExtraDataList*>         before;  // 줍기 전 플레이어가 가진 그 폼의 묶음들
        std::vector<std::pair<int, RE::FormID>> ground;  // 줍기 전 바닥에 있던 표시들(표시 번호, 참조). 그 폼의 것만
        std::vector<MarkClaim>                  marks;
        std::vector<ListClaim>                  lists;
    };

    Settings                g_settings;
    // 아래 넷은 진행을 깨우는 스레드도 읽는다(atomic). 나머지 상태는 g_state를 잡고 만진다.
    std::atomic<bool>       g_running{ false };
    std::atomic<bool>       g_waitingForLoad{ false };
    std::atomic<bool>       g_pumpQueued{ false };
    std::atomic<Clock::rep> g_loadDeadline{ 0 };
    // 시험이 어디까지 왔는가. 자동 단계 → 손 조작을 기다림 → 마무리 단계 → 끝.
    enum class Phase : std::uint8_t
    {
        kIdle,
        kMain,         // 자동 단계가 도는 중
        kAwaitManual,  // 자동 단계가 끝나고, 사용자가 손으로 버리고 줍기를 기다리는 중
        kExtra,        // 손 조작이 기록된 뒤의 마무리 단계가 도는 중
        kWatchSetup,   // 지켜보기 시험의 준비 단계가 도는 중
        kWatch,        // 지켜보기: 사용자가 상인·동료·시체·제작을 직접 하고, 플러그인은 따라가며 판정을 남기는 중
        kWatchRoundTrip,  // 지켜보기 도중의 저장 왕복(저장하고 그 세이브를 불러옴)이 도는 중
        kDone,
    };
    Phase                   g_phase = Phase::kIdle;
    // 지켜보기 시험이 어디까지 왔는가. 0: 저장 왕복 전(시작 키 = 저장 왕복), 1: 저장 왕복 뒤(시작 키 = 끝).
    int                     g_watchStage = 0;
    // 지켜보기 기록이 든 세이브가 시험의 요청 없이 불려 왔다(게임을 다시 켜고 불렀거나 메뉴에서 불렀다). 불러오기가 끝나면 판정을 남긴다.
    bool                    g_watchColdLoad = false;
    std::atomic<Clock::rep> g_afterLoadVerdictAt{ 0 };  // 그 판정을 남길 시각. 0이면 없음. 진행을 깨우는 스레드가 읽는다
    std::uint32_t           g_afterLoadRevert = 0;      // 그 판정을 맡길 때의 되돌리기 횟수. 그 뒤에 다른 불러오기가 시작됐으면 판정을 버린다
    // 지켜보기 기록을 코세이브에 쓴 횟수와, 저장 왕복을 시작할 때의 그 값. 저장 왕복의 불러오기 단계가 "방금 저장이 실제로 됐는가"를
    // 이것으로 본다. 저장이 안 됐는데 불러오면 예전 실행이 남긴 같은 이름의 세이브가 불려 와 통과처럼 보인다.
    std::uint32_t           g_watchSaveCount = 0;
    std::uint32_t           g_roundTripSaveBase = 0;
    std::size_t             g_step = 0;
    Clock::time_point       g_nextAt{};
    RE::ObjectRefHandle     g_chest;
    RE::FormID              g_chestId = 0;
    RE::FormID              g_corpseId = 0;                 // 지켜보기 시험에서 놓은 시체
    std::atomic<bool>       g_watchVerdictQueued{ false };  // 지켜보기의 판정 작업을 이미 맡겼다
    // 아래 상태(키, 보고 있는 줍기 등)를 지키는 잠금. 시험의 단계, 이벤트 수신부, 줍기 함수, 코세이브 콜백이 모두 이것을 잡는다.
    // 6차 시험에서 알게 된 것: SKSE에 맡긴 작업이 도는 스레드는 하나가 아니다(단계마다 달랐다). 그래서 "메인 스레드에서 온
    // 이벤트만 쓴다"는 식으로는 거를 수 없다(6차는 그렇게 걸러서 이벤트 대부분을 버렸다). 스레드를 따지지 않고 잠근다.
    // 단계가 부른 엔진 함수 안에서 이벤트가 같은 스레드로 돌아올 것으로 보므로 여러 번 잡을 수 있는 잠금을 쓴다.
    // 이벤트 수신부와 줍기 함수는 기다리지 않고 잡아 본다. 엔진이 제 잠금을 쥔 채 우리를 부르는 자리라 거기서 기다리면
    // 서로 물릴 수 있다. 못 잡아도 이벤트는 처리한다(버리지 않는다). 그 밖의 자리는 잠깐 기다려 본다(LockState).
    std::recursive_timed_mutex g_state;
    bool                    g_hookInstalled = false;
    Pickup                  g_pick;
    // 시험 단계가 아이템을 내려놓는 함수를 부르고 있는 동안 참. 그 밖에서 키가 바닥으로 나가면 손으로 버린 것이다.
    bool                    g_inTestDrop = false;
    // 표시마다: 지금 내려놓는 것에 고유 ID 변경 이벤트 안에서 번호를 남기라는 요청.
    std::array<bool, static_cast<std::size_t>(Mark::kCount)> g_stampInEvent{};
    // 표시마다: 내용(이름이나 꼬리표)으로 가려낼 수 있는가. 지켜보기 시험의 판정이 쓴다. 꼬리표나 이름표가 있는 셋은 처음부터
    // 참이고, 투구는 이름이 실제로 붙었을 때만 참이 된다. 이름이 붙지 않았는데 참으로 두면 멀쩡한 키가 "틀림"으로 찍힌다.
    std::array<bool, static_cast<std::size_t>(Mark::kCount)> g_hasContent{};
    std::array<Key, static_cast<std::size_t>(Mark::kCount)> g_keys;  // 표시마다 따라가고 있는 키
    // 표시를 붙일 때의 부가 데이터 묶음(주소만 견준다). 컨테이너 사이를 옮겨도 같은 묶음이 옮겨 다니므로,
    // 키로 찾은 것이 처음 표시한 그 아이템인지를 키와 따로 확인하는 데 쓴다. 불러오면 묶음이 새로 만들어져 쓸 수 없다.
    std::array<const RE::ExtraDataList*, static_cast<std::size_t>(Mark::kCount)> g_lists{};
    // 시험 아이템을 넣기 전부터 플레이어가 가지고 있던 묶음들. 시험이 건드리면 안 되는 것들이다.
    std::vector<const RE::ExtraDataList*> g_ownLists;
    // 표시를 붙이려고 바닥에 내려놓은 아이템들(표시 번호 순).
    std::array<RE::ObjectRefHandle, static_cast<std::size_t>(Mark::kCount)> g_markRefs;
    std::vector<SavedMark>  g_loadedMarks;      // 방금 불러온 코세이브에 들어 있던 기록
    bool                    g_cosaveLoaded = false;
    std::uint32_t           g_revertCount = 0;

    // 그 로그에 시험이 돈 흔적(단계 줄)이 있는가.
    bool HasTestRun(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }
        std::string line;
        while (std::getline(file, line)) {
            if (line.find(kStepMarker) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

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
        const auto path = *directory / "PoEProtoItems.log";
        // 시험 결과가 든 지난 로그는 게임을 다시 켜도 남긴다(최근 세 개). 시험을 돌리지 않은 실행의 로그는 남기지 않는다.
        // 3차 시험의 로그가 읽는 도중에 다음 실행으로 지워진 적이 있다.
        if (HasTestRun(path)) {
            const auto previous = [&](int index) {
                return *directory / (index == 1 ? std::string("PoEProtoItems.previous.log") : std::format("PoEProtoItems.previous.{}.log", index));
            };
            for (int index = kKeptLogs; index >= 1; --index) {
                const auto from = index == 1 ? path : previous(index - 1);
                std::filesystem::rename(from, previous(index), error);  // 없으면 실패하고 넘어간다
            }
        }
        try {
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
            auto logger = std::make_shared<spdlog::logger>("poe_proto_items", std::move(sink));
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
            if (key == "StartKey") {
                std::uint32_t code = 0;
                const bool hex = value.starts_with("0x") || value.starts_with("0X");
                const auto digits = hex ? value.substr(2) : value;
                const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
                if (result.ec == std::errc{} && result.ptr == digits.data() + digits.size() && code >= 1 && code <= 255) {
                    settings.startKey = code;
                } else {
                    SKSE::log::warn("설정: StartKey '{}'는 1~255의 키 코드가 아니어서 기본값 유지", value);
                }
            } else if (key == "SaveLoad") {
                if (value == "true" || value == "1") {
                    settings.saveLoad = true;
                } else if (value == "false" || value == "0") {
                    settings.saveLoad = false;
                } else {
                    SKSE::log::warn("설정: SaveLoad '{}'는 true 또는 false가 아니어서 기본값 유지", value);
                }
            } else if (key == "HookPickUp") {
                if (value == "true" || value == "1") {
                    settings.hookPickUp = true;
                } else if (value == "false" || value == "0") {
                    settings.hookPickUp = false;
                } else {
                    SKSE::log::warn("설정: HookPickUp '{}'는 true 또는 false가 아니어서 기본값 유지", value);
                }
            } else if (key == "Scenario") {
                if (value == "watch") {
                    settings.watch = true;
                } else if (value == "auto") {
                    settings.watch = false;
                } else {
                    SKSE::log::warn("설정: Scenario '{}'는 auto 또는 watch가 아니어서 기본값(auto) 유지", value);
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

    void Notify(const std::string& message)
    {
        Notify(message.c_str());
    }

    // 화면 안내에 적는 시작 키의 이름. INI의 StartKey를 바꾸면 안내도 따라간다.
    std::string StartKeyName()
    {
        return g_settings.startKey == 0x44 ? std::string("F10") : std::format("시작 키(스캔 코드 0x{:02X})", g_settings.startKey);
    }

    RE::TESBoundObject* TestForm(RE::FormID id)
    {
        return RE::TESForm::LookupByID<RE::TESBoundObject>(id);
    }

    bool IsTestForm(RE::FormID id)
    {
        return id == kIronSword || id == kIronHelmet || id == kIronIngot;
    }

    // 지금 스레드의 번호. 로그에 남겨, 단계와 이벤트와 줍기 함수가 어느 스레드에서 도는지를 본다.
    unsigned long Tid()
    {
        return ::GetCurrentThreadId();
    }

    using StateLock = std::unique_lock<std::recursive_timed_mutex>;

    // 상태 잠금을 잡는다. 어디서도 끝없이 기다리지 않는다: 정해진 시간 안에 못 잡으면 그 사실을 로그에 남기고 잠그지 않은 채
    // 돌려준다. 그때 일을 계속할지는 부르는 쪽이 정한다(이벤트는 계속 처리한다. 5차까지는 잠금 없이 그렇게 돌았다).
    // 이렇게 하는 까닭: 어느 스레드가 무엇을 부르는지를 다 알지 못한다. 6차 시험은 "이벤트는 메인 스레드에서 온다"는 확인하지 않은
    // 전제로 이벤트를 걸렀다가 대부분을 버렸다. 전제가 틀려도 시험이 돌아가고, 틀렸다는 사실이 로그에 남게 한다.
    StateLock LockState(const char* what, std::chrono::milliseconds wait)
    {
        StateLock lock(g_state, wait);
        if (!lock.owns_lock()) {
            SKSE::log::warn("[잠금] {}: {}ms 안에 상태 잠금을 잡지 못함. 잠그지 않고 진행 (스레드 {})", what, wait.count(), Tid());
        }
        return lock;
    }

    // 지켜볼 것이 있는가: 시험이 도는 중이거나, 따라가는 키가 하나라도 살아 있다. 시험이 끝난 뒤에도 표시한 세이브에 머무는
    // 동안에는 키가 살아 있어, 손으로 버리고 줍는 것도 로그에 남는다. g_state를 잡은 채로 부른다.
    bool Observing()
    {
        return g_running || std::any_of(g_keys.begin(), g_keys.end(), [](const Key& key) { return key.valid; });
    }

    RE::TESObjectREFR* Chest()
    {
        if (auto ref = g_chest.get()) {
            return ref.get();
        }
        // 불러온 뒤에는 핸들이 달라졌을 수 있으므로 FormID로 다시 찾는다.
        if (g_chestId != 0) {
            if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(g_chestId)) {
                g_chest = ref->GetHandle();
                return ref;
            }
        }
        return nullptr;
    }

    // 읽기만 한다. GetInventoryChanges()를 인자 없이 부르면 인벤토리 변경 데이터가 없는 참조에 그 자리에서 만들어 붙인다
    // (CommonLibSSE-NG의 구현: InitInventoryIfRequired, ForceInitInventoryChanges. 레벨드 아이템까지 굴린다). 지켜보기의 판정은
    // 상인의 상자나 NPC처럼 남의 참조를 들여다보므로, 보는 것만으로 그쪽 상태를 바꾸지 않게 "없으면 만들지 않음"으로 부른다.
    RE::InventoryEntryData* FindEntry(RE::TESObjectREFR* owner, RE::TESBoundObject* object)
    {
        auto* changes = owner ? owner->GetInventoryChanges(true) : nullptr;
        if (!changes || !changes->entryList) {
            return nullptr;
        }
        for (auto* entry : *changes->entryList) {
            // (GetObject()는 Windows 헤더의 매크로와 이름이 겹쳐 쓰지 않는다.)
            if (entry && entry->object == object) {
                return entry;
            }
        }
        return nullptr;
    }

    // owner가 가진 object의 개수: 베이스 컨테이너에 든 수에 인벤토리 변경분을 더한다.
    // 엔진의 InventoryChanges::GetItemCount는 쓰지 않는다. 지금 쓰는 CommonLibSSE-NG 9.0.2에는 그 함수의 AE 주소 번호가 틀려 있어
    // (16047, 맞는 번호는 12.1.0 기준 16108) 엉뚱한 함수가 불리고, 인벤토리 변경 데이터의 앞부분을 덮어쓴다(첫 시험의 크래시).
    int TotalCount(RE::TESObjectREFR* owner, RE::TESBoundObject* object)
    {
        int total = 0;
        if (const auto* container = owner ? owner->GetContainer() : nullptr) {
            container->ForEachContainerObject([&](RE::ContainerObject& entry) {
                if (entry.obj == object) {
                    total += entry.count;
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        }
        if (const auto* entry = FindEntry(owner, object)) {
            total += entry->countDelta;
        }
        return total;
    }

    float HealthOf(const RE::ExtraDataList& extra)
    {
        const auto* health = extra.GetByType<RE::ExtraHealth>();
        return health ? health->health : 0.0f;
    }

    bool HasTag(const RE::ExtraDataList& extra, float tag)
    {
        return std::abs(HealthOf(extra) - tag) < 0.00004f;
    }

    // 표시 이름이 그 글자로 시작하는가. 같은지가 아니라 시작하는지를 본다: 단련하면 게임이 이름 뒤에 품질 글자를 붙인다.
    bool NameStartsWith(const RE::ExtraDataList& extra, std::string_view prefix)
    {
        const auto* text = extra.GetByType<RE::ExtraTextDisplayData>();
        return text && text->displayName.c_str() && std::string_view(text->displayName.c_str()).starts_with(prefix);
    }

    bool HasNameTag(const RE::ExtraDataList& extra)
    {
        return NameStartsWith(extra, kNameTag);
    }

    // 지켜보기 시험에서 표시마다 붙이는 눈에 보이는 이름. 메뉴에서 사용자가 골라낼 수 있어야 하고, 꼬리표(단련 수치)가
    // 단련으로 바뀌어도 이름으로 가려낼 수 있어야 한다. 없는 표시는 고유 ID로만 따라간다.
    const char* WatchNameOf(Mark mark)
    {
        switch (mark) {
        case Mark::kSwordUidTagA:
            return "PoE5 A";
        case Mark::kSwordUidTagB:
            return "PoE5 B";
        case Mark::kSwordName:
            return kNameTag;
        case Mark::kHelmetUid:
            return "PoE5 helmet";
        default:
            return nullptr;
        }
    }

    // 부가 데이터 한 묶음을 한 줄로.
    std::string Describe(RE::ExtraDataList& extra)
    {
        std::string out = std::format("{}개", extra.GetCount());
        if (const auto* unique = extra.GetByType<RE::ExtraUniqueID>()) {
            out += std::format(" 고유ID={:08X}:{:04X}", unique->baseID, unique->uniqueID);
        } else {
            out += " 고유ID=없음";
        }
        if (const auto* text = extra.GetByType<RE::ExtraTextDisplayData>()) {
            out += std::format(" 이름='{}'", text->displayName.c_str() ? text->displayName.c_str() : "");
        }
        if (const auto* health = extra.GetByType<RE::ExtraHealth>()) {
            out += std::format(" 단련={:.4f}", health->health);
        }
        if (extra.HasType(RE::ExtraDataType::kEnchantment)) {
            out += " 마법부여";
        }
        if (extra.HasType(RE::ExtraDataType::kWorn) || extra.HasType(RE::ExtraDataType::kWornLeft)) {
            out += " 장착중";
        }
        out += std::format(" @{:X}", reinterpret_cast<std::uintptr_t>(&extra) & 0xFFFFF);
        return out;
    }

    // owner의 인벤토리에서 시험용 폼들의 상태를 모두 남긴다.
    void Snapshot(const char* label, RE::TESObjectREFR* owner, const char* who)
    {
        if (!owner) {
            SKSE::log::info("[상태:{}] {}: 없음", label, who);
            return;
        }
        for (const auto id : { kIronSword, kIronHelmet, kIronIngot }) {
            auto* object = TestForm(id);
            if (!object) {
                continue;
            }
            const int total = TotalCount(owner, object);
            auto* entry = FindEntry(owner, object);
            int   withExtra = 0;
            int   lists = 0;
            if (entry && entry->extraLists) {
                for (auto* extra : *entry->extraLists) {
                    if (extra) {
                        withExtra += extra->GetCount();
                        ++lists;
                    }
                }
            }
            if (total == 0 && lists == 0) {
                continue;
            }
            SKSE::log::info("[상태:{}] {} {:08X}: 합계 {}개, 부가 데이터 묶음 {}개({}개 분량), 묶음 없는 것 {}개", label, who, id, total, lists,
                withExtra, total - withExtra);
            if (entry && entry->extraLists) {
                for (auto* extra : *entry->extraLists) {
                    if (extra) {
                        SKSE::log::info("[상태:{}]     {}", label, Describe(*extra));
                    }
                }
            }
        }
    }

    RE::FormID FormOf(Mark mark)
    {
        switch (mark) {
        case Mark::kHelmetUid:
            return kIronHelmet;
        case Mark::kIngotUid:
            return kIronIngot;
        default:
            return kIronSword;
        }
    }

    // 이 묶음이 그 표시의 아이템인가. 표시가 사라졌으면 찾지 못한다(그것이 알고 싶은 사실이다).
    // 꼬리표나 이름이 있는 표시만 내용으로 가려낸다. 고유 ID만 붙인 표시는 여기서 가려낼 수 없다
    // (같은 종류의 다른 아이템도 고유 ID를 가지므로 2차 시험에서 엉뚱한 아이템을 가리켰다). 그런 표시는 FindByKey로 찾는다.
    bool Matches(Mark mark, RE::ExtraDataList& extra)
    {
        // 지켜보기 시험에서 붙인 이름이 있으면 그것으로도 가려낸다(자동 시험에서는 그런 이름이 붙지 않는다).
        if (const char* name = WatchNameOf(mark); name && NameStartsWith(extra, name)) {
            return true;
        }
        switch (mark) {
        case Mark::kSwordUidTagA:
            return HasTag(extra, kTagA);
        case Mark::kSwordUidTagB:
            return HasTag(extra, kTagB);
        case Mark::kSwordName:
            return HasNameTag(extra);
        default:
            return false;
        }
    }

    Found FindIn(Mark mark, RE::TESObjectREFR* owner, const char* where)
    {
        auto* entry = FindEntry(owner, TestForm(FormOf(mark)));
        if (entry && entry->extraLists) {
            for (auto* extra : *entry->extraLists) {
                if (extra && Matches(mark, *extra)) {
                    return { owner, extra, where };
                }
            }
        }
        return {};
    }

    Found Find(Mark mark)
    {
        if (auto found = FindIn(mark, RE::PlayerCharacter::GetSingleton(), "플레이어"); found.extra) {
            return found;
        }
        return FindIn(mark, Chest(), "상자");
    }

    // 고유 ID만 붙인 표시인가. 이런 표시는 내용으로 가려낼 수 없다(같은 종류의 다른 아이템도 고유 ID를 가질 수 있다).
    bool IsUidOnly(Mark mark)
    {
        return mark == Mark::kSwordUid || mark == Mark::kHelmetUid || mark == Mark::kIngotUid;
    }

    // 따라가고 있는 키로 찾는다. 키가 가리키는 쪽이 플레이어도 상자도 아니면(예: 바닥의 참조) 찾지 못한다.
    Found FindByKey(Mark mark)
    {
        const auto& key = g_keys[static_cast<std::size_t>(mark)];
        if (!key.valid) {
            return {};
        }
        auto*              player = RE::PlayerCharacter::GetSingleton();
        RE::TESObjectREFR* owner = nullptr;
        const char*        where = "";
        if (player && key.owner == player->GetFormID()) {
            owner = player;
            where = "플레이어";
        } else if (auto* chest = Chest(); chest && key.owner == chest->GetFormID()) {
            owner = chest;
            where = "상자";
        }
        auto* entry = FindEntry(owner, TestForm(FormOf(mark)));
        if (entry && entry->extraLists) {
            for (auto* extra : *entry->extraLists) {
                const auto* unique = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                if (unique && unique->baseID == key.owner && unique->uniqueID == key.uid) {
                    return { owner, extra, where };
                }
            }
        }
        return {};
    }

    // 표시한 아이템을 찾는다. 꼬리표나 이름이 있는 것은 내용으로(그것이 정답이다), 고유 ID만 있는 것은 따라간 키로.
    Found Locate(Mark mark)
    {
        return IsUidOnly(mark) ? FindByKey(mark) : Find(mark);
    }

    // 지금 그 묶음에 붙어 있는 고유 ID를 그 표시의 키로 삼고, 묶음 자체도 기억해 둔다.
    void RememberKey(Mark mark, RE::ExtraDataList* extra)
    {
        auto&       key = g_keys[static_cast<std::size_t>(mark)];
        const auto* unique = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
        key = unique ? Key{ unique->baseID, unique->uniqueID, true } : Key{};
        g_lists[static_cast<std::size_t>(mark)] = extra;
    }

    std::string DescribeKey(Mark mark)
    {
        const auto& key = g_keys[static_cast<std::size_t>(mark)];
        return key.valid ? std::format("{:08X}:{:04X}{}", key.owner, key.uid, key.world ? "(바닥)" : "") : std::string("없음");
    }

    // 바닥에 있던 참조의 지금 상태. 주워진 참조는 곧 지워지므로, 찾을 수 없거나 삭제·꺼짐 표시가 붙어 있으면 "사라짐"으로 본다.
    struct RefState
    {
        RE::TESObjectREFR* ref = nullptr;
        bool               gone = true;
        std::string        text;
    };

    RefState StateOfRef(RE::FormID id)
    {
        auto* ref = id != 0 ? RE::TESForm::LookupByID<RE::TESObjectREFR>(id) : nullptr;
        if (!ref) {
            return { nullptr, true, "찾을 수 없음" };
        }
        const bool deleted = ref->IsDeleted();
        const bool disabled = ref->IsDisabled();
        return { ref, deleted || disabled,
            deleted && disabled ? "있음(삭제 표시, 꺼짐)" : deleted ? "있음(삭제 표시)" : disabled ? "있음(꺼짐)" : "있음" };
    }

    // 표시한 아이템들이 지금 어디에 어떤 고유 ID로 있는지 한 줄씩 남긴다. 단계 사이를 견주는 표가 된다.
    // 줄 끝의 두 판정:
    //   키   - 이벤트로 따라간 키가 찾은 아이템의 고유 ID와 맞는지. 꼬리표나 이름이 있는 표시에서만 뜻이 있다(내용으로 찾았으므로).
    //          고유 ID만 붙인 표시는 키로 찾으므로 맞을 수밖에 없어 "키로 찾음"이라고만 적는다.
    //   묶음 - 찾은 것이 표시를 붙일 때의 그 부가 데이터 묶음인지. 키와 따로 하는 확인이다. 바닥을 거치거나 불러오면 묶음이
    //          새로 만들어지므로 그때는 "다름"이나 "대조 불가"가 정상이다.
    void Track(const char* label)
    {
        for (std::size_t i = 0; i < kMarkNames.size(); ++i) {
            const auto mark = static_cast<Mark>(i);
            const auto found = Locate(mark);
            if (!found.extra) {
                if (const auto& lost = g_keys[i]; lost.valid && lost.world) {
                    // 인벤토리에는 없고 키가 바닥의 참조를 가리킨다. 그 참조가 아직 있는지와 무엇을 달고 있는지를 남긴다.
                    const auto state = StateOfRef(lost.owner);
                    SKSE::log::info("[추적:{}] {} -> 바닥: 참조 {:08X} {}{} | 키 {}", label, kMarkNames[i], lost.owner, state.text,
                        state.ref ? ", " + Describe(state.ref->extraList) : std::string(), DescribeKey(mark));
                } else {
                    SKSE::log::info("[추적:{}] {} -> 찾지 못함 | 키 {}", label, kMarkNames[i], DescribeKey(mark));
                }
                continue;
            }
            const auto& key = g_keys[i];
            const auto* unique = found.extra->GetByType<RE::ExtraUniqueID>();
            const char* keyVerdict = IsUidOnly(mark) ? "키로 찾음" :
                                     !key.valid       ? "없음" :
                                     (unique && unique->baseID == key.owner && unique->uniqueID == key.uid) ? "맞음" : "어긋남";
            const char* listVerdict = !g_lists[i] ? "대조 불가" : g_lists[i] == found.extra ? "같음" : "다름";
            SKSE::log::info("[추적:{}] {} -> {}: {} | 키 {} {} | 묶음 {}", label, kMarkNames[i], found.where, Describe(*found.extra),
                DescribeKey(mark), keyVerdict, listVerdict);
        }
    }

    void Observe(const char* label)
    {
        Snapshot(label, RE::PlayerCharacter::GetSingleton(), "플레이어");
        Snapshot(label, Chest(), "상자");
        Track(label);
    }

    // ---------------------------------------------------------------- 지켜보기 시험의 판정
    //
    // 지켜보기 시험에서는 사용자가 실제 메뉴로 아이템을 옮긴다(상인, 동료, 시체, 상자, 제작). 플러그인은 결정 59의 방식
    // 그대로 키를 따라가기만 하고, 그 키가 맞는지를 이 판정으로 남긴다.
    // 판정의 물음: 키가 가리키는 자리(가진 쪽, 번호)에 묶음이 있는가, 그리고 그 묶음이 정말 그 표시의 아이템인가.
    // 뒤쪽은 키와 따로 선 확인이다: 표시마다 붙여 둔 이름이나 꼬리표를 그 묶음에서 읽어 견준다. 이름도 꼬리표도 없는 표시
    // (검/고유ID, 주괴/고유ID)는 자리에 묶음이 있는지까지만 알 수 있다.

    // 그 표시를 내용(이름이나 꼬리표)으로 가려낼 수 있는가.
    bool HasContentTruth(Mark mark)
    {
        return g_hasContent[static_cast<std::size_t>(mark)];
    }

    // 키가 가리키는 자리의 묶음. 가진 쪽은 플레이어나 상자뿐 아니라 어떤 참조든 될 수 있다(상인의 상자, 동료, 시체).
    Found FindAtKey(Mark mark)
    {
        const auto& key = g_keys[static_cast<std::size_t>(mark)];
        if (!key.valid || key.world) {
            return {};
        }
        auto* owner = RE::TESForm::LookupByID<RE::TESObjectREFR>(key.owner);
        auto* entry = FindEntry(owner, TestForm(FormOf(mark)));
        if (entry && entry->extraLists) {
            for (auto* extra : *entry->extraLists) {
                const auto* unique = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                if (unique && unique->baseID == key.owner && unique->uniqueID == key.uid) {
                    return { owner, extra, "키의 자리" };
                }
            }
        }
        return { owner, nullptr, "" };
    }

    // 가진 쪽을 사람이 읽을 수 있게: 폼 ID와 베이스의 이름.
    std::string DescribeOwner(RE::FormID id)
    {
        const auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && id == player->GetFormID()) {
            return "플레이어";
        }
        auto*       ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
        const auto* base = ref ? ref->GetBaseObject() : nullptr;
        const char* name = base ? base->GetName() : nullptr;
        return std::format("{:08X}({}{})", id, !ref ? "찾을 수 없음" : name && *name ? name : "이름 없음",
            id == g_chestId ? ", 시험이 놓은 상자" : id == g_corpseId ? ", 시험이 놓은 시체" : "");
    }

    // 키의 자리에 묶음이 없을 때 덧붙이는 말: 가진 쪽이 그 폼을 몇 개 가졌는지와, 그 표시의 것이 다른 고유 ID로 거기 있는지.
    // "아이템이 없어졌다"와 "아이템은 있는데 번호가 달라졌다(키가 낡았다)"와 "인벤토리를 읽을 수 없다"를 가르려는 것이다.
    // 저장 왕복 뒤의 끊김에서 이 구분이 필요하다.
    std::string DescribeBroken(Mark mark, RE::TESObjectREFR* owner)
    {
        auto* form = TestForm(FormOf(mark));
        if (!owner->GetInventoryChanges(true)) {  // true: 없으면 만들지 않는다(FindEntry의 설명)
            return " | 가진 쪽의 인벤토리 변경 데이터가 없음(읽을 수 없는 상태일 수 있음)";
        }
        std::string text = std::format(" | 가진 쪽이 그 폼을 {}개 가짐", TotalCount(owner, form));
        if (!HasContentTruth(mark)) {
            return text;
        }
        const auto byContent = FindIn(mark, owner, "");
        return text + (byContent.extra ? ", 그 표시의 것이 다른 고유 ID로 있음(키가 낡음): " + Describe(*byContent.extra) : ", 그 표시의 것은 없음");
    }

    // 지켜보기 시험이 놓은 스키버. 놓지 않았거나 그 참조를 지금 찾을 수 없으면 없다.
    RE::Actor* CorpseActor()
    {
        auto* ref = g_corpseId != 0 ? RE::TESForm::LookupByID<RE::TESObjectREFR>(g_corpseId) : nullptr;
        return ref ? ref->As<RE::Actor>() : nullptr;
    }

    // 그 스키버의 상태. 준비 단계가 죽이는 데 성공했는지와, 저장 왕복 뒤에도 그 참조가 있는지를 본다.
    void LogCorpse(const char* label)
    {
        if (g_corpseId == 0) {
            SKSE::log::info("[시체:{}] 놓은 스키버가 없음", label);
            return;
        }
        auto* actor = CorpseActor();
        if (!actor) {
            SKSE::log::info("[시체:{}] 스키버 {:08X}: 찾을 수 없음(그 폼 ID의 액터 참조가 지금 없음)", label, g_corpseId);
            return;
        }
        SKSE::log::info("[시체:{}] 스키버 {:08X}: 죽은 상태 {}, 체력 {:.1f}, 3D {}", label, g_corpseId, actor->IsDead(),
            actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth), actor->Is3DLoaded() ? "있음" : "없음");
    }

    void WatchVerdict(const std::string& reason)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (std::size_t i = 0; i < kMarkNames.size(); ++i) {
            const auto  mark = static_cast<Mark>(i);
            const auto& key = g_keys[i];
            if (!key.valid) {
                SKSE::log::info("[경로 판정:{}] {}: 키 없음", reason, kMarkNames[i]);
                continue;
            }
            if (key.world) {
                const auto state = StateOfRef(key.owner);
                SKSE::log::info("[경로 판정:{}] {}: 바닥의 참조 {:08X} {}{} | 키 {}", reason, kMarkNames[i], key.owner, state.text,
                    state.ref ? ", " + Describe(state.ref->extraList) : std::string(), DescribeKey(mark));
                continue;
            }
            const auto found = FindAtKey(mark);
            // 키가 플레이어가 아닌 곳을 가리키는데 플레이어 인벤토리에 내용으로 찾은 것이 있으면 키가 뒤처진 것이다.
            const auto* inPlayer = HasContentTruth(mark) ? FindIn(mark, player, "플레이어").extra : nullptr;
            const char* stale = inPlayer && inPlayer != found.extra ? " | 그런데 플레이어 인벤토리에 내용으로 찾은 그 표시가 따로 있음(키가 뒤처짐)" : "";
            if (!found.owner) {
                // 가진 쪽의 참조 자체를 찾지 못했다. 묶음이 없다는 것과는 다르다(키가 틀렸다고 말할 수 없다).
                SKSE::log::info("[경로 판정:{}] {}: 확인하지 못함 - 키 {} 의 가진 쪽 참조를 찾을 수 없음{}", reason, kMarkNames[i], DescribeKey(mark), stale);
                continue;
            }
            if (!found.extra) {
                SKSE::log::info("[경로 판정:{}] {}: 끊김 - 키 {} 의 자리({})에 묶음이 없음{}{}", reason, kMarkNames[i], DescribeKey(mark),
                    DescribeOwner(key.owner), DescribeBroken(mark, found.owner), stale);
                continue;
            }
            const char* verdict = !HasContentTruth(mark)        ? "키의 자리에 묶음이 있음(내용으로 확인할 수 없는 표시)" :
                                  Matches(mark, *found.extra) ? "맞음 - 키의 자리에 있는 묶음이 그 표시의 것" :
                                                                "틀림 - 키의 자리에 있는 묶음이 그 표시의 것이 아님";
            SKSE::log::info("[경로 판정:{}] {}: {} | {} 에 {} | 키 {}{}", reason, kMarkNames[i], verdict, DescribeOwner(key.owner),
                Describe(*found.extra), DescribeKey(mark), stale);
        }
    }

    // 판정을 다음 작업으로 맡긴다. 이벤트는 엔진이 아이템을 옮기는 도중에 오므로, 다 옮긴 뒤의 상태를 보려는 것이다.
    // 이미 맡긴 것이 있으면 더 쌓지 않는다(한 번의 조작에 이벤트가 여럿 온다).
    void QueueWatchVerdict(std::string reason)
    {
        if (g_watchVerdictQueued.exchange(true)) {
            return;
        }
        const auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            g_watchVerdictQueued = false;
            return;
        }
        // 맡긴 뒤에 다른 세이브의 불러오기가 시작되면(되돌리기 횟수가 달라짐) 이 판정은 버린다. 불려 오는 세이브에도 지켜보기
        // 기록이 있으면 단계가 다시 kWatch가 되므로 단계만 봐서는 가를 수 없고, 화면이 넘어가는 중의 인벤토리를 훑게 된다.
        tasks->AddTask([reason = std::move(reason), revert = g_revertCount] {
            g_watchVerdictQueued = false;
            const auto lock = LockState("지켜보기 판정", std::chrono::seconds(5));
            if (g_phase == Phase::kWatch && revert == g_revertCount) {
                WatchVerdict(reason);
            }
        });
    }

    // ---------------------------------------------------------------- 단계

    void StepSetup()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* chestBase = TestForm(kEmptyChest);
        if (auto chest = chestBase ? player->PlaceObjectAtMe(chestBase, true) : nullptr) {
            g_chest = chest->GetHandle();
            g_chestId = chest->GetFormID();
            SKSE::log::info("상자를 놓음: {:08X}", g_chestId);
        } else {
            SKSE::log::error("상자를 놓지 못함. 상자로 옮기는 단계는 건너뛰게 됩니다.");
        }
        Observe("0 시작(시험 아이템을 넣기 전)");
        // 플레이어가 원래 가진 것(단련한 철 검, 쓰고 있는 철 투구 등)을 적어 둔다. 표시할 것을 고를 때 이것들은 건드리지 않는다.
        g_ownLists.clear();
        for (const auto id : { kIronSword, kIronHelmet, kIronIngot }) {
            if (const auto* entry = FindEntry(player, TestForm(id)); entry && entry->extraLists) {
                for (const auto* extra : *entry->extraLists) {
                    if (extra) {
                        g_ownLists.push_back(extra);
                    }
                }
            }
        }
        player->AddObjectToContainer(TestForm(kIronSword), nullptr, 5, nullptr);
        player->AddObjectToContainer(TestForm(kIronHelmet), nullptr, 2, nullptr);
        player->AddObjectToContainer(TestForm(kIronIngot), nullptr, 2, nullptr);
        Observe("1 시험 아이템을 넣음(검 5, 투구 2, 주괴 2)");
    }

    std::string TempName(Mark mark)
    {
        return std::string(kTempNamePrefix) + std::to_string(static_cast<int>(mark));
    }

    // 시험이 새로 넣은 아이템 가운데 하나를 고른다. 묶음이 있는 새 것이 있으면 그 묶음을, 없고 묶음 없는 것이 남아 있으면
    // nullptr(묶음 없는 것)를 돌려준다. 새 것이 하나도 없으면 ok가 false다. 플레이어가 원래 가진 묶음은 고르지 않는다.
    RE::ExtraDataList* PickFresh(RE::TESObjectREFR* owner, RE::TESBoundObject* object, bool& ok)
    {
        int   withExtra = 0;
        auto* entry = FindEntry(owner, object);
        if (entry && entry->extraLists) {
            for (auto* extra : *entry->extraLists) {
                if (!extra) {
                    continue;
                }
                withExtra += extra->GetCount();
                if (std::find(g_ownLists.begin(), g_ownLists.end(), extra) == g_ownLists.end()) {
                    ok = true;
                    return extra;
                }
            }
        }
        ok = TotalCount(owner, object) - withExtra > 0;
        return nullptr;
    }

    // 인벤토리 안의 아이템에 부가 데이터 묶음을 직접 만들어 붙이는 길은 쓰지 않는다(이 CommonLib에는 묶음을 새로 만드는 함수가 없다).
    // 대신 하나를 바닥에 내려놓고, 그 참조가 가진 묶음에 표시를 붙인 뒤 다시 줍는다. 묶음은 게임이 만든 것을 쓴다.
    // 어느 것을 내려놓을지는 직접 고른다. 엔진에 맡기면 플레이어가 원래 가진 아이템이 골라질 수 있다.
    void StepMarkDrop()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (std::size_t i = 0; i < g_markRefs.size(); ++i) {
            auto* object = TestForm(FormOf(static_cast<Mark>(i)));
            bool  ok = false;
            auto* extra = PickFresh(player, object, ok);
            if (!ok) {
                g_markRefs[i] = RE::ObjectRefHandle();
                SKSE::log::error("표시: {} 에 쓸 새 아이템이 인벤토리에 없어 내려놓지 않음", kMarkNames[i]);
                continue;
            }
            g_markRefs[i] = player->RemoveItem(object, 1, RE::ITEM_REMOVE_REASON::kDropping, extra, nullptr);
            if (!g_markRefs[i]) {
                SKSE::log::error("표시: {} 을(를) 내려놓지 못함", kMarkNames[i]);
            }
        }
        Observe("2a 표시할 것을 바닥에 내려놓음");
    }

    void StepMarkTagAndPickUp()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (std::size_t i = 0; i < g_markRefs.size(); ++i) {
            const auto mark = static_cast<Mark>(i);
            auto       ref = g_markRefs[i].get();
            g_markRefs[i] = RE::ObjectRefHandle();
            if (!ref) {
                SKSE::log::error("표시: {} 의 바닥 참조가 없음", kMarkNames[i]);
                continue;
            }
            if (mark == Mark::kSwordUidTagA) {
                ref->extraList.Add(new RE::ExtraHealth(kTagA));
            } else if (mark == Mark::kSwordUidTagB) {
                ref->extraList.Add(new RE::ExtraHealth(kTagB));
            } else if (mark == Mark::kSwordName) {
                ref->extraList.Add(new RE::ExtraTextDisplayData(kNameTag));
            } else {
                ref->extraList.Add(new RE::ExtraTextDisplayData(TempName(mark).c_str()));
            }
            SKSE::log::info("[바닥] {} 참조 {:08X}: {}", kMarkNames[i], ref->GetFormID(), Describe(ref->extraList));
            player->PickUpObject(ref.get(), 1, false, false);
        }
        Observe("2b 꼬리표를 붙여 다시 주움");
    }

    // 임시 이름으로 주운 묶음을 찾는다.
    RE::ExtraDataList* FindByName(RE::TESObjectREFR* owner, RE::TESBoundObject* object, const std::string& name)
    {
        auto* entry = FindEntry(owner, object);
        if (entry && entry->extraLists) {
            for (auto* extra : *entry->extraLists) {
                const auto* text = extra ? extra->GetByType<RE::ExtraTextDisplayData>() : nullptr;
                if (text && text->displayName.c_str() && name == text->displayName.c_str()) {
                    return extra;
                }
            }
        }
        return nullptr;
    }

    // owner의 인벤토리에서 아직 쓰이지 않은 고유 ID 번호를 고른다.
    std::uint16_t NextFreeUniqueId(RE::TESObjectREFR* owner)
    {
        std::uint16_t highest = 0x7000;  // 엔진이 매기는 작은 번호들과 겹치지 않게 높은 데서 시작한다
        auto*         changes = owner->GetInventoryChanges();
        if (changes && changes->entryList) {
            for (auto* entry : *changes->entryList) {
                if (!entry || !entry->extraLists) {
                    continue;
                }
                for (auto* extra : *entry->extraLists) {
                    const auto* unique = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                    if (unique && unique->uniqueID > highest && unique->uniqueID < 0xFFFF) {
                        highest = unique->uniqueID;
                    }
                }
            }
        }
        return static_cast<std::uint16_t>(highest + 1);
    }

    // 고유 ID(ExtraUniqueID)가 없으면 직접 만들어 붙인다. 이미 붙어 있으면 그대로 둔다(둘을 붙이면 묶음 안에 고유 ID가 두 개가 된다).
    // 값은 엔진이 쓰는 모양을 따른다: 앞은 가진 쪽의 폼 ID, 뒤는 그 인벤토리에서 안 쓰인 번호(2차 시험에서 본 모양).
    // 엔진의 InventoryChanges::SetUniqueID는 쓰지 않는다. 지금 쓰는 CommonLibSSE-NG 9.0.2와 12.1.0이 그 함수의 AE 주소 번호를
    // 다르게 적고 있어(16149 / 16147) 어느 쪽도 실행으로 확인하기 전에는 부를 수 없다.
    void MarkUnique(Mark mark, RE::TESObjectREFR* owner, RE::ExtraDataList* extra, const char* name)
    {
        if (!extra) {
            SKSE::log::error("고유 ID: {} 의 묶음을 찾지 못해 붙이지 못함", name);
            return;
        }
        if (extra->HasType<RE::ExtraUniqueID>()) {
            SKSE::log::info("고유 ID: {} -> 이미 붙어 있어 그대로 둠: {}", name, Describe(*extra));
        } else {
            extra->Add(new RE::ExtraUniqueID(owner->GetFormID(), NextFreeUniqueId(owner)));
            SKSE::log::info("고유 ID: {} -> 직접 만들어 붙임: {}", name, Describe(*extra));
        }
        RememberKey(mark, extra);
    }

    void StepMarkUnique()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (std::size_t i = 0; i < kMarkNames.size(); ++i) {
            const auto mark = static_cast<Mark>(i);
            auto*      object = TestForm(FormOf(mark));
            if (mark == Mark::kSwordName) {
                // 이름만 붙이는 아이템에는 고유 ID를 붙이지 않는다. 다른 것이 이미 붙였으면 그것을 따라간다.
                RememberKey(mark, FindIn(mark, player, "플레이어").extra);
                continue;
            }
            if (IsUidOnly(mark)) {
                auto* extra = FindByName(player, object, TempName(mark));
                MarkUnique(mark, player, extra, kMarkNames[i]);
                // 고유 ID가 붙었을 때만 임시 이름을 뗀다. 안 붙었는데 떼면 표시 없는 것들과 다시 합쳐진다.
                if (extra && extra->HasType<RE::ExtraUniqueID>()) {
                    extra->RemoveByType(RE::ExtraDataType::kTextDisplayData);
                }
            } else {
                const auto found = FindIn(mark, player, "플레이어");
                MarkUnique(mark, player, found.extra, kMarkNames[i]);
            }
        }
        Observe("2 표시를 붙임(나누기)");
    }

    void StepAddPlain()
    {
        RE::PlayerCharacter::GetSingleton()->AddObjectToContainer(TestForm(kIronSword), nullptr, 1, nullptr);
        Observe("3 표시 없는 검 하나를 더 넣음(합치기)");
    }

    // 표시한 아이템을 모두 from에서 to로 옮긴다.
    void MoveMarked(RE::TESObjectREFR* from, RE::TESObjectREFR* to, const char* fromName, RE::ITEM_REMOVE_REASON reason)
    {
        if (!from || !to) {
            SKSE::log::error("옮길 곳이 없어 건너뜀");
            return;
        }
        for (std::size_t i = 0; i < kMarkNames.size(); ++i) {
            const auto mark = static_cast<Mark>(i);
            const auto found = Locate(mark);
            if (!found.extra || found.owner != from) {
                SKSE::log::info("옮기기: {}에서 {} 을(를) 찾지 못해 건너뜀", fromName, kMarkNames[i]);
                continue;
            }
            from->RemoveItem(TestForm(FormOf(mark)), 1, reason, found.extra, to);
        }
    }

    void StepToChest()
    {
        MoveMarked(RE::PlayerCharacter::GetSingleton(), Chest(), "플레이어", RE::ITEM_REMOVE_REASON::kStoreInContainer);
        Observe("4 표시한 것을 상자로 옮김");
    }

    void StepFromChest()
    {
        MoveMarked(Chest(), RE::PlayerCharacter::GetSingleton(), "상자", RE::ITEM_REMOVE_REASON::kRemove);
        Observe("5 상자에서 다시 꺼냄");
    }

    // ---------------------------------------------------------------- 바닥을 거친 아이템 잇기
    //
    // 줍기 하나에 대해 두 가지를 답해야 한다. (1) 따라가던 표시 가운데 어느 것이 돌아왔는가. (2) 인벤토리의 어느 묶음이 그것인가.
    // (1)의 방법: 컨테이너 이벤트에 실린 참조 / 따라가던 바닥 참조 가운데 사라진 것 / 줍기 함수에 끼어들어 본 참조 /
    //             바닥의 참조에 남겨 둔 번호를 엔진이 고유 ID 변경 이벤트로 알려 주는가.
    // (2)의 방법: CalamityAffixes가 쓰는 묶음 규칙(v2.3.2의 ChooseReturnedItem) / 줍기 함수의 앞뒤로 묶음을 견준 차이.
    // 방법마다의 답은 g_pick에 모으고, 시험 단계가 시작한 줍기라면 다음 단계에서 정답과 견준다.
    // 잇는 절차(키를 실제로 옮기는 것)는 본 모드가 하게 될 순서를 따른다: 줍기 함수에 끼어들 수 있으면 그것으로,
    // 없으면 이벤트의 참조(없으면 사라진 참조)와 묶음 규칙으로.

    struct FloorPlan
    {
        Mark     mark;
        DropPath drop;
        Stamp    stamp;  // 내려놓은 참조에 고유 ID를 직접 남기는가, 언제 남기는가("번호 남기기")
        PickPath pick;
    };

    // 내려놓는 순서. 같은 폼(철 검) 셋을 함께 바닥에 두어, 참조를 모르면 가려낼 수 없는 상황을 만든다.
    // 8차부터 주괴에도 번호를 남긴다. 7차에서 번호 없이 내려놓은 주괴는 묶음 없이 돌아와 표시를 잃었다.
    constexpr std::array<FloorPlan, 5> kFloorPlan{ {
        { Mark::kSwordUidTagA, DropPath::kRemoveItem, Stamp::kNone, PickPath::kPickUpObject },
        { Mark::kSwordUidTagB, DropPath::kDropObject, Stamp::kAfterDrop, PickPath::kActivate },
        { Mark::kSwordUid, DropPath::kDropObject, Stamp::kNone, PickPath::kActivate },
        { Mark::kIngotUid, DropPath::kRemoveItem, Stamp::kAfterDrop, PickPath::kPickUpObject },
        { Mark::kHelmetUid, DropPath::kRemoveItem, Stamp::kAfterDrop, PickPath::kPickUpObject },
    } };
    // 줍는 순서(kFloorPlan의 자리). 내려놓은 순서와 다르게 하고, 번호를 남긴 둘은 끝에 둔다(엔진이 어떻게 받을지 모르는 것을 뒤로).
    constexpr std::array<std::size_t, 5> kPickOrder{ 2, 0, 3, 4, 1 };

    const char* NameOf(DropPath path)
    {
        return path == DropPath::kDropObject ? "DropObject" : "RemoveItem(버리기 이유)";
    }

    const char* NameOf(PickPath path)
    {
        return path == PickPath::kActivate ? "활성화(ActivateRef)" : "PickUpObject";
    }

    const char* MarkAnswer(int mark)
    {
        if (mark >= 0 && static_cast<std::size_t>(mark) < kMarkNames.size()) {
            return kMarkNames[static_cast<std::size_t>(mark)];
        }
        return mark == kAmbiguous ? "후보가 여럿이라 고르지 않음" : "답 없음";
    }

    std::vector<RE::ExtraDataList*> ListsOf(RE::TESObjectREFR* owner, RE::TESBoundObject* object)
    {
        std::vector<RE::ExtraDataList*> lists;
        if (const auto* entry = FindEntry(owner, object); entry && entry->extraLists) {
            for (auto* extra : *entry->extraLists) {
                if (extra) {
                    lists.push_back(extra);
                }
            }
        }
        return lists;
    }

    // 그 폼의 표시 가운데 바닥에 있는 것이 있는가.
    bool HasGroundMark(RE::FormID base)
    {
        for (std::size_t i = 0; i < g_keys.size(); ++i) {
            if (g_keys[i].valid && g_keys[i].world && FormOf(static_cast<Mark>(i)) == base) {
                return true;
            }
        }
        return false;
    }

    // 줍기 하나를 보기 시작한다. 줍기 전의 상태(플레이어의 묶음들, 바닥에 있던 표시들)를 적어 둔다.
    void ResetPickup(RE::FormID base, bool automated, int expected)
    {
        const auto generation = g_pick.generation + 1;
        g_pick = Pickup{};
        g_pick.generation = generation;
        g_pick.automated = automated;
        g_pick.expected = expected;
        g_pick.base = base;
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* object = TestForm(base);
        g_pick.before = ListsOf(player, object);
        g_pick.countBefore = TotalCount(player, object);
        for (std::size_t i = 0; i < g_keys.size(); ++i) {
            if (g_keys[i].valid && g_keys[i].world && FormOf(static_cast<Mark>(i)) == base) {
                g_pick.ground.emplace_back(static_cast<int>(i), g_keys[i].owner);
            }
        }
    }

    // 줍기 전에 바닥에 있던 표시 가운데 그 참조의 것.
    int GroundMark(RE::FormID refId)
    {
        for (const auto& [mark, ref] : g_pick.ground) {
            if (ref == refId) {
                return mark;
            }
        }
        return kNoAnswer;
    }

    // 줍기 전에는 없던 묶음들.
    std::vector<RE::ExtraDataList*> FreshLists(RE::TESObjectREFR* owner, RE::TESBoundObject* object)
    {
        auto lists = ListsOf(owner, object);
        std::erase_if(lists, [](const RE::ExtraDataList* extra) {
            return std::find(g_pick.before.begin(), g_pick.before.end(), extra) != g_pick.before.end();
        });
        return lists;
    }

    // 방법 "사라진 참조": 줍기 전에 바닥에 있던 표시들의 참조 가운데 지금 사라진 것이 하나뿐이면 그것이 방금 주워진 것이다.
    int MarkByGoneRef(const char* when)
    {
        int answer = kNoAnswer;
        for (const auto& [mark, ref] : g_pick.ground) {
            const auto state = StateOfRef(ref);
            SKSE::log::info("[잇기:{}] 바닥에 있던 {} 의 참조 {:08X}: {}", when, MarkAnswer(mark), ref, state.text);
            if (state.gone) {
                answer = answer == kNoAnswer ? mark : kAmbiguous;
            }
        }
        return answer;
    }

    // 방법 "묶음 규칙": CalamityAffixes가 돌아온 아이템을 고르는 규칙을 그대로 옮긴 것(v2.3.2, ChooseReturnedItem).
    // 고유 ID의 앞 값이 바닥의 참조를 가리키는 묶음이 하나면 그것, 그런 것이 없고 고유 ID가 없는 묶음이 하나뿐이면 그것,
    // 그 밖에는 고르지 않는다. (그쪽은 고른 참조 하나만 보지만 여기서는 따라가던 바닥 참조 모두를 본다.)
    // 그 순간 플레이어가 가진 묶음들도 함께 남긴다. 주운 아이템이 언제 인벤토리에 들어오고 언제 번호를 받는지를 보려는 것이다.
    ListClaim ChooseReturned(const char* method, const char* when)
    {
        RE::ExtraDataList* exact = nullptr;
        RE::ExtraDataList* loose = nullptr;
        int                exactCount = 0;
        int                looseCount = 0;
        for (auto* extra : ListsOf(RE::PlayerCharacter::GetSingleton(), TestForm(g_pick.base))) {
            const bool  fresh = std::find(g_pick.before.begin(), g_pick.before.end(), extra) == g_pick.before.end();
            const auto* unique = extra->GetByType<RE::ExtraUniqueID>();
            const bool  pointsAtGround = unique && std::any_of(g_pick.ground.begin(), g_pick.ground.end(),
                                                       [&](const auto& entry) { return entry.second == unique->baseID; });
            const char* kind = "제 고유 ID가 있음";
            if (pointsAtGround) {
                exact = extra;
                ++exactCount;
                kind = "고유 ID가 바닥의 참조를 가리킴";
            } else if (!unique || unique->uniqueID == 0) {
                loose = extra;
                ++looseCount;
                kind = "고유 ID 없음";
            }
            SKSE::log::info("[잇기:{}] 플레이어의 묶음{}: {} | {}", when, fresh ? "(줍기 전에는 없던 것)" : "", Describe(*extra), kind);
        }
        if (exactCount == 1) {
            return { method, exact, {} };
        }
        if (exactCount == 0 && looseCount == 1) {
            return { method, loose, {} };
        }
        return { method, nullptr, std::format("바닥의 참조를 가리키는 묶음 {}개, 고유 ID 없는 묶음 {}개", exactCount, looseCount) };
    }

    // 돌아온 묶음에 그 표시의 키를 다시 묶는다. 묶음에 고유 ID가 없으면 붙이고, 플레이어의 것이 아닌 값(바닥의 참조에 남겨 둔
    // 번호)이면 플레이어의 빈 번호로 바꿔 쓴다. 이미 플레이어의 번호가 붙어 있으면 그 번호를 키로 삼는다.
    void Rebind(Mark mark, RE::ExtraDataList* extra)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (auto* unique = extra->GetByType<RE::ExtraUniqueID>(); !unique) {
            extra->Add(new RE::ExtraUniqueID(player->GetFormID(), NextFreeUniqueId(player)));
        } else if (unique->baseID != player->GetFormID() || unique->uniqueID == 0) {
            const auto number = NextFreeUniqueId(player);
            unique->baseID = player->GetFormID();
            unique->uniqueID = number;
        }
        RememberKey(mark, extra);
    }

    // 잇는 절차의 마지막: 고른 표시의 키를 고른 묶음으로 옮긴다.
    void Link(const char* by, int mark, RE::ExtraDataList* list)
    {
        if (mark < 0) {
            SKSE::log::info("[잇기] 절차({}): 돌아온 것이 따라가던 표시인지 알 수 없어 잇지 않음({})", by, MarkAnswer(mark));
            return;
        }
        const auto index = static_cast<std::size_t>(mark);
        if (!g_keys[index].valid) {
            SKSE::log::info("[잇기] 절차({}): {} 의 키가 비어 있어 잇지 않음", by, kMarkNames[index]);
            return;
        }
        if (!g_keys[index].world) {
            // 키가 더는 바닥의 참조를 가리키지 않는다. 줍는 동안 엔진이 고유 ID 변경 이벤트를 보내 옮긴 경우가 여기에 든다.
            SKSE::log::info("[잇기] 절차({}): {} 의 키는 이미 바닥의 것이 아님({}). 옮기지 않고 그대로 둠", by, kMarkNames[index],
                DescribeKey(static_cast<Mark>(mark)));
            g_pick.linked = true;
            return;
        }
        if (!list) {
            SKSE::log::info("[잇기] 절차({}): {} 이(가) 돌아왔지만 인벤토리의 어느 묶음인지 고르지 못해 잇지 않음", by, kMarkNames[index]);
            return;
        }
        Rebind(static_cast<Mark>(mark), list);
        g_pick.linked = true;
        SKSE::log::info("[잇기] 절차({}): {} -> {} | 키 {}", by, kMarkNames[index], Describe(*list), DescribeKey(static_cast<Mark>(mark)));
    }

    void OnManualPickupRecorded();

    // 손으로 주운 것(시험 단계가 시작하지 않은 줍기)의 판정. 정답을 아는 단계가 없으므로, 줍기 함수가 본 것(받은 참조와 새로 생긴
    // 묶음)을 기준으로 다른 방법들의 답을 견준다. 꼬리표나 이름이 있는 표시는 내용으로 찾은 묶음과도 견준다(이쪽이 따로 선 확인이다).
    // hookMark가 kNoAnswer이고 hookList가 없으면 줍기 함수를 거치지 않은 줍기다.
    void LogManualVerdict(bool viaHook, int hookMark, const RE::ExtraDataList* hookList)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (viaHook) {
            SKSE::log::info("[손 조작 판정] 줍기 함수가 본 것: {} / {}", MarkAnswer(hookMark),
                hookList ? std::format("@{:X}", reinterpret_cast<std::uintptr_t>(hookList) & 0xFFFFF) : std::string("새 묶음을 하나로 정하지 못함"));
        } else {
            SKSE::log::info("[손 조작 판정] 이 줍기는 줍기 함수를 거치지 않음(컨테이너 이벤트로만 봄)");
        }
        for (const auto& claim : g_pick.marks) {
            SKSE::log::info("[손 조작 판정] 어느 표시인가 | {} -> {}{}", claim.method, MarkAnswer(claim.mark),
                !viaHook || claim.mark < 0 ? "" : claim.mark == hookMark ? " : 줍기 함수와 같음" : " : 줍기 함수와 다름");
        }
        for (const auto& claim : g_pick.lists) {
            SKSE::log::info("[손 조작 판정] 어느 묶음인가 | {} -> {}{}", claim.method,
                claim.list ? std::format("@{:X}", reinterpret_cast<std::uintptr_t>(claim.list) & 0xFFFFF) : claim.why,
                !viaHook || !claim.list || !hookList ? "" : claim.list == hookList ? " : 줍기 함수와 같음" : " : 줍기 함수와 다름");
        }
        // 내용으로 가려낼 수 있는 표시들: 지금 내용으로 찾은 묶음의 고유 ID가 키와 맞는가. 잇는 절차가 맞게 이었는지의 따로 선 확인이다.
        for (const auto mark : { Mark::kSwordUidTagA, Mark::kSwordUidTagB, Mark::kSwordName }) {
            const auto  index = static_cast<std::size_t>(mark);
            const auto* byContent = FindIn(mark, player, "플레이어").extra;
            if (FormOf(mark) != g_pick.base || !byContent) {
                continue;
            }
            const auto* unique = byContent->GetByType<RE::ExtraUniqueID>();
            const auto& key = g_keys[index];
            SKSE::log::info("[손 조작 판정] 내용으로 찾은 {} 의 고유 ID가 키와 {} (키 {}){}", kMarkNames[index],
                unique && key.valid && !key.world && unique->baseID == key.owner && unique->uniqueID == key.uid ? "맞음" : "어긋남",
                DescribeKey(mark), viaHook && hookList ? (byContent == hookList ? " | 줍기 함수의 묶음과 같은 묶음" : " | 줍기 함수의 묶음과 다른 묶음") : "");
        }
    }

    // 줍기 함수가 끝난 뒤(다음 작업)에 한 번 더 본다. 이벤트가 온 순간에는 아이템이 아직 인벤토리에 없거나 번호를 받기 전일 수 있다.
    // 시험 단계가 직접 부른 줍기에서는 이 작업이 같은 프레임 안, 단계가 끝난 직후에 돌 수 있다(4.6의 SKSE 작업 관찰).
    // 이 관찰은 우리가 인벤토리를 고치기 전에 한다. 줍기 함수 쪽 절차(ApplyPendingLink)는 이 작업 뒤에 돌도록 맡겨져 있다.
    void AnalyzePickupLater(std::uint32_t generation)
    {
        if (generation != g_pick.generation || g_pick.laterDone) {
            return;  // 그사이 다른 줍기가 시작됐거나, 이미 봤다
        }
        g_pick.laterDone = true;
        const int  gone = MarkByGoneRef("줍기 뒤");
        g_pick.marks.push_back({ "사라진 참조(줍기 뒤)", gone });
        auto chosen = ChooseReturned("묶음 규칙(줍기 뒤)", "줍기 뒤");
        auto* list = chosen.list;
        g_pick.lists.push_back(std::move(chosen));
        if (g_pick.hookSaw) {
            return;  // 줍기 함수를 거친 줍기는 그쪽 절차가 잇는다
        }
        // 줍기 함수를 거치지 않은 줍기(끼어들기가 꺼져 있거나, 스크립트가 참조를 인벤토리에 넣은 경우)의 절차.
        // 줍기 함수를 거쳤는데 잇지 못한 것은 여기서 다시 잇지 않는다. 그쪽은 새 묶음이 하나가 아니라는 것을 직접 봤고,
        // 묶음 규칙은 그것을 모른 채 전부터 있던 묶음을 고를 수 있다.
        // "사라진 참조"는 시험 단계가 시작한 줍기에서만 쓴다. 시험 밖에서는 참조가 셀과 함께 내려가 찾을 수 없게 된 것도
        // 사라진 것으로 보이므로, 정답과 견줄 단계가 없는 손 조작에서 그것으로 묶음을 고치지 않는다.
        if (!g_pick.linked) {
            Link("이벤트와 묶음 규칙", g_pick.markByEventRef >= 0 ? g_pick.markByEventRef : g_pick.automated ? gone : kNoAnswer, list);
        }
        if (!g_pick.automated) {
            // 손으로 주운 것(또는 스크립트가 넣은 것)이 줍기 함수를 거치지 않았다.
            LogManualVerdict(false, kNoAnswer, nullptr);
            Track("손으로 주운 뒤");
            if (g_pick.linked) {
                OnManualPickupRecorded();
            }
        }
    }

    // 줍기 함수가 고른 답(받은 참조의 표시, 앞뒤 차이로 찾은 묶음)을 키에 옮긴다. 줍기 함수 안에서 바로 옮기지 않고
    // "줍기 뒤" 관찰 다음에 돌게 한 까닭: 여기서 묶음에 고유 ID를 붙이거나 바꿔 쓰면, 그 뒤에 보는 묶음 규칙이 우리가
    // 고친 상태를 보게 되어 끼어들지 않는 방법이 실제보다 나쁘게 나온다(검수 지적).
    void ApplyPendingLink(std::uint32_t generation)
    {
        if (generation != g_pick.generation || !g_pick.hasPending) {
            return;
        }
        if (g_pick.eventSeen && !g_pick.laterDone) {
            AnalyzePickupLater(generation);  // 작업이 맡긴 순서대로 돌지 않았더라도 관찰이 먼저다
        }
        g_pick.hasPending = false;
        auto* list = g_pick.pendingList;
        if (list) {
            // 그사이 그 묶음이 인벤토리에서 없어졌으면 쓰지 않는다.
            const auto lists = ListsOf(RE::PlayerCharacter::GetSingleton(), TestForm(g_pick.base));
            if (std::find(lists.begin(), lists.end(), list) == lists.end()) {
                SKSE::log::warn("[잇기] 줍기 함수가 고른 묶음이 그사이 인벤토리에서 없어짐");
                list = nullptr;
            }
        }
        Link("줍기 함수", g_pick.pendingMark, list);
        if (!g_pick.automated) {
            LogManualVerdict(true, g_pick.pendingMark, list);
            Track("손으로 주운 뒤");
            if (g_pick.pendingMark >= 0) {
                OnManualPickupRecorded();
            }
        }
    }

    // 맡겨 둔 "줍기 뒤" 관찰과 절차를 기다리지 않고 지금 돌린다. 판정 단계 안에서 다시 주웠을 때 쓴다.
    // 맡겨 둔 작업은 나중에 돌더라도 이미 끝난 것을 보고 아무것도 하지 않는다.
    void FinishPickupNow()
    {
        if (g_pick.eventSeen) {
            AnalyzePickupLater(g_pick.generation);
        }
        ApplyPendingLink(g_pick.generation);
    }

    // 플레이어에게 바닥에서 아이템이 들어왔다는 컨테이너 이벤트가 온 순간.
    void AnalyzePickupEvent(const RE::TESContainerChangedEvent& event)
    {
        if (g_pick.automated || g_pick.inHook) {
            if (event.baseObj != g_pick.base) {
                return;  // 보고 있는 줍기와 상관없는 아이템
            }
        } else {
            // 시험 단계도 줍기 함수도 거치지 않고 온 이벤트(손으로 주웠는데 끼어들기가 꺼져 있거나, 스크립트가 넣은 것).
            if (!HasGroundMark(event.baseObj)) {
                return;
            }
            ResetPickup(event.baseObj, false, kNoAnswer);
        }
        g_pick.eventSeen = true;
        // 이벤트의 참조 칸을 세 가지로 읽어 본다.
        // (1) 핸들로 보고 참조를 얻는다(CommonLibSSE-NG의 선언대로. 5차까지는 이렇게만 읽었고 늘 얻지 못했다).
        // (2) 그 칸의 값을 폼 ID로 본다. 6차 시험에서 내려놓을 때의 이 칸 값이 두 번 모두 바닥 참조의 폼 ID와 같았다.
        // (3) 이벤트의 고유 ID 칸을 바닥 참조에 남겨 둔 번호와 견준다. 6차 시험에서 번호를 남긴 참조를 줍자 이 칸에 그 번호가 실려 왔다.
        const auto       reference = event.reference.get();
        const RE::FormID refId = reference ? reference->GetFormID() : 0;
        const auto       raw = event.reference.native_handle();
        const int        byHandle = refId != 0 ? GroundMark(refId) : kNoAnswer;
        const int        byRawValue = raw != 0 ? GroundMark(static_cast<RE::FormID>(raw)) : kNoAnswer;
        int              byNumber = kNoAnswer;
        if (event.uniqueID != 0) {
            // 남긴 번호는 표시마다 다르다(kStampBase + 표시 번호). 키의 지금 값이 아니라 그 규칙으로 견준다.
            // 줍는 동안 엔진이 변경 이벤트를 보내 키가 이미 옮겨졌더라도 답이 달라지지 않게 하려는 것이다.
            for (const auto& [mark, ref] : g_pick.ground) {
                if (event.uniqueID == static_cast<std::uint16_t>(kStampBase + mark)) {
                    byNumber = byNumber == kNoAnswer ? mark : kAmbiguous;
                }
            }
        }
        g_pick.marks.push_back({ "컨테이너 이벤트의 참조(핸들로 읽음)", byHandle });
        g_pick.marks.push_back({ "컨테이너 이벤트의 참조 칸(폼 ID로 읽음)", byRawValue });
        g_pick.marks.push_back({ "컨테이너 이벤트의 고유 ID 칸(남긴 번호)", byNumber });
        // 끼어들지 않는 절차가 쓸 답: 셋 가운데 먼저 답이 나온 것.
        g_pick.markByEventRef = byHandle >= 0 ? byHandle : byRawValue >= 0 ? byRawValue : byNumber;
        SKSE::log::info("[잇기:이벤트 시점] 참조 칸 {:08X}, 핸들로 얻은 참조 {:08X}, 고유 ID 칸 {:04X} -> 핸들: {} / 폼 ID: {} / 번호: {}", raw, refId,
            event.uniqueID, MarkAnswer(byHandle), MarkAnswer(byRawValue), MarkAnswer(byNumber));
        g_pick.marks.push_back({ "사라진 참조(이벤트 시점)", MarkByGoneRef("이벤트 시점") });
        g_pick.lists.push_back(ChooseReturned("묶음 규칙(이벤트 시점)", "이벤트 시점"));
        if (const auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([generation = g_pick.generation] {
                const auto lock = LockState("줍기 뒤 관찰", std::chrono::seconds(5));
                AnalyzePickupLater(generation);
            });
        }
    }

    // "번호 남기기": 바닥의 참조에 고유 ID(그 참조의 폼 ID, 표시마다 다른 번호)를 남기고 그 번호를 돌려준다.
    // 엔진은 아이템을 내려놓을 때 고유 ID를 떼어 내고, 주울 때 고유 ID가 이미 있는 것은 그대로 인벤토리로 들고 온다(6·7차).
    std::uint16_t StampRef(RE::TESObjectREFR& ref, std::size_t index, const char* when)
    {
        const auto refId = ref.GetFormID();
        const auto number = static_cast<std::uint16_t>(kStampBase + index);
        if (auto* unique = ref.extraList.GetByType<RE::ExtraUniqueID>()) {
            SKSE::log::info("[번호] {}: {} 의 참조 {:08X}에 고유 ID가 있음({:08X}:{:04X}). 그 값을 {:08X}:{:04X}(으)로 바꿔 씀", when, kMarkNames[index],
                refId, unique->baseID, unique->uniqueID, refId, number);
            unique->baseID = refId;
            unique->uniqueID = number;
        } else {
            ref.extraList.Add(new RE::ExtraUniqueID(refId, number));
            SKSE::log::info("[번호] {}: {} 의 참조 {:08X}에 고유 ID가 없어 {:08X}:{:04X}을(를) 붙임", when, kMarkNames[index], refId, refId, number);
        }
        return number;
    }

    // 고유 ID 변경 이벤트 안에서 남긴 번호가 그 뒤에도 남아 있는지를 다음 작업에서 본다. 엔진은 그 이벤트를 아이템을 내려놓는
    // 도중에 보내므로, 그 뒤의 처리가 우리가 남긴 것을 지우거나 바꿀 수 있다. 없어졌거나 애초에 남기지 못했으면 여기서 남긴다.
    // 어느 때에 남긴 것이 살아남는지가 본 모드의 구현을 정한다.
    void VerifyStamp(std::size_t index, RE::FormID refId)
    {
        auto&      key = g_keys[index];
        const auto number = static_cast<std::uint16_t>(kStampBase + index);
        if (!key.valid || !key.world || key.owner != refId) {
            SKSE::log::info("[번호] 다음 작업의 확인: 그사이 {} 의 키가 바뀌어 확인하지 않음(키 {})", kMarkNames[index], DescribeKey(static_cast<Mark>(index)));
            return;
        }
        const auto state = StateOfRef(refId);
        if (!state.ref || state.gone) {
            SKSE::log::info("[번호] 다음 작업의 확인: {} 의 참조 {:08X}이(가) {} 상태라 확인하지 못함", kMarkNames[index], refId, state.text);
            return;
        }
        // 그 폼 ID가 정말 그 아이템의 참조일 때만 건드린다. 번호 0으로 나갔다고 해서 모두 바닥의 아이템이라는 보장은 없다.
        if (const auto* base = state.ref->GetBaseObject(); !base || base->GetFormID() != FormOf(static_cast<Mark>(index))) {
            SKSE::log::warn("[번호] 다음 작업의 확인: {} 의 키가 가리키는 {:08X}은(는) 그 아이템의 참조가 아님(베이스 {:08X}). 건드리지 않음", kMarkNames[index],
                refId, base ? base->GetFormID() : 0);
            return;
        }
        const auto* unique = state.ref->extraList.GetByType<RE::ExtraUniqueID>();
        if (unique && unique->baseID == refId && unique->uniqueID == number) {
            SKSE::log::info("[번호] 다음 작업의 확인: {} 의 참조 {:08X}에 남긴 번호가 그대로 있음: {}", kMarkNames[index], refId,
                Describe(state.ref->extraList));
            key.uid = number;
            return;
        }
        SKSE::log::info("[번호] 다음 작업의 확인: {} 의 참조 {:08X}에 남긴 번호가 {}. 여기서 다시 남김", kMarkNames[index], refId,
            unique ? std::format("다른 값이 돼 있음({:08X}:{:04X})", unique->baseID, unique->uniqueID) : std::string("없음"));
        key.uid = StampRef(*state.ref, index, "다음 작업에서");
    }

    // 고유 ID 변경 이벤트가 "이 표시가 바닥의 참조로 나갔다"고 알려 온 순간에 부른다(이벤트 수신부가 부른다).
    // writeNow가 참이면 그 이벤트 안에서 바로 번호를 남긴다. 그 참조를 그 순간에 찾을 수 있는지, 그 순간의 참조에 고유 ID가
    // 달려 있는지도 함께 드러난다. 거짓이면 이벤트 안에서는 아무것도 고치지 않고, 다음 작업에서 남긴다.
    // 손으로 버린 것에는 뒤쪽을 쓴다. 내려놓은 뒤에 남기는 것은 6·7차에서 여러 번 됐고, 이벤트 안에서 고치는 것은 처음 해 보는
    // 일이라 손 조작의 기록이 거기에 걸리지 않게 한다.
    void StampOnDropEvent(std::size_t index, RE::FormID refId, RE::FormID itemForm, bool writeNow)
    {
        if (writeNow) {
            auto*       ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(refId);
            const auto* base = ref ? ref->GetBaseObject() : nullptr;
            if (ref && base && base->GetFormID() == itemForm) {
                g_keys[index].uid = StampRef(*ref, index, "변경 이벤트 안에서");
            } else {
                SKSE::log::info("[번호] 변경 이벤트 안에서: {} 의 참조 {:08X}을(를) {}. 다음 작업에서 남김", kMarkNames[index], refId,
                    !ref ? "아직 찾을 수 없음" : "찾았지만 그 아이템의 참조가 아님");
            }
        }
        if (const auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([index, refId] {
                const auto lock = LockState("남긴 번호 확인", std::chrono::seconds(5));
                VerifyStamp(index, refId);
            });
        }
    }

    // 표시한 아이템 하나를 바닥에 내려놓는다. 키는 엔진이 보내는 고유 ID 변경 이벤트가 바닥의 참조로 옮겨 준다.
    void DropMarked(Mark mark, DropPath path, Stamp stamp)
    {
        auto*      player = RE::PlayerCharacter::GetSingleton();
        const auto index = static_cast<std::size_t>(mark);
        const auto found = Locate(mark);
        if (!found.extra || found.owner != player) {
            SKSE::log::info("내려놓기: 플레이어에게서 {} 을(를) 찾지 못해 건너뜀", kMarkNames[index]);
            return;
        }
        auto*      object = TestForm(FormOf(mark));
        auto&      key = g_keys[index];
        const bool wasWorld = key.valid && key.world;
        // 내려놓는 함수 안에서 고유 ID 변경 이벤트가 온다. 이벤트 수신부가 이것이 시험 단계의 내려놓기임을 알게 하고,
        // 번호를 이벤트 안에서 남기는 경우에는 그 요청도 알린다.
        g_inTestDrop = true;
        g_stampInEvent[index] = stamp == Stamp::kInEvent;
        const auto handle = path == DropPath::kDropObject ? player->DropObject(object, found.extra, 1) :
                                                            player->RemoveItem(object, 1, RE::ITEM_REMOVE_REASON::kDropping, found.extra, nullptr);
        g_stampInEvent[index] = false;
        g_inTestDrop = false;
        auto ref = handle.get();
        if (!ref && !wasWorld && key.valid && key.world) {
            // 함수가 참조를 돌려주지 않았지만 고유 ID 변경 이벤트가 바닥의 참조를 알려 줬다. 그것으로 이어 간다.
            SKSE::log::info("내려놓기: {} 이(가) 참조를 돌려주지 않아, 고유 ID 변경 이벤트가 알려 준 참조 {:08X}을(를) 씀", NameOf(path), key.owner);
            ref = RE::NiPointer<RE::TESObjectREFR>(RE::TESForm::LookupByID<RE::TESObjectREFR>(key.owner));
        }
        if (!ref) {
            SKSE::log::error("내려놓기: {} 을(를) {}(으)로 내려놓았지만 바닥의 참조를 얻지 못함. 이 표시는 뒤 단계에서 찾지 못함", kMarkNames[index],
                NameOf(path));
            return;
        }
        const auto refId = ref->GetFormID();
        const bool followed = key.valid && key.world && key.owner == refId;
        if (!followed) {
            // 고유 ID가 없던 아이템이면 변경 이벤트가 오지 않는다. 그때는 돌려받은 참조로 직접 맞춘다.
            SKSE::log::info("내려놓기: {} 의 키가 이벤트로 바닥의 참조에 옮겨지지 않아(키 {}) 직접 맞춤", kMarkNames[index], DescribeKey(mark));
            key = Key{ refId, 0, true, true };
        }
        g_lists[index] = nullptr;  // 바닥에 있는 동안에는 견줄 묶음이 없다
        if (stamp == Stamp::kAfterDrop) {
            key.uid = StampRef(*ref, index, "내려놓은 직후");
        } else if (stamp == Stamp::kInEvent && !followed) {
            // 고유 ID가 없던 아이템이라 변경 이벤트가 오지 않았다. 이벤트 안에서 남길 기회가 없었으므로 여기서 남긴다.
            key.uid = StampRef(*ref, index, "내려놓은 직후(변경 이벤트가 오지 않음)");
        }
        SKSE::log::info("[바닥] {} 을(를) {}(으)로 내려놓음: 참조 {:08X}, {} | 키 {}({}){}", kMarkNames[index], NameOf(path), refId,
            Describe(ref->extraList), DescribeKey(mark), followed ? "이벤트로 옮겨짐" : "직접 맞춤",
            stamp == Stamp::kNone ? "" : stamp == Stamp::kAfterDrop ? " | 내려놓은 직후 번호를 남김" : " | 변경 이벤트 안에서 번호를 남기게 함");
    }

    void StepFloorDrop()
    {
        for (const auto& plan : kFloorPlan) {
            DropMarked(plan.mark, plan.drop, plan.stamp);
        }
        Observe("6 다섯 개를 바닥에 내려놓음");
    }

    // 바닥에 있는 표시 하나를 줍는다. 판정은 다음 단계(StepPickVerdict)에서 한다.
    void StepPick(Mark mark, PickPath path)
    {
        auto*       player = RE::PlayerCharacter::GetSingleton();
        const auto  index = static_cast<std::size_t>(mark);
        const auto& key = g_keys[index];
        g_pick.automated = false;
        if (!key.valid || !key.world) {
            SKSE::log::info("줍기: {} 은(는) 바닥에 있지 않아 건너뜀(키 {})", kMarkNames[index], DescribeKey(mark));
            return;
        }
        const auto state = StateOfRef(key.owner);
        if (!state.ref || state.gone) {
            SKSE::log::info("줍기: {} 의 바닥 참조 {:08X}이(가) {} 상태라 건너뜀", kMarkNames[index], key.owner, state.text);
            return;
        }
        // 그 폼 ID가 정말 그 아이템의 참조인지 본다. 불러온 뒤에는 코세이브에서 되살린 폼 ID라, 다른 것이면 엉뚱한 것을 활성화하게 된다.
        if (const auto* base = state.ref->GetBaseObject(); !base || base->GetFormID() != FormOf(mark)) {
            SKSE::log::warn("줍기: {} 의 바닥 참조 {:08X}이(가) 그 아이템이 아님(베이스 {:08X}). 건너뜀", kMarkNames[index], key.owner,
                base ? base->GetFormID() : 0);
            return;
        }
        // 줍는 동안 참조가 지워지지 않게 잡아 둔다.
        const RE::NiPointer<RE::TESObjectREFR> ref(state.ref);
        const RE::FormID                       refId = key.owner;
        const bool                             stamped = key.uid != 0;
        ResetPickup(FormOf(mark), true, static_cast<int>(index));
        g_pick.stamped = stamped;
        SKSE::log::info("[바닥] {} 을(를) {}(으)로 주움: 참조 {:08X}, {}", kMarkNames[index], NameOf(path), refId, Describe(ref->extraList));
        if (path == PickPath::kActivate) {
            const bool activated = ref->ActivateRef(player, 0, nullptr, 1, false);
            g_pick.activated = activated ? 1 : 0;
            SKSE::log::info("[바닥] 활성화의 반환값 {}", activated);
        } else {
            player->PickUpObject(ref.get(), 1, false, false);
        }
    }

    // 직전 단계의 줍기를 정답과 견준다. 정답은 시험이 알고 있다: 어느 표시를 주웠는지는 단계가 정했고, 어느 묶음인지는
    // 줍기 전에는 없던 묶음(하나씩만 줍기 때문에 하나여야 한다)이다. 꼬리표나 이름이 있는 표시는 내용으로도 확인한다.
    void StepPickVerdict(Mark mark, const std::string& label)
    {
        auto*      player = RE::PlayerCharacter::GetSingleton();
        const auto index = static_cast<std::size_t>(mark);
        if (!g_pick.automated || g_pick.expected != static_cast<int>(index)) {
            Observe((label + " (줍기를 건너뜀)").c_str());
            return;
        }
        auto* object = TestForm(g_pick.base);
        auto  fresh = FreshLists(player, object);
        int   gained = TotalCount(player, object) - g_pick.countBefore;
        if (gained == 0 && fresh.empty() && g_keys[index].valid && g_keys[index].world && !StateOfRef(g_keys[index].owner).gone) {
            if (g_pick.activated == 0) {
                // 활성화가 거절됐다. 뒤 단계가 이어지도록 직접 줍고, 맡겨질 "줍기 뒤" 관찰과 절차를 여기서 바로 돌려 판정에 넣는다.
                SKSE::log::warn("[잇기 판정] {}: 활성화가 거절돼 주워지지 않음. PickUpObject로 다시 주움(이 판정은 그 줍기의 것)", kMarkNames[index]);
                StepPick(mark, PickPath::kPickUpObject);
                FinishPickupNow();
                fresh = FreshLists(player, object);
                gained = TotalCount(player, object) - g_pick.countBefore;
            } else {
                // 활성화는 받아들여졌는데 아직 돌아오지 않았다. 다른 모드가 줍는 동작을 늦게 끝내는 경우일 수 있어, 직접 줍지 않는다
                // (같은 참조를 두 번 줍게 될 수 있다). 이 표시는 바닥에 둔 채 넘어가고, 뒤늦게 주워지면 손으로 주운 것처럼 기록된다.
                SKSE::log::warn("[잇기 판정] {}: 주운 뒤 기다렸지만 돌아오지 않음(개수 그대로, 참조도 그대로). 다시 줍지 않고 넘어감", kMarkNames[index]);
                g_pick.automated = false;
                Observe((label + " (돌아오지 않음)").c_str());
                return;
            }
        }
        auto* truth = fresh.size() == 1 ? fresh.front() : nullptr;
        if (truth) {
            SKSE::log::info("[잇기 판정] {}: 정답(줍기 전에는 없던 묶음) = {}", kMarkNames[index], Describe(*truth));
        } else if (fresh.empty()) {
            SKSE::log::info("[잇기 판정] {}: 새 묶음 없음(개수 {:+}). 묶음 없이 돌아왔거나 돌아오지 않음", kMarkNames[index], gained);
        } else {
            SKSE::log::info("[잇기 판정] {}: 새 묶음이 {}개라 정답을 정하지 못함(개수 {:+})", kMarkNames[index], fresh.size(), gained);
        }
        if (!IsUidOnly(mark)) {
            // 정답과 따로 선 확인: 꼬리표나 이름으로 찾은 묶음이 정답과 같은가.
            const auto* byContent = FindIn(mark, player, "플레이어").extra;
            SKSE::log::info("[잇기 판정] {}: 내용으로 찾은 묶음이 정답과 {}", kMarkNames[index],
                !truth ? "견줄 수 없음(정답이 없음)" : !byContent ? "견줄 수 없음(내용으로 찾지 못함)" : byContent == truth ? "같음" : "다름");
        } else {
            // 꼬리표나 이름이 없는 표시는 정답을 따로 확인할 길이 없다. 정답과 "줍기 함수의 앞뒤 차이"는 같은 계산(줍기 전 목록과의
            // 차이)을 시점만 달리한 것이고, 절차가 그 묶음의 고유 ID를 키로 삼았다면 "절차의 결과"도 그 묶음과 견주는 것이다.
            SKSE::log::info("[잇기 판정] {}: 주의 - 내용으로 따로 확인할 수 없는 표시. 정답, 줍기 함수의 앞뒤 차이, 절차의 결과가 같은 계산에서 나옴",
                kMarkNames[index]);
        }
        bool uidEventClaimed = false;
        for (const auto& claim : g_pick.marks) {
            uidEventClaimed = uidEventClaimed || std::string_view(claim.method) == kMethodUidEvent;
            SKSE::log::info("[잇기 판정] {}: 어느 표시인가 | {} -> {} : {}", kMarkNames[index], claim.method, MarkAnswer(claim.mark),
                claim.mark == static_cast<int>(index) ? "맞음" : claim.mark < 0 ? "답하지 못함" : "틀림");
        }
        if (g_pick.stamped && !uidEventClaimed) {
            SKSE::log::info("[잇기 판정] {}: 어느 표시인가 | {} -> 번호를 남겼지만 이벤트가 오지 않음 : 답하지 못함", kMarkNames[index], kMethodUidEvent);
        }
        for (const auto& claim : g_pick.lists) {
            SKSE::log::info("[잇기 판정] {}: 어느 묶음인가 | {} -> {} : {}", kMarkNames[index], claim.method,
                claim.list ? std::format("@{:X}", reinterpret_cast<std::uintptr_t>(claim.list) & 0xFFFFF) : claim.why,
                !claim.list ? "답하지 못함" : !truth ? "견줄 수 없음(정답이 없음)" : claim.list == truth ? "맞음" : "틀림");
        }
        const auto& key = g_keys[index];
        const auto* unique = truth ? truth->GetByType<RE::ExtraUniqueID>() : nullptr;
        const bool  right = unique && key.valid && !key.world && unique->baseID == key.owner && unique->uniqueID == key.uid;
        const bool  moved = key.valid && !key.world;  // 절차나 이벤트가 키를 인벤토리 쪽으로 옮겼다
        const char* outcome = right                  ? "맞음" :
                              truth && moved         ? "틀리게 이음" :
                              truth                  ? "잇지 못함" :
                              moved && fresh.empty() ? "틀리게 이음(새 묶음이 없는데 키를 옮김)" :
                              moved                  ? "견줄 수 없음(정답을 정하지 못함)" :
                              fresh.empty()          ? "잇지 못함(다시 묶을 묶음이 없음)" :
                                                       "잇지 못함(정답을 정하지 못함)";
        SKSE::log::info("[잇기 판정] {}: 절차의 결과 {} (키 {})", kMarkNames[index], outcome, DescribeKey(mark));
        if (truth && !right) {
            Rebind(mark, truth);
            SKSE::log::info("[잇기 판정] {}: 뒤 단계가 이어지도록 정답으로 바로잡음 -> {} | 키 {}", kMarkNames[index], Describe(*truth),
                DescribeKey(mark));
        } else if (truth) {
            g_lists[index] = truth;  // 이후의 "묶음" 판정은 돌아온 이 묶음을 기준으로 한다
        } else if (fresh.empty() && (moved || (key.valid && key.world && StateOfRef(key.owner).gone))) {
            // 새 묶음이 없다. 키가 인벤토리 쪽으로 옮겨져 있다면 엉뚱한 묶음을 가리키는 것이고, 바닥의 참조를 가리킨 채라면
            // 그 참조는 사라졌다. 어느 쪽이든 그대로 두면 뒤 단계와 저장이 틀린 것을 따라가므로 키를 비운다.
            SKSE::log::info("[잇기 판정] {}: 다시 묶을 묶음이 없어 키를 비움. 이 표시는 뒤 단계에서 찾지 못함", kMarkNames[index]);
            g_keys[index] = Key{};
            g_lists[index] = nullptr;
        }
        g_pick.automated = false;
        Observe(label.c_str());
    }

    void StepLeaveNameSword()
    {
        // 이름표 검은 저장할 때 바닥에 있게 둔다. 불러온 뒤에 주워, 바닥에 있는 동안의 키가 저장을 거쳐 남는지를 본다.
        // 8차부터 번호를 남긴 채로 둔다. 번호 없이 둔 경우는 6·7차에서 봤다. 남긴 번호가 저장을 거쳐 남는지가 물음이다.
        DropMarked(Mark::kSwordName, DropPath::kDropObject, Stamp::kAfterDrop);
        Observe("11a 검(이름)을 바닥에 내려놓음(저장할 때 바닥에 있게 둠)");
    }

    void SetHelmetEquipped(bool equip)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto found = Locate(Mark::kHelmetUid);
        if (!found.extra || found.owner != player) {
            SKSE::log::info("장착: 투구/고유ID 를 찾지 못해 건너뜀");
            return;
        }
        auto* manager = RE::ActorEquipManager::GetSingleton();
        if (equip) {
            manager->EquipObject(player, TestForm(kIronHelmet), found.extra, 1, nullptr, false, false, false, true);
        } else {
            manager->UnequipObject(player, TestForm(kIronHelmet), found.extra, 1, nullptr, false, false, false, true);
        }
    }

    void StepSell()
    {
        // 파는 것은 상인의 상자로 옮기는 것이다. 이유만 "판매"로 바꿔 같은 상자로 옮겨 본다.
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto found = FindIn(Mark::kSwordUidTagB, player, "플레이어");
        if (!found.extra || !Chest()) {
            SKSE::log::info("판매: 검/고유ID+꼬리표B 또는 상자를 찾지 못해 건너뜀");
            return;
        }
        player->RemoveItem(TestForm(kIronSword), 1, RE::ITEM_REMOVE_REASON::kSelling, found.extra, Chest());
        Observe("10 검(꼬리표B)을 판매 이유로 상자에 넘김");
    }

    void StepBuyBack()
    {
        const auto found = FindIn(Mark::kSwordUidTagB, Chest(), "상자");
        if (!found.extra) {
            SKSE::log::info("되사기: 상자에서 검/고유ID+꼬리표B 를 찾지 못해 건너뜀");
            return;
        }
        Chest()->RemoveItem(TestForm(kIronSword), 1, RE::ITEM_REMOVE_REASON::kRemove, found.extra, RE::PlayerCharacter::GetSingleton());
        Observe("11 상자에서 되찾음");
    }

    // 저장할 때 코세이브에 넣을 기록: 표시한 아이템마다 지금의 위치와 고유 ID.
    std::vector<SavedMark> CollectMarks()
    {
        std::vector<SavedMark> marks;
        for (std::size_t i = 0; i < kMarkNames.size(); ++i) {
            const auto  found = Locate(static_cast<Mark>(i));
            const auto& key = g_keys[i];
            SavedMark   saved;
            saved.mark = static_cast<std::uint8_t>(i);
            saved.chestRef = g_chestId;
            if (found.extra) {
                saved.where = found.owner == Chest() ? kInChest : kInPlayer;
                if (const auto* unique = found.extra->GetByType<RE::ExtraUniqueID>()) {
                    saved.uniqueId = unique->uniqueID;
                    saved.uniqueBase = unique->baseID;
                }
            } else if (key.valid && key.world) {
                // 바닥에 있다. 그 참조의 폼 ID가 키다(번호를 남겼으면 그 번호도).
                saved.where = kOnGround;
                saved.groundRef = key.owner;
                saved.uniqueId = key.uid;
                saved.uniqueBase = key.owner;
            } else {
                continue;
            }
            marks.push_back(saved);
        }
        return marks;
    }

    // 불러온 뒤: 코세이브에 적어 둔 것과 지금의 인벤토리를 견준다.
    void CompareWithCosave(const char* label)
    {
        SKSE::log::info("[코세이브:{}] 되돌리기 콜백 {}번째, 불러오기 콜백 {}, 기록 {}건", label, g_revertCount, g_cosaveLoaded ? "불림" : "안 불림",
            g_loadedMarks.size());
        for (const auto& saved : g_loadedMarks) {
            if (saved.mark >= kMarkNames.size()) {
                SKSE::log::info("[코세이브:{}] 알 수 없는 표시 번호 {}", label, saved.mark);
                continue;
            }
            if (saved.where == kOnGround) {
                // 저장할 때 바닥에 있던 표시. 그 참조가 불러온 뒤에도 같은 폼 ID로 있는지, 꼬리표나 이름이 있으면 그것도 그대로인지를 본다.
                const auto state = StateOfRef(saved.groundRef);
                const auto mark = static_cast<Mark>(saved.mark);
                SKSE::log::info("[코세이브:{}] {}: 저장 때 바닥의 참조 {:08X} -> 지금 {}{}{}", label, kMarkNames[saved.mark], saved.groundRef,
                    state.text, state.ref ? ", " + Describe(state.ref->extraList) : std::string(),
                    IsUidOnly(mark) || !state.ref ? "" : Matches(mark, state.ref->extraList) ? " | 내용이 그 표시의 것" : " | 내용이 그 표시의 것이 아님");
                continue;
            }
            const auto found = Locate(static_cast<Mark>(saved.mark));
            if (!found.extra) {
                SKSE::log::info("[코세이브:{}] {}: 저장 때 고유ID {:08X}:{:04X} -> 지금은 찾지 못함", label, kMarkNames[saved.mark], saved.uniqueBase,
                    saved.uniqueId);
                continue;
            }
            const auto* unique = found.extra->GetByType<RE::ExtraUniqueID>();
            const bool  same = unique ? (unique->uniqueID == saved.uniqueId && unique->baseID == saved.uniqueBase) : (saved.uniqueId == 0);
            // 고유 ID만 붙인 표시는 저장해 둔 키로 찾은 것이라 고유 ID가 같을 수밖에 없다. 알 수 있는 것은 "그 키의 자리에 아이템이 있다"까지다.
            // 꼬리표나 이름이 있는 표시는 내용으로 찾았으므로, 고유 ID가 같으면 그 아이템의 고유 ID가 저장을 거쳐 남았다는 뜻이다.
            const char* verdict = IsUidOnly(static_cast<Mark>(saved.mark)) ? "저장한 키로 다시 찾음" : same ? "같음" : "다름";
            SKSE::log::info("[코세이브:{}] {}: 저장 때 고유ID {:08X}:{:04X}, 지금 {} -> {}", label, kMarkNames[saved.mark], saved.uniqueBase,
                saved.uniqueId, Describe(*found.extra), verdict);
        }
    }

    void RequestSave(const char* name)
    {
        if (auto* manager = RE::BGSSaveLoadManager::GetSingleton()) {
            manager->Save(name);
            SKSE::log::info("저장을 요청함: {}", name);
        }
    }

    void RequestLoad(const char* name)
    {
        if (auto* manager = RE::BGSSaveLoadManager::GetSingleton()) {
            g_loadDeadline = (Clock::now() + kLoadTimeout).time_since_epoch().count();
            g_waitingForLoad = true;
            g_cosaveLoaded = false;
            g_loadedMarks.clear();
            SKSE::log::info("불러오기를 요청함: {}", name);
            manager->Load(name);
        }
    }

    struct Step
    {
        std::string           name;
        std::function<void()> run;
        int                   waitMs;       // 이 단계를 한 뒤 다음 단계까지 기다리는 시간
        bool                  needsSaveLoad;
    };

    // 줍기 하나는 두 단계다: 줍고, 조금 기다렸다가(줍는 동작과 다른 모드의 뒤처리가 끝나도록) 정답과 견준다.
    // 활성화로 줍는 쪽은 줍는 동작이 늦게 끝나는 환경을 생각해 더 기다린다.
    void AddPickSteps(std::vector<Step>& steps, Mark mark, PickPath path, const std::string& label, bool needsSaveLoad)
    {
        const auto name = std::string(kMarkNames[static_cast<std::size_t>(mark)]);
        steps.push_back({ "줍기: " + name, [mark, path] { StepPick(mark, path); }, path == PickPath::kActivate ? 1500 : 800, needsSaveLoad });
        steps.push_back({ "줍기 판정: " + name, [mark, label] { StepPickVerdict(mark, label); }, 300, needsSaveLoad });
    }

    std::vector<Step> BuildSteps()
    {
        std::vector<Step> steps{
            { "기준 세이브 저장", [] { RequestSave(kBaselineSave); }, 6000, true },
            { "준비", StepSetup, 500, false },
            { "표시 1/3: 내려놓기", StepMarkDrop, 1500, false },
            { "표시 2/3: 꼬리표 붙여 줍기", StepMarkTagAndPickUp, 1000, false },
            { "표시 3/3: 고유 ID 붙이기", StepMarkUnique, 500, false },
            { "합치기", StepAddPlain, 500, false },
            { "상자로", StepToChest, 700, false },
            { "상자에서", StepFromChest, 700, false },
            { "바닥에 내려놓기(다섯 개)", StepFloorDrop, 1500, false },
        };
        for (const auto order : kPickOrder) {
            const auto& plan = kFloorPlan[order];
            AddPickSteps(steps, plan.mark, plan.pick,
                std::format("7 {} 을(를) {}(으)로 주움", kMarkNames[static_cast<std::size_t>(plan.mark)], NameOf(plan.pick)), false);
        }
        steps.insert(steps.end(), {
            Step{ "투구 장착", [] { SetHelmetEquipped(true); }, 800, false },
            Step{ "장착 뒤 관찰", [] { Observe("8 투구를 장착함"); }, 200, false },
            Step{ "투구 해제", [] { SetHelmetEquipped(false); }, 800, false },
            Step{ "해제 뒤 관찰", [] { Observe("9 투구를 해제함"); }, 200, false },
            Step{ "판매", StepSell, 700, false },
            Step{ "되찾기", StepBuyBack, 700, false },
            Step{ "검(이름)을 바닥에 둠", StepLeaveNameSword, 1500, true },
            Step{ "표시한 세이브 저장", [] { RequestSave(kMarkedSave); }, 6000, true },
            Step{ "표시한 세이브 불러오기", [] { RequestLoad(kMarkedSave); }, 3000, true },
            Step{ "불러온 뒤 관찰(표시한 세이브)",
                [] {
                    Observe("12 표시한 세이브를 불러옴");
                    CompareWithCosave("표시한 세이브");
                },
                500, true },
        });
        AddPickSteps(steps, Mark::kSwordName, PickPath::kActivate, "12a 불러온 뒤 바닥의 검(이름)을 활성화로 주움", true);
        steps.insert(steps.end(), {
            Step{ "기준 세이브 불러오기", [] { RequestLoad(kBaselineSave); }, 3000, true },
            Step{ "불러온 뒤 관찰(기준 세이브)",
                [] {
                    Observe("13 기준 세이브를 불러옴(표시가 없어야 함)");
                    CompareWithCosave("기준 세이브");
                },
                500, true },
            Step{ "표시한 세이브 다시 불러오기", [] { RequestLoad(kMarkedSave); }, 3000, true },
            Step{ "불러온 뒤 관찰(표시한 세이브, 두 번째)",
                [] {
                    Observe("14 표시한 세이브를 다시 불러옴");
                    CompareWithCosave("표시한 세이브(두 번째)");
                },
                500, true },
        });
        AddPickSteps(steps, Mark::kSwordName, PickPath::kPickUpObject, "14a 다시 불러온 뒤 바닥의 검(이름)을 PickUpObject로 주움", true);
        return steps;
    }

    // 손 조작이 기록된 뒤에 도는 마무리 단계. 번호를 고유 ID 변경 이벤트 안에서 남겨도 되는지를 본다.
    // 엔진이 아이템을 내려놓는 도중에 그 참조의 데이터를 고치는 일이라 어떻게 될지 모른다. 그래서 자동 단계와 손 조작의 기록이
    // 모두 로그에 남은 뒤에 돌린다. 여기서 문제가 생겨도 잃는 것은 이 물음의 답 하나다.
    std::vector<Step> BuildExtraSteps()
    {
        std::vector<Step> steps{
            { "투구를 내려놓으며 변경 이벤트 안에서 번호를 남김",
                [] {
                    DropMarked(Mark::kHelmetUid, DropPath::kDropObject, Stamp::kInEvent);
                    Observe("15 투구를 내려놓으며 변경 이벤트 안에서 번호를 남김");
                },
                1500, false },
        };
        AddPickSteps(steps, Mark::kHelmetUid, PickPath::kActivate, "15a 변경 이벤트 안에서 번호를 남긴 투구를 활성화로 주움", false);
        return steps;
    }

    // 지켜보기 시험: 표시한 아이템에 눈에 보이는 이름을 붙인다. 메뉴에서 사용자가 골라낼 수 있어야 하고,
    // 단련으로 꼬리표(단련 수치)가 바뀌어도 이름으로 가려낼 수 있어야 한다.
    void StepWatchNames()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (const auto mark : { Mark::kSwordUidTagA, Mark::kSwordUidTagB, Mark::kHelmetUid }) {
            const auto  index = static_cast<std::size_t>(mark);
            const char* name = WatchNameOf(mark);
            const auto  found = Locate(mark);
            if (!found.extra || found.owner != player) {
                SKSE::log::error("이름 붙이기: 플레이어에게서 {} 을(를) 찾지 못함", kMarkNames[index]);
                continue;
            }
            if (auto* text = found.extra->GetByType<RE::ExtraTextDisplayData>()) {
                text->SetName(name);
            } else {
                found.extra->Add(new RE::ExtraTextDisplayData(name));
            }
            if (!NameStartsWith(*found.extra, name)) {
                // 꼬리표가 있는 검은 꼬리표로 여전히 가려낼 수 있다. 투구는 이름뿐이라, 붙지 않았으면 내용으로 확인할 수 없는 표시로 둔다.
                SKSE::log::error("이름 붙이기: {} 에 이름 '{}'이(가) 붙지 않음: {}", kMarkNames[index], name, Describe(*found.extra));
            } else {
                g_hasContent[index] = true;
                SKSE::log::info("이름 붙이기: {} -> {}", kMarkNames[index], Describe(*found.extra));
            }
        }
        // 단련에 쓸 재료. 표시한 주괴(주괴/고유ID)가 재료로 쓰이지 않도록 표시 없는 주괴를 넉넉히 넣는다.
        player->AddObjectToContainer(TestForm(kIronIngot), nullptr, 6, nullptr);
        Observe("W1 이름을 붙이고 단련 재료를 넣음");
    }

    // 지켜보기 시험: 시체 경로. 스키버를 하나 놓고 표시한 검('PoE5 B')을 그 인벤토리에 넣은 뒤 죽인다.
    // 사용자는 그 시체를 뒤져 검을 꺼낸다. 살아 있는 액터에게 넘기는 것과, 죽은 뒤 꺼내는 것이 이 한 번에 들어 있다.
    void StepWatchCorpse()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(kSkeever);
        auto  placed = base ? player->PlaceObjectAtMe(base, false) : nullptr;
        auto* actor = placed ? placed->As<RE::Actor>() : nullptr;
        if (!actor) {
            SKSE::log::error("[시체] 스키버를 놓지 못함. 시체 경로는 건너뛰게 됩니다.");
            return;
        }
        g_corpseId = actor->GetFormID();
        const auto found = Locate(Mark::kSwordUidTagB);
        if (found.extra && found.owner == player) {
            player->RemoveItem(TestForm(kIronSword), 1, RE::ITEM_REMOVE_REASON::kRemove, found.extra, actor);
        } else {
            SKSE::log::error("[시체] 플레이어에게서 검/고유ID+꼬리표B 를 찾지 못해 시체에 넣지 못함");
        }
        LogCorpse("놓은 직후");
        Observe("W2 스키버에게 'PoE5 B'를 넣음");
    }

    // 놓은 스키버를 죽인다. 9차에서는 놓은 바로 그 작업 안에서 Actor::DoDamage를 불렀고, 함수가 false를 돌려줬으며 직후에는
    // 죽은 상태가 아니었다. 그 뒤에 죽었는지는 로그에 없어 그 방법이 통했는지 알 수 없다. 그래서 놓고 나서 한 박자 뒤에 부르고,
    // 다음 단계에서 죽었는지를 다시 본다.
    void StepWatchKill()
    {
        auto* actor = CorpseActor();
        if (!actor) {
            SKSE::log::error("[시체] 놓은 스키버를 찾지 못함");
            return;
        }
        LogCorpse("죽이기 전");
        if (!actor->IsDead()) {
            // 프로토타입 2에서 게임 안 동작을 본 함수. 그때는 이미 자리를 잡은 액터에게 불렀다.
            const bool result = actor->DoDamage(100000.0f, RE::PlayerCharacter::GetSingleton(), true);
            SKSE::log::info("[시체] DoDamage(100000)을 부름: 반환 {}", result);
        }
        LogCorpse("DoDamage 직후");
    }

    // 죽었는지 다시 본다. 아직 살아 있으면 체력 수치를 직접 깎는다(액터 값의 가상 함수라 주소 번호가 틀릴 일이 없다).
    // 그래도 죽지 않으면 사용자가 직접 잡아야 한다는 것을 화면에 알린다.
    void StepWatchKillCheck()
    {
        auto* actor = CorpseActor();
        if (!actor) {
            return;
        }
        LogCorpse("DoDamage 1.5초 뒤");
        if (!actor->IsDead()) {
            // (이 CommonLibSSE-NG 9.0.2에서는 이 가상 함수(06)의 이름이 ModActorValue다. 피해 쪽 수정치에 음수를 더한다.)
            actor->AsActorValueOwner()->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -100000.0f);
            SKSE::log::info("[시체] 아직 살아 있어 체력 수치를 직접 깎음");
            LogCorpse("체력을 깎은 직후");
        }
    }

    // 지켜보기 시험의 준비 단계. 표시는 자동 시험과 같은 방법으로 붙인다.
    std::vector<Step> BuildWatchSteps()
    {
        return {
            { "준비", StepSetup, 500, false },
            { "표시 1/3: 내려놓기", StepMarkDrop, 1500, false },
            { "표시 2/3: 꼬리표 붙여 줍기", StepMarkTagAndPickUp, 1000, false },
            { "표시 3/3: 고유 ID 붙이기", StepMarkUnique, 500, false },
            { "이름 붙이기와 단련 재료", StepWatchNames, 500, false },
            { "시체 준비: 스키버를 놓고 검을 넣음", StepWatchCorpse, 1500, false },
            { "시체 준비: 죽임", StepWatchKill, 1500, false },
            { "시체 준비: 확인", StepWatchKillCheck, 1500, false },
        };
    }

    // 지켜보기의 저장 왕복: 사용자가 아이템을 상인·동료·시체·상자에 둔 채로 시작 키를 누르면, 지금 상태를 저장하고 그 세이브를
    // 불러온 뒤 키가 여전히 맞는지를 본다. 코세이브에는 결정 59의 키를 그대로 적고(SaveWatch), 불러올 때 가진 쪽의 폼 ID를
    // 게임에 다시 물어 푼다(LoadWatch).
    void StepWatchBeforeSave()
    {
        WatchVerdict("저장 직전");
        LogCorpse("저장 직전");
        Observe("W5 저장 직전");
    }

    void AfterLoadReport(const char* how)
    {
        SKSE::log::info("[코세이브:불러온 뒤] {} / 되돌리기 콜백 {}번째, 불러오기 콜백 {}", how, g_revertCount, g_cosaveLoaded ? "불림" : "안 불림");
        WatchVerdict("불러온 뒤");
        LogCorpse("불러온 뒤");
        Observe("W6 불러온 뒤");
    }

    // 저장 왕복의 불러오기. 바로 앞 단계의 저장에서 코세이브 콜백이 지켜보기 기록을 쓰지 않았으면 불러오지 않는다.
    // 그대로 불러오면 예전 실행이 남긴 같은 이름의 세이브(그때의 키가 든)가 불려 와 판정이 통과처럼 남는다.
    void StepWatchLoad()
    {
        if (g_watchSaveCount == g_roundTripSaveBase) {
            SKSE::log::error("저장 왕복: 저장 요청 뒤에 코세이브의 지켜보기 기록이 쓰이지 않음(저장이 되지 않은 것으로 봄). 불러오지 않고 저장 왕복을 그만둠");
            Notify(std::format("[PoE Proto 5] 저장이 되지 않아 불러오지 않았습니다(로그 확인). {}을 다시 누르면 다시 해 봅니다.", StartKeyName()));
            g_running = false;
            g_phase = Phase::kWatch;  // 저장 왕복 전의 지켜보기로 돌아간다. g_watchStage는 0 그대로다
            return;
        }
        RequestLoad(kWatchSave);
    }

    std::vector<Step> BuildWatchRoundTripSteps()
    {
        return {
            { "저장 직전의 판정", StepWatchBeforeSave, 500, false },
            { "저장", [] { RequestSave(kWatchSave); }, 6000, false },
            { "불러오기", StepWatchLoad, 3000, false },
            { "불러온 뒤의 판정", [] { AfterLoadReport("시험이 요청한 불러오기"); }, 500, false },
        };
    }

    // 지금 도는 단계 목록: 자동 단계, 손 조작 뒤의 마무리 단계, 지켜보기 시험의 준비 단계, 또는 지켜보기의 저장 왕복.
    const std::vector<Step>& Steps()
    {
        static const std::vector<Step> main = BuildSteps();
        static const std::vector<Step> extra = BuildExtraSteps();
        static const std::vector<Step> watch = BuildWatchSteps();
        static const std::vector<Step> roundTrip = BuildWatchRoundTripSteps();
        return g_phase == Phase::kExtra ? extra : g_phase == Phase::kWatchSetup ? watch : g_phase == Phase::kWatchRoundTrip ? roundTrip : main;
    }

    void NotifyWatchTodo()
    {
        if (g_watchStage == 0 && g_settings.saveLoad) {
            Notify("[PoE Proto 5] 맡기기: 'PoE5 A'는 상인에게 팔기 / 'PoE5 helmet'은 동료에게");
            Notify("[PoE Proto 5] 'PoE5 name tag'는 발밑 상자에 / 'PoE5 B'는 스키버에 그대로");
            Notify(std::format("[PoE Proto 5] 다 맡겼으면 메뉴를 닫고 {}(저장하고 불러옵니다)", StartKeyName()));
        } else if (g_settings.saveLoad) {
            Notify("[PoE Proto 5] 불러왔습니다. 되찾기: 상인에게서 'PoE5 A' / 동료에게서 'PoE5 helmet'");
            Notify("[PoE Proto 5] 상자에서 'PoE5 name tag' / 스키버 시체에서 'PoE5 B'");
            Notify(std::format("[PoE Proto 5] 다 되찾았으면 {}(끝)", StartKeyName()));
        } else {
            Notify(std::format("[PoE Proto 5] 'PoE5'로 시작하는 아이템을 팔고, 건네고, 꺼내고, 단련해 보세요. 다 했으면 {}(끝).", StartKeyName()));
        }
    }

    // 손 조작(버리고 줍기)이 기록됐다. 자동 단계가 끝나고 손 조작을 기다리던 중이었으면 마무리 단계를 시작한다.
    void OnManualPickupRecorded()
    {
        if (g_phase != Phase::kAwaitManual) {
            Notify("[PoE Proto 5] 주운 것을 기록했습니다.");
            if (g_phase == Phase::kWatch) {
                QueueWatchVerdict("바닥에서 주운 것을 이음");  // 주울 때는 고유 ID 변경 이벤트가 오지 않아 판정이 따로 맡겨지지 않는다
            }
            return;
        }
        g_phase = Phase::kExtra;
        g_step = 0;
        g_waitingForLoad = false;
        g_nextAt = Clock::now() + std::chrono::milliseconds(1500);
        SKSE::log::info("=== 손 조작이 기록됨. 마무리 단계를 시작함 (스레드 {})", Tid());
        Notify("[PoE Proto 5] 주운 것을 기록했습니다. 마무리 단계가 몇 초 돕니다. 조작하지 말고 두세요.");
        g_running = true;
    }

    void Pump();

    // SKSE에 Pump를 한 번 돌리도록 맡긴다. 이미 맡긴 것이 있으면 더 쌓지 않는다.
    // 맡긴 작업이 도는 스레드는 하나로 정해져 있지 않다(6차 시험). 상태는 g_state로 지킨다.
    void QueuePump()
    {
        if (g_pumpQueued.exchange(true)) {
            return;
        }
        const auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            g_pumpQueued = false;
            return;
        }
        tasks->AddTask([] {
            g_pumpQueued = false;
            // 못 잡으면 이번에는 건너뛴다. 50ms 뒤에 다시 맡겨진다.
            const StateLock lock(g_state, std::chrono::milliseconds(200));
            if (lock.owns_lock()) {
                Pump();
            }
        });
    }

    // 시험이 도는 동안 일정한 간격으로 Pump를 깨우는 스레드. 게임의 것은 건드리지 않고 작업 하나를 맡기기만 한다.
    // 작업이 끝날 때 스스로 다음 작업을 맡기는 방식은 쓰지 않는다. 2차 시험에서 그렇게 했더니 시험 전체가 한 프레임 안에서 돌았고
    // (단계 시각이 18번 모두 밀리초까지 예정과 같았다) 그동안 게임이 멈춰 불러오기가 3분 동안 시작되지 못했다. SKSE가 맡겨진 작업이
    // 없어질 때까지 한자리에서 돌리는 것으로 보인다(관찰에서 나온 추론이고 SKSE 소스로 대조하지는 않았다).
    // 불러오기를 기다리는 동안에는 제한 시간이 지날 때까지 아무것도 맡기지 않는다. 대기는 kPostLoadGame 알림이 푼다.
    void TickerMain()
    {
        for (;;) {
            std::this_thread::sleep_for(kTick);
            // 지켜보기 기록이 든 세이브가 시험의 요청 없이 불려 온 경우: 화면이 자리를 잡은 뒤에 판정을 한 번 남긴다.
            if (const auto at = g_afterLoadVerdictAt.load(); at != 0 && Clock::now().time_since_epoch().count() >= at) {
                g_afterLoadVerdictAt = 0;
                if (const auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([] {
                        const auto lock = LockState("불러온 뒤의 판정", std::chrono::seconds(5));
                        // 맡겨진 뒤에 다른 불러오기가 시작됐으면(되돌리기 횟수가 달라짐) 버린다. 그 불러오기가 제 판정을 따로 맡긴다.
                        if (g_phase == Phase::kWatch && g_revertCount == g_afterLoadRevert) {
                            SKSE::log::info("=== 지켜보기: 기록이 든 세이브가 불려 와 이어 감({}) (스레드 {})",
                                g_watchStage == 0 ? "저장 왕복 전의 세이브" : "저장 왕복의 세이브", Tid());
                            AfterLoadReport("시험이 요청하지 않은 불러오기(메뉴에서, 또는 게임을 다시 켠 뒤)");
                            NotifyWatchTodo();
                        }
                    });
                }
            }
            if (!g_running) {
                continue;
            }
            if (g_waitingForLoad && Clock::now().time_since_epoch().count() < g_loadDeadline) {
                continue;
            }
            QueuePump();
        }
    }

    // SKSE의 작업으로, g_state를 잡은 채로. 기다릴 시간이 지났으면 다음 단계를 돌린다.
    void Pump()
    {
        if (!g_running) {
            return;
        }
        if (g_waitingForLoad) {
            // 불러오기가 끝났다는 알림이 끝내 오지 않으면(세이브가 없거나 불러오기가 실패) 시험을 멈춘다.
            if (Clock::now().time_since_epoch().count() >= g_loadDeadline) {
                g_running = false;
                g_waitingForLoad = false;
                SKSE::log::error("불러오기가 끝났다는 알림이 제한 시간 안에 오지 않아 시험을 멈춤({}단계)", g_step);
                Notify("[PoE Proto 5] 불러오기가 끝나지 않아 시험을 멈춤(로그 확인)");
                if (g_phase == Phase::kWatchRoundTrip) {
                    // 저장 왕복을 끝내지 못했다. 불러오기가 어디까지 갔는지에 따라 남은 상태가 다르다.
                    const bool anyKey = std::any_of(g_keys.begin(), g_keys.end(), [](const Key& key) { return key.valid; });
                    if (anyKey && g_cosaveLoaded) {
                        // 불러오기 콜백까지는 왔다(키는 그 세이브의 것이고 단계는 저장 왕복 뒤). 끝났다는 알림이 늦게라도 오면
                        // 시험이 요청하지 않은 불러오기와 같은 길로 판정을 남긴다.
                        g_phase = Phase::kWatch;
                        g_watchColdLoad = true;
                        SKSE::log::warn("저장 왕복: 불러오기 콜백은 왔으나 끝났다는 알림이 없음. 알림이 오면 그때 판정을 남김");
                    } else if (anyKey) {
                        // 불러오기가 시작되지 않았다(되돌리기 콜백이 오지 않아 키가 그대로다). 저장 왕복 전의 지켜보기로 돌아간다.
                        g_phase = Phase::kWatch;
                        g_watchStage = 0;
                        SKSE::log::warn("저장 왕복: 불러오기가 시작되지 않음. 지켜보기를 이어 가고, 시작 키를 다시 누르면 다시 해 봄");
                    } else {
                        // 되돌리기 콜백이 키를 비웠는데 불러오기 콜백은 오지 않았다. 따라갈 키가 없으므로 지켜보기를 이어 갈 수 없다.
                        // 끝난 것으로 둔다(이 상태에서 새 시험을 시작하지 않게. 세이브를 불러오면 처음으로 돌아간다).
                        g_phase = Phase::kDone;
                        g_watchStage = 0;
                        SKSE::log::error("저장 왕복: 불러오기가 시작된 뒤 끝나지 않아 키가 비워진 채임. 지켜보기를 그만둠");
                        Notify("[PoE Proto 5] 지켜보기를 이어 갈 수 없습니다. 플레이하던 세이브를 불러오세요.");
                    }
                }
            }
            return;
        }
        if (Clock::now() < g_nextAt) {
            return;
        }
        const auto& steps = Steps();
        while (g_step < steps.size() && steps[g_step].needsSaveLoad && !g_settings.saveLoad) {
            ++g_step;
        }
        if (g_step >= steps.size()) {
            g_running = false;
            if (g_phase == Phase::kExtra) {
                g_phase = Phase::kDone;
                SKSE::log::info("=== 마무리 단계 끝 (스레드 {})", Tid());
                Notify("[PoE Proto 5] 모두 끝났습니다. 플레이하던 세이브를 불러오세요(지금 상태로 저장하지 마세요).");
                return;
            }
            if (g_phase == Phase::kWatchSetup) {
                // 준비가 끝났다. 여기부터는 사용자가 직접 하고, 플러그인은 키를 따라가며 판정을 남긴다.
                g_phase = Phase::kWatch;
                g_watchStage = 0;
                SKSE::log::info("=== 준비 끝. 여기부터 지켜보기 (스레드 {})", Tid());
                WatchVerdict("준비 직후");
                LogCorpse("준비 끝");
                if (auto* corpse = CorpseActor(); corpse && !corpse->IsDead()) {
                    Notify("[PoE Proto 5] 스키버가 죽지 않았습니다. 직접 잡아 주세요(검은 그 안에 있습니다).");
                }
                Notify("[PoE Proto 5] 준비 끝.");
                NotifyWatchTodo();
                return;
            }
            if (g_phase == Phase::kWatchRoundTrip) {
                // 저장하고 불러왔다. 불러온 세이브에서 지켜보기를 이어 간다. 사용자가 맡겨 둔 것을 되찾는 동안의 판정이 남는다.
                g_phase = Phase::kWatch;
                g_watchStage = 1;
                SKSE::log::info("=== 저장 왕복 끝. 불러온 세이브에서 지켜보기를 이어 감 (스레드 {})", Tid());
                NotifyWatchTodo();
                return;
            }
            // 키가 살아 있는 동안에는 손으로 버리고 줍는 것도 기록된다. 인벤토리 메뉴와 조준해서 줍기가 어느 길을 타는지,
            // 그 길에서도 남긴 번호로 이어지는지를 보는 데 쓴다. 8차에서는 이 손 조작이 시험의 일부다.
            g_phase = Phase::kAwaitManual;
            SKSE::log::info("=== 자동 단계 끝. 여기부터 손 조작의 기록 (스레드 {})", Tid());
            Notify("[PoE Proto 5] 자동 시험 끝. 아직 세이브를 불러오지 마세요.");
            Notify("[PoE Proto 5] 인벤토리에서 'PoE5 name tag' 검을 버린 뒤, 조준해서 다시 주워 주세요.");
            return;
        }
        const auto& step = steps[g_step];
        ++g_step;
        SKSE::log::info("=== {} {}/{}: {} (스레드 {})",
            g_phase == Phase::kExtra          ? "마무리 단계" :
            g_phase == Phase::kWatchSetup     ? "지켜보기 준비" :
            g_phase == Phase::kWatchRoundTrip ? "지켜보기 저장 왕복" :
                                                "단계",
            g_step, steps.size(), step.name, Tid());
        g_nextAt = Clock::now() + std::chrono::milliseconds(step.waitMs);
        step.run();
    }

    // 결과에 영향을 줄 수 있는 다른 모드의 상태를 로그에 남긴다(아이템에 고유 ID를 붙이거나 옮기는 것들).
    // DLL이 로드됐는지와 플러그인 파일(esp)이 켜져 있는지를 따로 적는다. 둘은 다를 수 있다(4차 시험: DLL은 로드, esp는 꺼짐).
    // 그 모드가 제 설정에서 꺼져 있는지는 여기서 알 수 없으므로 그 모드의 로그를 봐야 한다.
    void LogCompanions()
    {
        struct Companion
        {
            const wchar_t* dll;
            const char*    dllName;
            const char*    plugin;
        };
        auto* data = RE::TESDataHandler::GetSingleton();
        for (const auto& companion : { Companion{ L"CalamityAffixes.dll", "CalamityAffixes.dll", "CalamityAffixes.esp" } }) {
            const bool dllLoaded = ::GetModuleHandleW(companion.dll) != nullptr;
            const bool pluginLoaded = data && (data->LookupLoadedModByName(companion.plugin) || data->LookupLoadedLightModByName(companion.plugin));
            SKSE::log::info("[환경] {}: {} / {}: {} (모드 자체 설정의 켜짐·꺼짐은 그 모드의 로그 참고)", companion.dllName,
                dllLoaded ? "로드됨" : "없음", companion.plugin, pluginLoaded ? "켜짐" : "꺼짐");
        }
    }

    void StartScenario()
    {
        if (g_running) {
            Notify("[PoE Proto 5] 이미 시험이 도는 중");
            return;
        }
        if (g_phase == Phase::kAwaitManual) {
            // 자동 단계는 끝났고 손 조작을 기다리는 중이다. 시작 키를 다시 누른 것은 무엇을 할지 몰라서일 수 있으므로 다시 알려 준다.
            Notify("[PoE Proto 5] 인벤토리에서 'PoE5 name tag' 검을 버린 뒤, 조준해서 다시 주워 주세요.");
            return;
        }
        if (g_phase == Phase::kWatch && g_settings.saveLoad && g_watchStage == 0) {
            // 첫 번째 시작 키: 지금 상태(아이템이 상인·동료·시체·상자에 있는 채)를 저장하고 그 세이브를 불러온다.
            if (auto* ui = RE::UI::GetSingleton(); !ui || ui->GameIsPaused()) {
                Notify("[PoE Proto 5] 메뉴를 닫고 게임 안에서 눌러 주세요");
                return;
            }
            g_phase = Phase::kWatchRoundTrip;
            g_roundTripSaveBase = g_watchSaveCount;
            g_step = 0;
            g_waitingForLoad = false;
            g_pumpQueued = false;
            g_nextAt = Clock::now();
            SKSE::log::info("=== 지켜보기: 저장 왕복을 시작함(사용자가 시작 키를 누름) (스레드 {})", Tid());
            Notify("[PoE Proto 5] 지금 상태를 저장하고 불러옵니다. 끝났다는 안내가 뜰 때까지 조작하지 말고 두세요.");
            g_running = true;
            return;
        }
        if (g_phase == Phase::kWatch) {
            // 지켜보기를 끝낸다. 마지막 판정을 남긴다.
            SKSE::log::info("=== 지켜보기 끝(사용자가 시작 키를 누름) (스레드 {})", Tid());
            WatchVerdict("끝");
            LogCorpse("끝");
            Observe("W9 지켜보기 끝");
            g_phase = Phase::kDone;
            Notify("[PoE Proto 5] 지켜보기 끝. 플레이하던 세이브를 불러오세요(지금 상태로 저장하지 마세요).");
            return;
        }
        if (g_phase == Phase::kDone) {
            // 시험이 끝난 세이브에 머물러 있다. 여기서 새 시험을 시작하지 않는다. 아래의 "이전 시험의 아이템" 검사는 플레이어의
            // 인벤토리만 보므로, 표시한 것을 상인·시체·상자에 둔 채 끝냈으면 통과해 버린다. 7~10차의 로그에서 시작 키 누름이
            // 매번 1초쯤 뒤에 한 번 더 들어왔으므로(까닭은 모름) 끝낸 직후의 두 번째 누름이 여기로 온다.
            // 세이브를 불러오면 되돌리기 콜백이 단계를 처음으로 돌린다.
            SKSE::log::info("시작하지 않음: 시험이 끝난 세이브에 머물러 있음(세이브를 불러온 뒤에 시작할 수 있음)");
            Notify("[PoE Proto 5] 시험은 끝났습니다. 플레이하던 세이브를 불러온 뒤에 새로 시작할 수 있습니다.");
            return;
        }
        const auto* player = RE::PlayerCharacter::GetSingleton();
        auto*       ui = RE::UI::GetSingleton();
        if (!player || !player->Is3DLoaded() || !ui || ui->GameIsPaused()) {
            Notify("[PoE Proto 5] 메뉴를 닫고 게임 안에서 눌러 주세요");
            return;
        }
        for (const auto id : { kIronSword, kIronHelmet, kIronIngot, kEmptyChest, kSkeever }) {
            if (id == kSkeever && !g_settings.watch) {
                continue;  // 시체는 지켜보기 시험에서만 쓴다
            }
            if (!TestForm(id)) {
                SKSE::log::error("시험에 쓸 폼을 찾지 못함: {:08X}", id);
                Notify("[PoE Proto 5] 시험에 쓸 폼을 찾지 못함(로그 확인)");
                return;
            }
        }
        // 이전 시험의 아이템이 남아 있는 세이브에서는 시작하지 않는다. 같은 꼬리표를 가진 아이템이 둘이 되면
        // 내용으로는 어느 쪽이 이번 것인지 가려낼 수 없다(3차 시험이 시험용 세이브에서 시작돼 그렇게 됐다).
        for (const auto mark : { Mark::kSwordUidTagA, Mark::kSwordUidTagB, Mark::kSwordName }) {
            if (FindIn(mark, RE::PlayerCharacter::GetSingleton(), "플레이어").extra) {
                SKSE::log::warn("시작하지 않음: 인벤토리에 이전 시험의 아이템({})이 남아 있음", kMarkNames[static_cast<std::size_t>(mark)]);
                Notify("[PoE Proto 5] 이전 시험의 아이템이 남은 세이브입니다. 플레이하던 세이브를 불러온 뒤 다시 누르세요");
                return;
            }
        }
        g_keys = {};
        g_lists = {};
        g_ownLists.clear();
        const auto generation = g_pick.generation + 1;
        g_pick = Pickup{};
        g_pick.generation = generation;
        g_phase = g_settings.watch ? Phase::kWatchSetup : Phase::kMain;
        g_inTestDrop = false;
        g_stampInEvent = {};
        g_hasContent = {};
        for (const auto mark : { Mark::kSwordUidTagA, Mark::kSwordUidTagB, Mark::kSwordName }) {
            g_hasContent[static_cast<std::size_t>(mark)] = true;  // 꼬리표나 이름표를 처음부터 가진 셋
        }
        g_corpseId = 0;
        g_watchStage = 0;
        g_watchColdLoad = false;
        g_afterLoadVerdictAt = 0;
        g_step = 0;
        g_waitingForLoad = false;
        g_pumpQueued = false;
        g_nextAt = Clock::now();
        if (g_settings.watch) {
            Notify("[PoE Proto 5] 지켜보기 시험을 준비합니다. 몇 초만 조작하지 말고 두세요. 끝나면 할 일을 알려 드립니다");
        } else {
            Notify("[PoE Proto 5] 시험을 시작합니다. 약 1분과 불러오기 세 번, 조작하지 말고 두세요. 끝나면 할 일을 알려 드립니다");
        }
        LogCompanions();
        SKSE::log::info("[환경] 시험: {} / 줍기 함수에 끼어들기: {} / 시험을 시작한 스레드 {}", g_settings.watch ? "지켜보기" : "자동",
            g_hookInstalled ? "켜짐" : "꺼짐", Tid());
        g_running = true;
    }

    // ---------------------------------------------------------------- 이벤트

    // 시험용 폼이 컨테이너 사이를 오갈 때 게임이 알려 주는 내용을 그대로 남긴다.
    class ContainerSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
    {
    public:
        static ContainerSink& Get()
        {
            static ContainerSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
        {
            if (!event || !IsTestForm(event->baseObj)) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // 기다리지 않고 잡아 본다. 못 잡았어도(다른 스레드가 상태를 쓰는 중) 이벤트는 처리하고, 그 사실을 남긴다.
            const StateLock lock(g_state, std::try_to_lock);
            if (!lock.owns_lock() && g_running) {
                SKSE::log::warn("[잠금] 컨테이너 이벤트: 다른 스레드가 상태를 쓰는 중. 잠그지 않고 처리 (스레드 {})", Tid());
            }
            if (!Observing()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            // 참조 칸의 값과, 그것을 핸들로 보고 참조를 얻을 수 있는지를 따로 적는다. 5차까지는 얻은 참조만 적어 모두 0이었고,
            // 6차에서 값 자체를 적어 보니 내려놓을 때의 값이 바닥 참조의 폼 ID와 같았다.
            const auto reference = event->reference.get();
            SKSE::log::info("[이벤트] 컨테이너 변경: {:08X} -> {:08X}, 폼 {:08X}, {}개, 고유ID {:04X}, 참조 칸 {:08X}, 핸들로 얻은 참조 {:08X} (스레드 {})",
                event->oldContainer, event->newContainer, event->baseObj, event->itemCount, event->uniqueID,
                event->reference.native_handle(), reference ? reference->GetFormID() : 0, Tid());
            const auto* player = RE::PlayerCharacter::GetSingleton();
            if (player && event->oldContainer == 0 && event->newContainer == player->GetFormID() && event->itemCount > 0) {
                AnalyzePickupEvent(*event);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        ContainerSink() = default;
    };

    // 엔진이 아이템의 고유 ID를 바꿀 때 보내는 이벤트. 시험용 폼의 것은 모두 남기고, 따라가던 키와 맞으면 새 값으로 옮긴다.
    class UniqueIdSink final : public RE::BSTEventSink<RE::TESUniqueIDChangeEvent>
    {
    public:
        static UniqueIdSink& Get()
        {
            static UniqueIdSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESUniqueIDChangeEvent* event, RE::BSTEventSource<RE::TESUniqueIDChangeEvent>*) override
        {
            if (!event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            if (!IsTestForm(event->objectID)) {
                return RE::BSEventNotifyControl::kContinue;  // 따라가는 키는 모두 시험용 폼의 것이다
            }
            const StateLock lock(g_state, std::try_to_lock);
            if (!lock.owns_lock() && g_running) {
                SKSE::log::warn("[잠금] 고유 ID 이벤트: 다른 스레드가 상태를 쓰는 중. 잠그지 않고 처리 (스레드 {})", Tid());
            }
            if (!Observing()) {
                return RE::BSEventNotifyControl::kContinue;
            }
            std::string followed;
            for (std::size_t i = 0; i < g_keys.size(); ++i) {
                auto& key = g_keys[i];
                if (key.valid && key.owner == event->oldBaseID && key.uid == event->oldUniqueID && event->objectID == FormOf(static_cast<Mark>(i))) {
                    const bool wasWorld = key.world;
                    key.owner = event->newBaseID;
                    key.uid = event->newUniqueID;
                    // 번호 0으로 옮겨 가는 것은 인벤토리가 아닌 곳, 곧 바닥의 참조로 나간 것이다(4차 시험: 버릴 때 새 가진 쪽이
                    // 바닥의 참조, 새 번호가 0).
                    key.world = event->newUniqueID == 0;
                    if (!wasWorld && key.world) {
                        // 이 표시가 바닥으로 나갔다. 시험 단계가 내려놓는 중이 아니고 시험도 돌고 있지 않으면 손으로 버린 것이다.
                        // 시험 단계가 요청한 것에는 지금 이 이벤트 안에서 번호를 남기고, 손으로 버린 것에는 다음 작업에서 남긴다.
                        const bool manual = !g_inTestDrop && !g_running;
                        if (manual || g_stampInEvent[i]) {
                            // 지켜보기 시험에서는 손으로 버린 것에도 이벤트 안에서 남긴다(결정 59의 남은 세부: 손으로 버리는
                            // 경로에서 이벤트 안에 남겨도 되는가). 8차의 손 조작에서는 다음 작업에서 남겼다.
                            StampOnDropEvent(i, event->newBaseID, event->objectID, g_stampInEvent[i] || g_phase == Phase::kWatch);
                        }
                        if (manual) {
                            SKSE::log::info("[손 조작] {} 을(를) 버림: 바닥의 참조 {:08X} (스레드 {})", kMarkNames[i], event->newBaseID, Tid());
                            Notify("[PoE Proto 5] 버린 것을 기록했습니다. 조준해서 다시 주워 주세요.");
                        }
                    }
                    if (wasWorld && !key.world) {
                        // 바닥에 있던 것의 키를 엔진이 인벤토리 쪽으로 옮겨 줬다. 번호를 남겨 둔 참조를 주울 때 이것이 오는지가 물음이다.
                        g_pick.marks.push_back({ kMethodUidEvent.data(), static_cast<int>(i) });
                    }
                    followed += followed.empty() ? "" : ", ";
                    followed += kMarkNames[i];
                }
            }
            SKSE::log::info("[이벤트] 고유ID 변경: {:08X}:{:04X} -> {:08X}:{:04X}, 폼 {:08X} (스레드 {}){}{}", event->oldBaseID, event->oldUniqueID,
                event->newBaseID, event->newUniqueID, event->objectID, Tid(), followed.empty() ? "" : " | 따라감: ", followed);
            if (g_phase == Phase::kWatch) {
                // 표시의 것이든 아니든, 시험용 폼의 번호가 바뀌었으면 다 옮겨진 뒤의 상태로 판정을 남긴다.
                QueueWatchVerdict(followed.empty() ? "시험용 폼의 고유 ID가 바뀜(따라간 표시 없음)" : "고유 ID 변경을 따라감: " + followed);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        UniqueIdSink() = default;
    };

    // 지켜보기 시험에서 어느 메뉴에서 일어난 일인지를 로그에 남기고, 메뉴가 닫힐 때 판정을 남긴다.
    // 제작(단련, 마법 부여)은 아이템을 옮기지 않아 이벤트가 오지 않을 수 있으므로, 메뉴가 닫힐 때의 판정이 그 경로를 잡는다.
    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MenuSink& Get()
        {
            static MenuSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const std::string_view name = event->menuName.c_str() ? event->menuName.c_str() : "";
            if (name != RE::BarterMenu::MENU_NAME && name != RE::ContainerMenu::MENU_NAME && name != RE::CraftingMenu::MENU_NAME &&
                name != RE::GiftMenu::MENU_NAME && name != RE::InventoryMenu::MENU_NAME) {
                return RE::BSEventNotifyControl::kContinue;
            }
            const StateLock lock(g_state, std::try_to_lock);
            if (g_phase != Phase::kWatch) {
                return RE::BSEventNotifyControl::kContinue;
            }
            SKSE::log::info("[메뉴] {} {} (스레드 {})", name, event->opening ? "열림" : "닫힘", Tid());
            if (!event->opening) {
                QueueWatchVerdict(std::string(name) + " 닫힘");
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        MenuSink() = default;
    };

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
            // 입력 이벤트가 오는 스레드를 한 번 적어 둔다. 단계와 이벤트의 스레드와 견줘 보려는 것이다.
            if (static bool logged = false; !logged) {
                logged = true;
                SKSE::log::info("[환경] 입력 이벤트가 오는 스레드 {}", Tid());
            }
            for (auto* event = *a_events; event; event = event->next) {
                const auto* button = event->AsButtonEvent();
                if (button && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == g_settings.startKey &&
                    button->IsDown()) {
                    if (const auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([] {
                            const auto lock = LockState("시험 시작", std::chrono::seconds(2));
                            StartScenario();
                        });
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        InputSink() = default;
    };

    // 플레이어의 줍기 함수(Actor::PickUpObject, 가상 함수 0xCC)에 끼어든다. 여기서는 주울 참조를 직접 알 수 있고,
    // 원래 함수를 부르기 전과 뒤의 인벤토리를 견줘 그 참조가 어느 묶음이 됐는지도 알 수 있다. 줍는 동작 자체는 바꾸지 않는다.
    // 같은 자리에 다른 모드가 끼어들어 있어도 서로를 차례로 부르므로 함께 동작한다.
    struct PickUpHook
    {
        static void thunk(RE::PlayerCharacter* a_this, RE::TESObjectREFR* a_object, std::int32_t a_count, bool a_arg3, bool a_playSound)
        {
            const auto*      base = a_object ? a_object->GetBaseObject() : nullptr;
            const RE::FormID baseId = base ? base->GetFormID() : 0;
            if (!IsTestForm(baseId)) {
                func(a_this, a_object, a_count, a_arg3, a_playSound);
                return;
            }
            // 기다리지 않고 잡아 본다. 못 잡았어도 보는 일은 하고, 그 사실을 남긴다.
            // 잡았으면 원래 함수가 끝날 때까지 쥐고 있는다. 그 안에서 오는 이벤트가 같은 스레드라면 다시 잡을 수 있다.
            const StateLock lock(g_state, std::try_to_lock);
            if (!lock.owns_lock() && g_running) {
                SKSE::log::warn("[잠금] 줍기 함수: 다른 스레드가 상태를 쓰는 중. 잠그지 않고 처리 (스레드 {})", Tid());
            }
            // 따라가던 표시가 바닥에 있는 폼일 때만 본다. 그 밖의 줍기는 그대로 지나간다.
            if (!HasGroundMark(baseId)) {
                // 손 조작을 지켜보는 동안에는 이런 줍기도 한 줄 남긴다. 손으로 버린 것이 변경 이벤트로 잡히지 않았다면
                // (그러면 바닥의 표시가 없는 것으로 보인다) 여기서 그 사실이 드러난다.
                if (!g_running && Observing()) {
                    SKSE::log::info("[줍기 함수] 따라가던 바닥의 표시가 없는 폼의 줍기: 참조 {:08X}, 폼 {:08X}, {} (스레드 {})", a_object->GetFormID(), baseId,
                        Describe(a_object->extraList), Tid());
                }
                func(a_this, a_object, a_count, a_arg3, a_playSound);
                return;
            }
            // 원래 함수가 참조를 지워도 뒤에서 상태를 읽을 수 있게 잡아 둔다.
            const RE::NiPointer<RE::TESObjectREFR> keep(a_object);
            const auto                             refId = a_object->GetFormID();
            if (!g_pick.automated) {
                ResetPickup(baseId, false, kNoAnswer);  // 손으로 주운 것
            }
            const int mark = GroundMark(refId);
            SKSE::log::info("[줍기 함수] 들어옴: 참조 {:08X}, 폼 {:08X}, {}개 요청, {} -> {} (스레드 {})", refId, baseId, a_count,
                Describe(a_object->extraList), MarkAnswer(mark), Tid());
            g_pick.inHook = true;
            g_pick.hookSaw = true;
            func(a_this, a_object, a_count, a_arg3, a_playSound);
            g_pick.inHook = false;

            auto*      object = TestForm(baseId);
            const auto fresh = FreshLists(a_this, object);
            auto*      list = fresh.size() == 1 ? fresh.front() : nullptr;
            SKSE::log::info("[줍기 함수] 나감: 새 묶음 {}개, 개수 {:+}, 참조는 {}{}", fresh.size(), TotalCount(a_this, object) - g_pick.countBefore,
                StateOfRef(refId).text, list ? ", 새 묶음 " + Describe(*list) : std::string());
            g_pick.marks.push_back({ "줍기 함수가 받은 참조", mark });
            g_pick.lists.push_back({ "줍기 함수의 앞뒤 차이", list, list ? std::string() : std::format("새 묶음이 {}개", fresh.size()) });
            // 키를 옮기는 일은 "줍기 뒤" 관찰 다음으로 미룬다(ApplyPendingLink의 설명). 컨테이너 이벤트가 맡긴 관찰 작업이
            // 원래 함수 안에서 먼저 맡겨졌으므로 이 작업은 그 뒤에 돈다.
            g_pick.hasPending = true;
            g_pick.pendingMark = mark;
            g_pick.pendingList = list;
            if (const auto* tasks = SKSE::GetTaskInterface()) {
                tasks->AddTask([generation = g_pick.generation] {
                    const auto lock = LockState("줍기 함수 쪽 절차", std::chrono::seconds(5));
                    ApplyPendingLink(generation);
                });
            } else {
                ApplyPendingLink(g_pick.generation);
            }
        }

        static inline REL::Relocation<decltype(thunk)> func;
    };

    void InstallPickUpHook()
    {
        if (!g_settings.hookPickUp) {
            SKSE::log::info("줍기 함수에 끼어들지 않음(설정 HookPickUp = false). 이벤트와 묶음 규칙만으로 잇는 절차를 봅니다.");
            return;
        }
        // 가상 함수 자리 0xCC: 이 플러그인이 5차까지 player->PickUpObject(...)로 불러 온 자리와 같다(같은 헤더로 컴파일된 호출이
        // 아이템을 실제로 주웠다). 플레이어의 가상 함수 표에 쓰는 방식은 프로토타입 2가 0xAD 자리에 써서 게임 안에서 확인했다.
        REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_PlayerCharacter[0] };
        PickUpHook::func = vtable.write_vfunc(0xCC, PickUpHook::thunk);
        g_hookInstalled = true;
        SKSE::log::info("줍기 함수에 끼어듦(PlayerCharacter 가상 함수 0xCC)");
    }

    // ---------------------------------------------------------------- 코세이브

    // 코세이브 콜백과 불러오기 알림에서 쓰는 잠금. 이 콜백들이 단계와 같은 스레드에서 불리는지는 모른다(저장 콜백은
    // 단계가 저장을 요청한 호출 안에서 불렸다). 다른 스레드에서 불리는데 단계가 그 끝을 기다리고 있다면 여기서 마냥 기다리면
    // 서로 물리므로, 잠깐만 기다리고 못 잡으면 잠그지 않은 채 진행한다.
    StateLock LockForCallback(const char* what)
    {
        return LockState(what, std::chrono::seconds(2));
    }

    // 지켜보기 시험의 기록을 쓴다: 결정 59의 키를 그대로. 가진 쪽이 무엇인지(플레이어, 상자, 상인의 상자, 동료, 시체, 바닥의
    // 참조)는 따지지 않고 폼 ID와 번호만 적는다.
    void SaveWatch(SKSE::SerializationInterface* intfc)
    {
        if (!intfc->OpenRecord(kRecordWatch, kWatchRecordVersion)) {
            SKSE::log::error("[코세이브] 저장: 지켜보기 레코드를 열지 못함");
            return;
        }
        // 저장 왕복이 만드는 세이브는 "저장 왕복 뒤"의 단계로 불려 온다. 그 밖의 저장(자동 저장 등)은 지금 단계 그대로.
        const auto    stage = static_cast<std::uint8_t>(g_phase == Phase::kWatchRoundTrip ? 1 : g_watchStage);
        std::uint8_t  content = 0;
        std::uint32_t count = 0;
        for (std::size_t i = 0; i < g_keys.size(); ++i) {
            if (g_hasContent[i]) {
                content = static_cast<std::uint8_t>(content | (1u << i));
            }
            if (g_keys[i].valid) {
                ++count;
            }
        }
        bool ok = intfc->WriteRecordData(stage) && intfc->WriteRecordData(content) && intfc->WriteRecordData(g_chestId) &&
                  intfc->WriteRecordData(g_corpseId) && intfc->WriteRecordData(count);
        for (std::size_t i = 0; i < g_keys.size(); ++i) {
            const auto& key = g_keys[i];
            if (!key.valid) {
                SKSE::log::info("[코세이브] 저장: {} 은(는) 키가 없어 적지 않음", kMarkNames[i]);
                continue;
            }
            const auto mark = static_cast<std::uint8_t>(i);
            const auto world = static_cast<std::uint8_t>(key.world ? 1 : 0);
            ok = ok && intfc->WriteRecordData(mark) && intfc->WriteRecordData(world) && intfc->WriteRecordData(key.uid) &&
                 intfc->WriteRecordData(key.owner);
            SKSE::log::info("[코세이브] 저장: {} 의 키 {} (가진 쪽 {})", kMarkNames[i], DescribeKey(static_cast<Mark>(i)), DescribeOwner(key.owner));
        }
        if (ok) {
            ++g_watchSaveCount;
        }
        SKSE::log::info("[코세이브] 저장: 지켜보기 키 {}건 {} (불려 올 단계 {}, 스레드 {})", count, ok ? "씀" : "쓰다가 실패", stage, Tid());
    }

    // 지켜보기 시험의 기록을 읽는다. 가진 쪽의 폼 ID는 게임에 다시 물어 푼다(경량 플러그인의 참조는 플러그인 순서가 바뀌면
    // 앞자리가 달라지고, 실행 중에 만들어진 참조는 그 세이브에 있어야 한다). 풀지 못한 키는 버리고 그 사실을 남긴다.
    void LoadWatch(SKSE::SerializationInterface* intfc)
    {
        std::uint8_t  stage = 0, content = 0;
        RE::FormID    chest = 0, corpse = 0;
        std::uint32_t count = 0;
        if (!intfc->ReadRecordData(stage) || !intfc->ReadRecordData(content) || !intfc->ReadRecordData(chest) || !intfc->ReadRecordData(corpse) ||
            !intfc->ReadRecordData(count) || count > g_keys.size()) {
            SKSE::log::error("[코세이브] 불러오기: 지켜보기 레코드의 머리를 읽지 못함");
            return;
        }
        const auto resolve = [&](RE::FormID saved, const char* what) {
            RE::FormID resolved = 0;
            if (saved == 0) {
                return resolved;
            }
            if (!intfc->ResolveFormID(saved, resolved)) {
                SKSE::log::warn("[코세이브] 불러오기: {} {:08X}을(를) 이 세이브에서 풀지 못함", what, saved);
                return RE::FormID{ 0 };
            }
            return resolved;
        };
        std::uint32_t restored = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint8_t  mark = 0, world = 0;
            std::uint16_t uid = 0;
            RE::FormID    owner = 0;
            if (!intfc->ReadRecordData(mark) || !intfc->ReadRecordData(world) || !intfc->ReadRecordData(uid) || !intfc->ReadRecordData(owner)) {
                SKSE::log::error("[코세이브] 불러오기: 지켜보기 키 {}번째를 읽다가 끝남", i);
                break;
            }
            if (mark >= g_keys.size()) {
                SKSE::log::error("[코세이브] 불러오기: 알 수 없는 표시 번호 {}", mark);
                continue;
            }
            const auto resolved = resolve(owner, "가진 쪽");
            if (resolved == 0) {
                SKSE::log::error("[코세이브] 불러오기: {} 의 키 {:08X}:{:04X} 를 되살리지 못함(가진 쪽을 풀지 못함)", kMarkNames[mark], owner, uid);
                continue;
            }
            g_keys[mark] = Key{ resolved, uid, true, world != 0 };
            ++restored;
            SKSE::log::info("[코세이브] 불러오기: {} 의 키: 저장 때 {:08X}:{:04X}{} -> 푼 뒤 {:08X}:{:04X} ({})", kMarkNames[mark], owner, uid,
                world != 0 ? "(바닥)" : "", resolved, uid, resolved == owner ? "폼 ID 그대로" : "폼 ID가 달라짐");
        }
        for (std::size_t i = 0; i < g_hasContent.size(); ++i) {
            g_hasContent[i] = (content & (1u << i)) != 0;
        }
        g_chestId = resolve(chest, "시험이 놓은 상자");
        g_chest = RE::ObjectRefHandle();
        g_corpseId = resolve(corpse, "시험이 놓은 스키버");
        g_watchStage = stage;
        if (g_phase != Phase::kWatchRoundTrip) {
            // 시험이 요청한 불러오기가 아니다(메뉴에서 불렀거나 게임을 다시 켠 뒤 불렀다). 지켜보기를 이 세이브에서 이어 간다.
            g_phase = Phase::kWatch;
            g_watchColdLoad = true;
        }
        SKSE::log::info("[코세이브] 불러오기: 지켜보기 키 {}건 가운데 {}건을 되살림 (단계 {}, 스레드 {})", count, restored, stage, Tid());
    }

    void OnSave(SKSE::SerializationInterface* intfc)
    {
        const auto lock = LockForCallback("[코세이브] 저장");
        if (g_phase == Phase::kWatch || g_phase == Phase::kWatchRoundTrip) {
            // 지켜보기 중의 저장은 시험이 요청한 것이든 게임의 자동 저장이든 키를 적는다. 그 세이브를 부르면 지켜보기가 이어진다.
            SaveWatch(intfc);
            return;
        }
        const auto marks = g_running ? CollectMarks() : std::vector<SavedMark>{};
        if (marks.empty()) {
            SKSE::log::info("[코세이브] 저장: 적을 기록이 없어 아무것도 쓰지 않음");
            return;
        }
        if (!intfc->OpenRecord(kRecordMarks, kRecordVersion)) {
            SKSE::log::error("[코세이브] 저장: 레코드를 열지 못함");
            return;
        }
        const auto count = static_cast<std::uint32_t>(marks.size());
        bool ok = intfc->WriteRecordData(count);
        for (const auto& saved : marks) {
            ok = ok && intfc->WriteRecordData(saved);
        }
        SKSE::log::info("[코세이브] 저장: 기록 {}건 {} (스레드 {})", count, ok ? "씀" : "쓰다가 실패", Tid());
    }

    void OnLoad(SKSE::SerializationInterface* intfc)
    {
        const auto lock = LockForCallback("[코세이브] 불러오기");
        g_cosaveLoaded = true;
        std::uint32_t type = 0, version = 0, length = 0;
        while (intfc->GetNextRecordInfo(type, version, length)) {
            if (type == kRecordWatch && version == kWatchRecordVersion) {
                LoadWatch(intfc);
                continue;
            }
            if (type != kRecordMarks || version != kRecordVersion) {
                SKSE::log::warn("[코세이브] 불러오기: 모르는 레코드 {:08X} v{} ({}바이트)는 건너뜀", type, version, length);
                continue;
            }
            std::uint32_t count = 0;
            if (!intfc->ReadRecordData(count) || count > 64) {
                SKSE::log::error("[코세이브] 불러오기: 기록 수를 읽지 못함");
                continue;
            }
            for (std::uint32_t i = 0; i < count; ++i) {
                SavedMark saved;
                if (!intfc->ReadRecordData(saved)) {
                    SKSE::log::error("[코세이브] 불러오기: {}번째 기록을 읽다가 끝남", i);
                    break;
                }
                // 상자는 실행 중에 만든 참조라 불러올 때 번호가 그대로인지 게임에 물어야 한다.
                RE::FormID resolved = 0;
                if (saved.chestRef != 0 && intfc->ResolveFormID(saved.chestRef, resolved)) {
                    saved.chestRef = resolved;
                }
                // 저장 때 적어 둔 고유 ID가 그 표시의 키다. 불러온 뒤 이 키로 다시 찾을 수 있는지가 저장 왕복의 답이다.
                // 키의 앞부분(가진 쪽)은 플레이어이거나 실행 중에 만든 상자라, 상자면 번호를 게임에 다시 묻는다.
                // 바닥에 있던 표시의 키는 그 참조의 폼 ID다. 실행 중에 만들어진 참조이므로 이것도 게임에 다시 묻는다.
                if (saved.mark < g_keys.size() && saved.where == kOnGround) {
                    RE::FormID ground = 0;
                    if (saved.groundRef != 0 && intfc->ResolveFormID(saved.groundRef, ground)) {
                        saved.groundRef = ground;
                        g_keys[saved.mark] = Key{ ground, saved.uniqueId, true, true };
                    } else {
                        SKSE::log::warn("[코세이브] 불러오기: {} 의 바닥 참조 {:08X}을(를) 이 세이브에서 되찾지 못함", kMarkNames[saved.mark],
                            saved.groundRef);
                        saved.groundRef = 0;
                    }
                } else if (saved.mark < g_keys.size() && saved.uniqueId != 0) {
                    RE::FormID owner = saved.uniqueBase;
                    if (saved.where == kInChest) {
                        owner = saved.chestRef;
                    }
                    g_keys[saved.mark] = Key{ owner, saved.uniqueId, true, false };
                }
                g_loadedMarks.push_back(saved);
            }
        }
        if (!g_loadedMarks.empty()) {
            g_chestId = g_loadedMarks.front().chestRef;
            g_chest = RE::ObjectRefHandle();
        }
        SKSE::log::info("[코세이브] 불러오기: 기록 {}건 (스레드 {})", g_loadedMarks.size(), Tid());
    }

    void OnRevert(SKSE::SerializationInterface*)
    {
        const auto lock = LockForCallback("[코세이브] 되돌리기");
        ++g_revertCount;
        g_loadedMarks.clear();
        g_cosaveLoaded = false;
        g_keys = {};
        g_lists = {};      // 불러오면 묶음이 새로 만들어진다. 예전 주소는 견줄 수 없다
        g_ownLists.clear();
        g_markRefs = {};
        // 시험이 요청하지 않은 불러오기(사용자의 빠른 불러오기 등)나 새 게임이 시험 도중에 일어났다. 남은 단계를 다른 세이브에서
        // 돌리면 엉뚱한 결과와 저장이 남으므로 여기서 멈춘다. 지금은 화면이 넘어가는 중이라 알림은 로그에만 남긴다.
        if (g_running && !g_waitingForLoad) {
            g_running = false;
            SKSE::log::warn("시험이 요청하지 않은 불러오기가 시험 도중에 일어나 시험을 멈춤({}단계까지 돎)", g_step);
        }
        // 시험이 요청한 불러오기(자동 단계의 일부)가 아니면 시험은 여기서 끝난 것이다. 손 조작을 기다리던 중이었으면
        // 손 조작 없이 세이브가 불려 온 것이므로 그 사실을 남긴다.
        if (!g_running) {
            if (g_phase == Phase::kAwaitManual) {
                SKSE::log::warn("손 조작(버리고 줍기)이 기록되기 전에 세이브가 불려 옴. 손 조작과 마무리 단계의 기록이 없음");
            } else if (g_phase == Phase::kWatch || g_phase == Phase::kWatchRoundTrip) {
                // 불려 오는 세이브에 지켜보기 기록이 있으면 바로 뒤의 불러오기 콜백이 지켜보기를 다시 켠다.
                SKSE::log::info("=== 지켜보기가 멈춤(세이브가 불려 옴. 시작 키로 끝내지 않아 마지막 판정은 없음)");
            }
            g_phase = Phase::kIdle;
            g_watchStage = 0;
        }
        g_watchColdLoad = false;
        g_afterLoadVerdictAt = 0;
        g_corpseId = 0;
        g_inTestDrop = false;
        g_stampInEvent = {};
        g_chest = RE::ObjectRefHandle();
        g_chestId = 0;
        // 보고 있던 줍기도 버린다. 번호(generation)는 이어 가서, 불러오기 전에 맡겨 둔 "줍기 뒤" 작업이 뒤늦게 돌아도 아무것도 하지 않게 한다.
        const auto generation = g_pick.generation + 1;
        g_pick = Pickup{};
        g_pick.generation = generation;
        SKSE::log::info("[코세이브] 되돌리기({}번째): 이전 세이브에서 온 상태를 비움 (스레드 {})", g_revertCount, Tid());
    }

    void OnMessage(SKSE::MessagingInterface::Message* message)
    {
        if (!message) {
            return;
        }
        switch (message->type) {
        case SKSE::MessagingInterface::kInputLoaded:
            if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
                input->AddEventSink(&InputSink::Get());
            } else {
                SKSE::log::error("입력 장치 관리자를 찾지 못함. 키로 시작할 수 없습니다.");
            }
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            SKSE::log::info("[환경] 데이터 로드 알림을 받은 스레드 {}", Tid());
            InstallPickUpHook();
            // 프로세스가 끝날 때까지 도는 스레드. 시험이 돌지 않을 때는 자기만 한다.
            std::thread(TickerMain).detach();
            if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
                events->AddEventSink<RE::TESContainerChangedEvent>(&ContainerSink::Get());
                events->AddEventSink<RE::TESUniqueIDChangeEvent>(&UniqueIdSink::Get());
            } else {
                SKSE::log::error("이벤트 원본을 찾지 못함. 컨테이너와 고유 ID 이벤트를 받지 못합니다.");
            }
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(&MenuSink::Get());
            } else {
                SKSE::log::error("UI를 찾지 못함. 메뉴가 열리고 닫히는 것을 받지 못합니다(지켜보기 시험의 메뉴 판정이 빠짐).");
            }
            break;
        case SKSE::MessagingInterface::kPostLoadGame: {
            const auto lock = LockForCallback("[알림] 불러오기 끝");
            SKSE::log::info("[알림] 불러오기 끝(kPostLoadGame), 성공 {} (스레드 {})", message->data != nullptr, Tid());
            if (g_running && g_waitingForLoad && message->data == nullptr) {
                // 실패했다는 알림이다. 불러오지 않은 상태를 불러온 것으로 보고 견주면 안 되므로 대기를 풀지 않는다.
                // 이 모드팩에서는 실패 알림 뒤에 곧 성공 알림이 오는 경우가 있었다. 끝내 오지 않으면 제한 시간이 시험을 멈춘다.
                SKSE::log::warn("불러오기가 실패했다는 알림. 성공 알림을 계속 기다림");
            } else if (g_running && g_waitingForLoad) {
                // 대기를 풀고, 화면이 자리를 잡을 시간을 조금 둔다. 불러오기 전에 맡겨 둔 작업이 버려졌을 수 있으니 표시도 지운다.
                g_nextAt = Clock::now() + std::chrono::milliseconds(2500);
                g_pumpQueued = false;
                g_waitingForLoad = false;
            } else if (!g_running && g_watchColdLoad && message->data != nullptr) {
                // 지켜보기 기록이 든 세이브가 시험의 요청 없이 불려 왔다. 화면이 자리를 잡은 뒤 판정을 남기도록 맡긴다.
                g_watchColdLoad = false;
                g_afterLoadRevert = g_revertCount;
                g_afterLoadVerdictAt = (Clock::now() + std::chrono::seconds(4)).time_since_epoch().count();
            }
            break;
        }
        case SKSE::MessagingInterface::kNewGame:
            SKSE::log::info("[알림] 새 게임(kNewGame)");
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    // 로그는 InitializeLogging이 직접 연다. SKSE::Init에 맡기면(기본값) 플러그인 이름의 로그 파일을 먼저 열면서 비워 버려,
    // 지난 로그를 남기는 처리가 빈 파일만 보게 된다(5차 시험에서 지난 로그가 남지 않아 CommonLibSSE-NG의 API.cpp로 확인).
    SKSE::Init(skse, { .log = false });
    InitializeLogging();

    g_settings = LoadSettings();
    SKSE::log::info("설정: StartKey=0x{:02X} SaveLoad={} HookPickUp={} Scenario={}", g_settings.startKey, g_settings.saveLoad,
        g_settings.hookPickUp, g_settings.watch ? "watch" : "auto");

    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("SKSE 메시지 리스너를 등록하지 못함. 시험을 시작할 수 없습니다.");
        return true;
    }
    if (const auto* serialization = SKSE::GetSerializationInterface()) {
        serialization->SetUniqueID(kCosaveId);
        serialization->SetSaveCallback(OnSave);
        serialization->SetLoadCallback(OnLoad);
        serialization->SetRevertCallback(OnRevert);
    } else {
        SKSE::log::error("SKSE 저장 인터페이스를 얻지 못함. 코세이브 단계는 기록이 남지 않습니다.");
    }
    SKSE::log::info("PoEProtoItems 준비 완료");
    return true;
}
