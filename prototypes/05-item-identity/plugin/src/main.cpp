// PoE Proto 5: 아이템의 동일성과 저장 왕복을 관찰하는 시험용 플러그인.
// 키를 한 번 누르면 정해진 절차를 스스로 차례로 돌린다. 시험용 아이템에 표시를 붙이고, 상자로 옮기고, 바닥에 버렸다 줍고,
// 장착했다 풀고, 저장하고 불러온다. 단계마다 인벤토리의 상태를 로그에 남겨, 어떤 표시가 어떤 경로에서 남는지를 본다.
// 여기서 정하려는 것은 "어픽스가 붙은 아이템을 무엇으로 다시 찾을 것인가"이고, 이 플러그인은 그 판단에 쓸 사실을 모은다.

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
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <string_view>
#include <thread>
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
    // 시험이 만드는 세이브의 이름. 플레이하던 세이브는 건드리지 않는다.
    constexpr auto kBaselineSave = "PoEProto5_baseline";
    constexpr auto kMarkedSave = "PoEProto5_marked";
    // 코세이브의 플러그인 표식과 레코드.
    constexpr std::uint32_t kCosaveId = 'PPI5';
    constexpr std::uint32_t kRecordMarks = 'MARK';
    constexpr std::uint32_t kRecordVersion = 1;
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
    struct SavedMark
    {
        std::uint8_t  mark = 0;
        std::uint8_t  inChest = 0;
        std::uint16_t uniqueId = 0;
        RE::FormID    uniqueBase = 0;
        RE::FormID    chestRef = 0;
    };

    // 표시한 아이템을 가리키는 키: 가진 쪽의 폼 ID와 그 안에서의 번호. 엔진의 ExtraUniqueID가 담는 값과 같은 모양이다.
    // 엔진이 번호를 바꿀 때 보내는 이벤트(TESUniqueIDChangeEvent)로 따라간다.
    struct Key
    {
        RE::FormID    owner = 0;
        std::uint16_t uid = 0;
        bool          valid = false;
    };

    struct Found
    {
        RE::TESObjectREFR* owner = nullptr;
        RE::ExtraDataList* extra = nullptr;
        const char*        where = "";
    };

    Settings                g_settings;
    // 아래 넷은 진행을 깨우는 스레드도 읽는다. 나머지 상태는 게임의 메인 스레드에서만 만진다.
    std::atomic<bool>       g_running{ false };
    std::atomic<bool>       g_waitingForLoad{ false };
    std::atomic<bool>       g_pumpQueued{ false };
    std::atomic<Clock::rep> g_loadDeadline{ 0 };
    std::size_t             g_step = 0;
    Clock::time_point       g_nextAt{};
    RE::ObjectRefHandle     g_chest;
    RE::FormID              g_chestId = 0;
    RE::ObjectRefHandle     g_dropped;
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

    RE::TESBoundObject* TestForm(RE::FormID id)
    {
        return RE::TESForm::LookupByID<RE::TESBoundObject>(id);
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

    RE::InventoryEntryData* FindEntry(RE::TESObjectREFR* owner, RE::TESBoundObject* object)
    {
        auto* changes = owner ? owner->GetInventoryChanges() : nullptr;
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

    bool HasNameTag(const RE::ExtraDataList& extra)
    {
        const auto* text = extra.GetByType<RE::ExtraTextDisplayData>();
        return text && text->displayName.c_str() && std::strcmp(text->displayName.c_str(), kNameTag) == 0;
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
        return key.valid ? std::format("{:08X}:{:04X}", key.owner, key.uid) : std::string("없음");
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
                SKSE::log::info("[추적:{}] {} -> 찾지 못함 | 키 {}", label, kMarkNames[i], DescribeKey(mark));
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

    void StepDrop()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto found = FindIn(Mark::kSwordUidTagA, player, "플레이어");
        if (!found.extra) {
            SKSE::log::info("버리기: 검/고유ID+꼬리표A 를 찾지 못해 건너뜀");
            return;
        }
        g_dropped = player->RemoveItem(TestForm(kIronSword), 1, RE::ITEM_REMOVE_REASON::kDropping, found.extra, nullptr);
        Observe("6 검(꼬리표A)을 바닥에 버림");
    }

    void StepPickUp()
    {
        auto dropped = g_dropped.get();
        if (!dropped) {
            SKSE::log::info("줍기: 버린 아이템의 참조를 찾지 못함(버리기가 참조를 만들지 않았거나 이미 사라짐)");
            return;
        }
        SKSE::log::info("[바닥] 참조 {:08X}의 부가 데이터: {}", dropped->GetFormID(), Describe(dropped->extraList));
        RE::PlayerCharacter::GetSingleton()->PickUpObject(dropped.get(), 1, false, false);
        g_dropped = RE::ObjectRefHandle();
        Observe("7 바닥의 검을 다시 주움");
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
            const auto found = Locate(static_cast<Mark>(i));
            if (!found.extra) {
                continue;
            }
            SavedMark saved;
            saved.mark = static_cast<std::uint8_t>(i);
            saved.inChest = found.owner == Chest() ? 1 : 0;
            if (const auto* unique = found.extra->GetByType<RE::ExtraUniqueID>()) {
                saved.uniqueId = unique->uniqueID;
                saved.uniqueBase = unique->baseID;
            }
            saved.chestRef = g_chestId;
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
        const char*           name;
        std::function<void()> run;
        int                   waitMs;       // 이 단계를 한 뒤 다음 단계까지 기다리는 시간
        bool                  needsSaveLoad;
    };

    const std::vector<Step>& Steps()
    {
        static const std::vector<Step> steps{
            { "기준 세이브 저장", [] { RequestSave(kBaselineSave); }, 6000, true },
            { "준비", StepSetup, 500, false },
            { "표시 1/3: 내려놓기", StepMarkDrop, 1500, false },
            { "표시 2/3: 꼬리표 붙여 줍기", StepMarkTagAndPickUp, 1000, false },
            { "표시 3/3: 고유 ID 붙이기", StepMarkUnique, 500, false },
            { "합치기", StepAddPlain, 500, false },
            { "상자로", StepToChest, 700, false },
            { "상자에서", StepFromChest, 700, false },
            { "버리기", StepDrop, 1500, false },
            { "줍기", StepPickUp, 700, false },
            { "투구 장착", [] { SetHelmetEquipped(true); }, 800, false },
            { "장착 뒤 관찰", [] { Observe("8 투구를 장착함"); }, 200, false },
            { "투구 해제", [] { SetHelmetEquipped(false); }, 800, false },
            { "해제 뒤 관찰", [] { Observe("9 투구를 해제함"); }, 200, false },
            { "판매", StepSell, 700, false },
            { "되찾기", StepBuyBack, 700, false },
            { "표시한 세이브 저장", [] { RequestSave(kMarkedSave); }, 6000, true },
            { "표시한 세이브 불러오기", [] { RequestLoad(kMarkedSave); }, 3000, true },
            { "불러온 뒤 관찰(표시한 세이브)",
                [] {
                    Observe("12 표시한 세이브를 불러옴");
                    CompareWithCosave("표시한 세이브");
                },
                500, true },
            { "기준 세이브 불러오기", [] { RequestLoad(kBaselineSave); }, 3000, true },
            { "불러온 뒤 관찰(기준 세이브)",
                [] {
                    Observe("13 기준 세이브를 불러옴(표시가 없어야 함)");
                    CompareWithCosave("기준 세이브");
                },
                500, true },
            { "표시한 세이브 다시 불러오기", [] { RequestLoad(kMarkedSave); }, 3000, true },
            { "불러온 뒤 관찰(표시한 세이브, 두 번째)",
                [] {
                    Observe("14 표시한 세이브를 다시 불러옴");
                    CompareWithCosave("표시한 세이브(두 번째)");
                },
                500, true },
        };
        return steps;
    }

    void Pump();

    // 메인 스레드에서 Pump를 한 번 돌리도록 맡긴다. 이미 맡긴 것이 있으면 더 쌓지 않는다.
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
            Pump();
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
            if (!g_running) {
                continue;
            }
            if (g_waitingForLoad && Clock::now().time_since_epoch().count() < g_loadDeadline) {
                continue;
            }
            QueuePump();
        }
    }

    // 메인 스레드에서. 기다릴 시간이 지났으면 다음 단계를 돌린다.
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
                Notify("[PoE Proto 5] 불러오기가 끝나지 않아 시험을 멈춤(로그 확인)");
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
            Notify("[PoE Proto 5] 시험 끝. 플레이하던 세이브를 다시 불러오세요(지금 상태로 저장하지 마세요).");
            return;
        }
        const auto& step = steps[g_step];
        ++g_step;
        SKSE::log::info("=== 단계 {}/{}: {}", g_step, steps.size(), step.name);
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
        const auto* player = RE::PlayerCharacter::GetSingleton();
        auto*       ui = RE::UI::GetSingleton();
        if (!player || !player->Is3DLoaded() || !ui || ui->GameIsPaused()) {
            Notify("[PoE Proto 5] 메뉴를 닫고 게임 안에서 눌러 주세요");
            return;
        }
        for (const auto id : { kIronSword, kIronHelmet, kIronIngot, kEmptyChest }) {
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
        g_step = 0;
        g_waitingForLoad = false;
        g_pumpQueued = false;
        g_nextAt = Clock::now();
        Notify("[PoE Proto 5] 시험을 시작합니다. 끝날 때까지 약 40초와 불러오기 세 번, 조작하지 말고 두세요");
        LogCompanions();
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
            if (event && g_running &&
                (event->baseObj == kIronSword || event->baseObj == kIronHelmet || event->baseObj == kIronIngot)) {
                const auto reference = event->reference.get();
                SKSE::log::info("[이벤트] 컨테이너 변경: {:08X} -> {:08X}, 폼 {:08X}, {}개, 고유ID {:04X}, 참조 {:08X}", event->oldContainer,
                    event->newContainer, event->baseObj, event->itemCount, event->uniqueID, reference ? reference->GetFormID() : 0);
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
            if (!event || !g_running) {
                return RE::BSEventNotifyControl::kContinue;
            }
            std::string followed;
            for (std::size_t i = 0; i < g_keys.size(); ++i) {
                auto& key = g_keys[i];
                if (key.valid && key.owner == event->oldBaseID && key.uid == event->oldUniqueID && event->objectID == FormOf(static_cast<Mark>(i))) {
                    key.owner = event->newBaseID;
                    key.uid = event->newUniqueID;
                    followed += followed.empty() ? "" : ", ";
                    followed += kMarkNames[i];
                }
            }
            if (!followed.empty() || event->objectID == kIronSword || event->objectID == kIronHelmet || event->objectID == kIronIngot) {
                SKSE::log::info("[이벤트] 고유ID 변경: {:08X}:{:04X} -> {:08X}:{:04X}, 폼 {:08X}{}{}", event->oldBaseID, event->oldUniqueID,
                    event->newBaseID, event->newUniqueID, event->objectID, followed.empty() ? "" : " | 따라감: ", followed);
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        UniqueIdSink() = default;
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
            for (auto* event = *a_events; event; event = event->next) {
                const auto* button = event->AsButtonEvent();
                if (button && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == g_settings.startKey &&
                    button->IsDown()) {
                    if (const auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([] { StartScenario(); });
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        InputSink() = default;
    };

    // ---------------------------------------------------------------- 코세이브

    void OnSave(SKSE::SerializationInterface* intfc)
    {
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
        SKSE::log::info("[코세이브] 저장: 기록 {}건 {}", count, ok ? "씀" : "쓰다가 실패");
    }

    void OnLoad(SKSE::SerializationInterface* intfc)
    {
        g_cosaveLoaded = true;
        std::uint32_t type = 0, version = 0, length = 0;
        while (intfc->GetNextRecordInfo(type, version, length)) {
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
                if (saved.mark < g_keys.size() && saved.uniqueId != 0) {
                    RE::FormID owner = saved.uniqueBase;
                    if (saved.inChest != 0) {
                        owner = saved.chestRef;
                    }
                    g_keys[saved.mark] = Key{ owner, saved.uniqueId, true };
                }
                g_loadedMarks.push_back(saved);
            }
        }
        if (!g_loadedMarks.empty()) {
            g_chestId = g_loadedMarks.front().chestRef;
            g_chest = RE::ObjectRefHandle();
        }
        SKSE::log::info("[코세이브] 불러오기: 기록 {}건", g_loadedMarks.size());
    }

    void OnRevert(SKSE::SerializationInterface*)
    {
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
        g_chest = RE::ObjectRefHandle();
        g_chestId = 0;
        g_dropped = RE::ObjectRefHandle();
        SKSE::log::info("[코세이브] 되돌리기({}번째): 이전 세이브에서 온 상태를 비움", g_revertCount);
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
            // 프로세스가 끝날 때까지 도는 스레드. 시험이 돌지 않을 때는 자기만 한다.
            std::thread(TickerMain).detach();
            if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
                events->AddEventSink<RE::TESContainerChangedEvent>(&ContainerSink::Get());
                events->AddEventSink<RE::TESUniqueIDChangeEvent>(&UniqueIdSink::Get());
            } else {
                SKSE::log::error("이벤트 원본을 찾지 못함. 컨테이너와 고유 ID 이벤트를 받지 못합니다.");
            }
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            SKSE::log::info("[알림] 불러오기 끝(kPostLoadGame), 성공 {}", message->data != nullptr);
            if (g_running && g_waitingForLoad && message->data == nullptr) {
                // 실패했다는 알림이다. 불러오지 않은 상태를 불러온 것으로 보고 견주면 안 되므로 대기를 풀지 않는다.
                // 이 모드팩에서는 실패 알림 뒤에 곧 성공 알림이 오는 경우가 있었다. 끝내 오지 않으면 제한 시간이 시험을 멈춘다.
                SKSE::log::warn("불러오기가 실패했다는 알림. 성공 알림을 계속 기다림");
            } else if (g_running && g_waitingForLoad) {
                // 대기를 풀고, 화면이 자리를 잡을 시간을 조금 둔다. 불러오기 전에 맡겨 둔 작업이 버려졌을 수 있으니 표시도 지운다.
                g_nextAt = Clock::now() + std::chrono::milliseconds(2500);
                g_pumpQueued = false;
                g_waitingForLoad = false;
            }
            break;
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
    SKSE::log::info("설정: StartKey=0x{:02X} SaveLoad={}", g_settings.startKey, g_settings.saveLoad);

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
