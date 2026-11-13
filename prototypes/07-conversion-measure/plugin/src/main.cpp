// PoE Proto 7: 환산의 값을 정하기 위한 측정 플러그인.
// 환산의 식(결정 62)에 들어갈 값(기준 피해 B, 기준 타격 수 N, 기준 타격률)을 정하려면 이 모드 목록의 실제 수치가 필요하다.
// 이 플러그인은 평소 플레이를 옆에서 기록만 한다. 게임에 아무것도 놓지 않고 아무 값도 바꾸지 않는다.
//   1) 주변 액터를 주기적으로 훑어 레벨과 최대 체력을 적는다(적의 체력 표).
//   2) 액터의 체력 피해 가상 함수에 끼어들어 호출마다 대상, 공격자, 받은 값, 앞뒤의 체력을 적는다.
//   3) 엔진의 적중, 사망, 전투 상태 이벤트를 그대로 적는다.
//   4) 플레이어의 레벨, 난이도 배율, 손에 든 것, 있는 곳이 바뀔 때마다 적는다.
// 걸러 내지 않는다. 무엇을 적으로 볼지, 무엇을 타격 한 번으로 볼지는 분석(tools/analyze.py)에서 정한다.
// 걸러 내는 조건이 틀리면 플레이 한 번을 잃기 때문이다.
// 기록의 형식과 값을 정하는 법은 README에 있다. 빌드하는 CommonLibSSE-NG는 저장소의 서브모듈(v16.4.0, 결정 60)이다.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoMeasure.ini";
    constexpr auto kLogName = "PoEProtoMeasure";
    constexpr auto kDataFolder = "PoEProtoMeasure";
    constexpr auto kPluginVersion = "0.1.0";
    constexpr auto kTick = std::chrono::milliseconds(250);  // 이 간격으로 기록을 디스크에 내려 쓰고, 훑을 때가 됐는지 본다
    constexpr RE::FormID  kPlayerRef = 0x00000014;
    constexpr std::size_t kHealthDamageSlot = 0x104;  // Actor::HandleHealthDamage (v16.4.0의 Actor.h)

    // 상태 키가 보여 주는 목표(README의 "충분한 표본").
    constexpr int           kGoalHostiles = 150;
    constexpr int           kGoalLowHostiles = 30;
    constexpr int           kGoalDeaths = 40;
    constexpr int           kGoalHits = 300;
    constexpr int           kGoalReceived = 100;
    constexpr std::uint16_t kLowLevel = 10;

    struct Settings
    {
        std::uint32_t statusKey = 0x3E;    // 키보드 DirectInput 스캔 코드. 0x3E = F4, 0 = 쓰지 않음
        bool          hookDamage = true;   // 체력 피해 함수에 끼어든다
        float         scanSeconds = 1.0f;  // 주변 액터를 훑는 간격
        bool          notify = true;       // 게임에 들어갔을 때 알림을 띄운다
        std::uint32_t maxSessionMB = 256;  // 기록 파일 하나의 한도
    };

    // 액터의 표시. 기록의 A 줄에는 글자로 적는다.
    enum ActorBits : std::uint32_t
    {
        kHostile = 1 << 0,    // H: 플레이어에게 적대(훑을 때만 판정한다)
        kTeammate = 1 << 1,   // T: 동료
        kCommanded = 1 << 2,  // C: 조종받는 액터
        kSummoned = 1 << 3,   // S: 소환된 것
        kEssential = 1 << 4,  // E
        kProtected = 1 << 5,  // P
        kUnique = 1 << 6,     // U
        kDead = 1 << 7,       // D
        kInCombat = 1 << 8,   // X
        kHostileKnown = 1 << 9,  // 적대 여부를 판정한 표시(글자로는 적지 않는다)
    };
    // 이 표시가 달라지면 그 액터를 다시 설명한다. 죽음이 들어 있는 까닭: 죽은 뒤 같은 폼 ID가 산 액터로 다시 보이면
    // 다른 액터(임시 참조의 폼 ID가 다시 쓰인 것)로 봐야 하기 때문이다.
    constexpr std::uint32_t kKeyBits = kHostile | kTeammate | kCommanded | kSummoned | kDead | kHostileKnown;

    struct Snapshot
    {
        RE::FormID    id = 0;
        RE::FormID    base = 0;
        std::uint16_t level = 0;
        float         maxHealth = 0.0f;
        float         health = 0.0f;
        std::uint32_t bits = 0;
    };

    struct Known
    {
        RE::FormID    base = 0;
        std::uint16_t level = 0;
        long          maxHealth = 0;  // 반올림한 값
        std::uint32_t keyBits = 0;
        float         lastHealth = 0.0f;  // 훑기가 마지막으로 적은 체력
    };

    Settings                g_settings;
    const Clock::time_point g_epoch = Clock::now();

    // 기록. 줄은 어느 스레드에서든 g_buffer에 붙이고, 내려 쓰는 스레드(Ticker)만 파일을 다룬다.
    std::mutex                 g_bufferMutex;
    std::string                g_buffer;
    std::ofstream              g_file;
    std::filesystem::path      g_dataPath;
    std::atomic<bool>          g_recording{ false };
    std::atomic<std::uint64_t> g_bytes{ 0 };
    std::mutex                 g_stopMutex;
    std::string                g_stopReason = "기록 파일을 열지 못함";
    std::atomic<bool>          g_stopNoticePending{ false };
    std::atomic<bool>          g_loadNoticePending{ false };
    std::atomic<bool>          g_scanQueued{ false };
    bool                       g_hookInstalled = false;

    // 본 액터와 폼. 엔진의 값을 읽는 동안에는 이 잠금을 쥐지 않는다(끼어든 자리에서 엔진의 잠금과 엇갈리지 않게).
    std::mutex                            g_knownMutex;
    std::unordered_map<RE::FormID, Known> g_known;
    std::unordered_set<RE::FormID>        g_knownForms;
    std::unordered_set<std::uint64_t>     g_countedHostiles;  // 세이브를 불러와도 남긴다(같은 적을 두 번 세지 않게)
    std::unordered_set<RE::FormID>        g_countedDeaths;
    std::string                           g_lastContext;  // 훑는 작업만 다룬다

    std::atomic<int> g_hostiles{ 0 };
    std::atomic<int> g_lowHostiles{ 0 };
    std::atomic<int> g_deaths{ 0 };
    std::atomic<int> g_playerHits{ 0 };
    std::atomic<int> g_received{ 0 };

    unsigned long Tid() { return ::GetCurrentThreadId(); }

    std::int64_t Micros() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_epoch).count(); }

    std::string PathText(const std::filesystem::path& path)
    {
        const auto text = path.u8string();
        return { reinterpret_cast<const char*>(text.data()), text.size() };
    }

    // ---------------------------------------------------------------- 로그와 설정

    // 상태 로그는 직접 연다(SKSE::Init에 맡기면 같은 이름의 파일을 먼저 비운다). 지난 로그는 하나 남긴다.
    bool InitializeLogging()
    {
        auto directory = SKSE::log::log_directory();
        if (!directory) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(*directory, error);
        const auto path = *directory / std::format("{}.log", kLogName);
        std::filesystem::rename(path, *directory / std::format("{}.previous.log", kLogName), error);
        try {
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
            auto logger = std::make_shared<spdlog::logger>("poe_proto_measure", std::move(sink));
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
        return first == std::string_view::npos ? std::string_view{} : text.substr(first, text.find_last_not_of(kSpace) - first + 1);
    }

    bool ParseBool(std::string_view key, std::string_view value, bool& out)
    {
        if (value == "true" || value == "1") {
            out = true;
        } else if (value == "false" || value == "0") {
            out = false;
        } else {
            SKSE::log::warn("설정: {} '{}'는 true 또는 false가 아니어서 기본값 유지", key, value);
            return false;
        }
        return true;
    }

    bool ParseUnsigned(std::string_view value, std::uint32_t& out)
    {
        const bool hex = value.starts_with("0x") || value.starts_with("0X");
        const auto digits = hex ? value.substr(2) : value;
        std::uint32_t parsed = 0;
        const auto    result = std::from_chars(digits.data(), digits.data() + digits.size(), parsed, hex ? 16 : 10);
        if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
            return false;
        }
        out = parsed;
        return true;
    }

    Settings LoadSettings()
    {
        Settings      settings;
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
            const auto equals = text.find('=');
            if (text.empty() || text.front() == '[' || equals == std::string_view::npos) {
                continue;
            }
            const auto key = Trim(text.substr(0, equals));
            const auto value = Trim(text.substr(equals + 1));
            if (key == "StatusKey") {
                std::uint32_t code = 0;
                if (ParseUnsigned(value, code) && code <= 255) {
                    settings.statusKey = code;
                } else {
                    SKSE::log::warn("설정: StatusKey '{}'는 0~255의 키 코드가 아니어서 기본값 유지", value);
                }
            } else if (key == "HookDamage") {
                ParseBool(key, value, settings.hookDamage);
            } else if (key == "Notify") {
                ParseBool(key, value, settings.notify);
            } else if (key == "ScanSeconds") {
                float      seconds = 0.0f;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), seconds);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && seconds >= 0.5f && seconds <= 10.0f) {
                    settings.scanSeconds = seconds;
                } else {
                    SKSE::log::warn("설정: ScanSeconds '{}'는 0.5 이상 10 이하의 수가 아니어서 기본값 유지", value);
                }
            } else if (key == "MaxSessionMB") {
                std::uint32_t megabytes = 0;
                if (ParseUnsigned(value, megabytes) && megabytes >= 16 && megabytes <= 4096) {
                    settings.maxSessionMB = megabytes;
                } else {
                    SKSE::log::warn("설정: MaxSessionMB '{}'는 16 이상 4096 이하의 수가 아니어서 기본값 유지", value);
                }
            } else {
                SKSE::log::warn("설정: 알 수 없는 키 '{}'", key);
            }
        }
        return settings;
    }

    void Notify(const std::string& message)
    {
        RE::SendHUDMessage::ShowHUDMessage(message.c_str());
        SKSE::log::info("{}", message);
    }

    std::string StatusKeyName()
    {
        return g_settings.statusKey == 0x3E ? std::string("F4") : std::format("상태 키(스캔 코드 0x{:02X})", g_settings.statusKey);
    }

    // ---------------------------------------------------------------- 기록 파일

    void Append(std::string_view row)
    {
        if (!g_recording.load(std::memory_order_relaxed)) {
            return;
        }
        const std::scoped_lock lock(g_bufferMutex);
        g_buffer.append(row);
    }

    void StopRecording(std::string reason)
    {
        if (!g_recording.exchange(false)) {
            return;
        }
        SKSE::log::error("기록을 멈춤: {}", reason);
        {
            const std::scoped_lock lock(g_stopMutex);
            g_stopReason = std::move(reason);
        }
        g_stopNoticePending = true;
    }

    // 모인 줄을 디스크에 내려 쓴다. 내려 쓰는 스레드에서만 부른다.
    void Flush()
    {
        std::string pending;
        {
            const std::scoped_lock lock(g_bufferMutex);
            pending.swap(g_buffer);
        }
        if (pending.empty() || !g_file.is_open()) {
            return;
        }
        g_file.write(pending.data(), static_cast<std::streamsize>(pending.size()));
        g_file.flush();
        if (!g_file) {
            StopRecording("기록 파일에 쓰지 못함(디스크 공간이나 권한을 확인)");
            return;
        }
        const auto total = g_bytes.fetch_add(pending.size()) + pending.size();
        if (g_recording && total >= static_cast<std::uint64_t>(g_settings.maxSessionMB) * 1024 * 1024) {
            const auto row = std::format("S\t{}\tstop\tcap\n", Micros());
            g_file.write(row.data(), static_cast<std::streamsize>(row.size()));
            g_file.flush();
            StopRecording(std::format("기록 파일이 한도 {}MB에 닿음", g_settings.maxSessionMB));
        }
    }

    bool OpenDataFile(const std::string& runtime)
    {
        auto directory = SKSE::log::log_directory();
        if (!directory) {
            SKSE::log::error("SKSE 로그 폴더를 찾지 못해 기록 파일을 만들지 못함");
            return false;
        }
        const auto      folder = *directory / kDataFolder;
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        const std::time_t now = std::time(nullptr);
        std::tm           local{};
        localtime_s(&local, &now);
        char fileStamp[32]{};
        char textStamp[32]{};
        std::strftime(fileStamp, sizeof(fileStamp), "%Y%m%d-%H%M%S", &local);
        std::strftime(textStamp, sizeof(textStamp), "%Y-%m-%dT%H:%M:%S", &local);
        g_dataPath = folder / std::format("measure-{}.tsv", fileStamp);
        // 같은 이름이 이미 있으면(같은 초에 다시 켜짐, 시계가 뒤로 맞춰짐) 다른 실행의 기록에 이어 쓰지 않고 새 이름을 쓴다.
        for (int extra = 2; extra < 100 && std::filesystem::exists(g_dataPath, error); ++extra) {
            g_dataPath = folder / std::format("measure-{}-{}.tsv", fileStamp, extra);
        }
        g_file.open(g_dataPath, std::ios::binary | std::ios::out | std::ios::app);
        if (!g_file) {
            SKSE::log::error("기록 파일을 열지 못함: {}", PathText(g_dataPath));
            return false;
        }
        const auto header = std::format(
            "# PoEProtoMeasure {} / runtime {} / started {} (local)\n"
            "# S\tt_us\tkind\tdetail\n"
            "# C\tt_us\tplayerLevel\tplayerMaxHealth\tdifficulty\tmultByPC\tmultToPC\trightHand\tleftHand\tinterior\tcell\tlocation\tcellName\tlocationName\n"
            "# A\tt_us\tref\tbase\trootBase\tlevel\tmaxHealth\tbaseHealth\thealth\tflags\trace\tplugin\tname\traceName\n"
            "# P\tt_us\tref\thealth\tmaxHealth\n"
            "# D\tt_us\tthread\ttarget\tattacker\targ\thealthBefore\thealthAfter\tmaxHealth\n"
            "# H\tt_us\tthread\ttarget\tcause\tsource\tprojectile\tflags\n"
            "# K\tt_us\tthread\tdying\tkiller\tdead\n"
            "# E\tt_us\tactor\ttarget\tstate\n"
            "# F\tt_us\tform\ttype\tweaponType\tattackDamage\tname\n"
            "S\t{}\tstart\t{}\n",
            kPluginVersion, runtime, textStamp, Micros(), textStamp);
        g_file.write(header.data(), static_cast<std::streamsize>(header.size()));
        g_file.flush();
        if (!g_file) {
            SKSE::log::error("기록 파일에 쓰지 못함: {}", PathText(g_dataPath));
            g_file.close();
            return false;
        }
        g_bytes = header.size();
        g_recording = true;
        SKSE::log::info("기록 파일: {}", PathText(g_dataPath));
        return true;
    }

    // ---------------------------------------------------------------- 액터와 폼 읽기

    // 기록의 한 칸에 넣을 글자. 탭과 줄바꿈은 칸을 깨므로 빈칸으로 바꾼다.
    std::string Clean(const char* text)
    {
        std::string out = text ? text : "";
        for (auto& c : out) {
            if (c == '\t' || c == '\n' || c == '\r') {
                c = ' ';
            }
        }
        return out.empty() ? std::string("-") : out;
    }

    float Health(RE::Actor* actor) { return actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth); }

    // player가 있으면 적대 여부까지 판정한다(훑을 때). 없으면 판정하지 않는다: 끼어든 자리와 이벤트 수신부에서는
    // 엔진의 함수를 되도록 적게 부른다. 그렇게 적힌 액터는 다음 훑기가 적대 여부를 붙여 다시 적는다.
    Snapshot Take(RE::Actor* actor, RE::Actor* player)
    {
        Snapshot shot;
        shot.id = actor->GetFormID();
        const auto* base = actor->GetActorBase();
        shot.base = base ? base->GetFormID() : 0;
        shot.level = actor->GetLevel();
        shot.maxHealth = actor->GetActorValueMax(RE::ActorValue::kHealth);
        shot.health = Health(actor);
        if (player) {
            shot.bits |= kHostileKnown;
            if (actor != player && actor->IsHostileToActor(player)) {
                shot.bits |= kHostile;
            }
        }
        shot.bits |= actor->IsPlayerTeammate() ? kTeammate : 0;
        shot.bits |= actor->IsCommandedActor() ? kCommanded : 0;
        shot.bits |= actor->IsSummoned() ? kSummoned : 0;
        shot.bits |= actor->IsEssential() ? kEssential : 0;
        shot.bits |= actor->IsProtected() ? kProtected : 0;
        shot.bits |= base && base->IsUnique() ? kUnique : 0;
        shot.bits |= actor->IsDead() ? kDead : 0;
        shot.bits |= actor->IsInCombat() ? kInCombat : 0;
        return shot;
    }

    void WriteActor(RE::Actor* actor, const Snapshot& shot, std::int64_t t)
    {
        const auto* base = actor->GetActorBase();
        // 레벨 목록에서 나온 액터의 기본 폼은 실행 중에 만들어진 것(FF로 시작)이라, 그 틀이 된 폼을 함께 적는다.
        const auto* root = base ? base->GetRootFaceNPC() : nullptr;
        const auto* file = root ? root->GetFile(0) : nullptr;
        const auto* race = actor->GetRace();
        std::string flags;
        constexpr std::array<std::pair<std::uint32_t, char>, 9> kLetters{ { { kHostile, 'H' }, { kTeammate, 'T' }, { kCommanded, 'C' }, { kSummoned, 'S' },
            { kEssential, 'E' }, { kProtected, 'P' }, { kUnique, 'U' }, { kDead, 'D' }, { kInCombat, 'X' } } };
        for (const auto& [bit, letter] : kLetters) {
            if (shot.bits & bit) {
                flags.push_back(letter);
            }
        }
        // 적대 여부를 아직 판정하지 않은 줄은 '?'를 붙여 구분한다.
        if (!(shot.bits & kHostileKnown)) {
            flags.push_back('?');
        }
        if (flags.empty()) {
            flags = "-";
        }
        const bool isPlayer = shot.id == kPlayerRef;
        Append(std::format("A\t{}\t{:08X}\t{:08X}\t{:08X}\t{}\t{:.1f}\t{:.1f}\t{:.1f}\t{}\t{:08X}\t{}\t{}\t{}\n", t, shot.id, shot.base,
            root ? root->GetFormID() : 0, shot.level, shot.maxHealth, actor->AsActorValueOwner()->GetBaseActorValue(RE::ActorValue::kHealth), shot.health, flags,
            race ? race->GetFormID() : 0, file ? Clean(std::string(file->GetFilename()).c_str()) : std::string("-"),
            isPlayer ? std::string("(player)") : Clean(base ? base->GetName() : nullptr), Clean(race ? race->GetName() : nullptr)));
    }

    // 본 것을 적는다. 처음 보거나 열쇠가 되는 값이 바뀌었으면 A 줄, 훑는 중에 체력만 달라졌으면 P 줄.
    void Record(RE::Actor* actor, const Snapshot& shot, std::int64_t t, bool fromScan)
    {
        bool       describe = false;
        bool       healthOnly = false;
        const long maxRounded = std::lround(shot.maxHealth);
        const auto keyBits = shot.bits & kKeyBits;
        {
            const std::scoped_lock lock(g_knownMutex);
            const auto [it, inserted] = g_known.try_emplace(shot.id);
            auto&      known = it->second;
            if (inserted || known.base != shot.base || known.level != shot.level || known.maxHealth != maxRounded || known.keyBits != keyBits) {
                describe = true;
                known = Known{ shot.base, shot.level, maxRounded, keyBits, shot.health };
                if ((shot.bits & kHostile) && !(shot.bits & (kTeammate | kCommanded | kSummoned | kDead))) {
                    const auto key = (static_cast<std::uint64_t>(shot.id) << 32) ^ (static_cast<std::uint64_t>(shot.base) << 8) ^ shot.level;
                    if (g_countedHostiles.insert(key).second) {
                        ++g_hostiles;
                        if (shot.level <= kLowLevel) {
                            ++g_lowHostiles;
                        }
                    }
                }
            } else if (fromScan && std::abs(shot.health - known.lastHealth) > 0.5f) {
                healthOnly = true;
                known.lastHealth = shot.health;
            }
        }
        if (describe) {
            WriteActor(actor, shot, t);
        } else if (healthOnly) {
            Append(std::format("P\t{}\t{:08X}\t{:.2f}\t{:.1f}\n", t, shot.id, shot.health, shot.maxHealth));
        }
    }

    // 이벤트나 피해 기록에 나온 액터가 아직 설명되지 않았으면 설명한다(적대 여부는 판정하지 않는다).
    void EnsureDescribed(RE::Actor* actor, std::int64_t t)
    {
        if (!actor) {
            return;
        }
        const auto  id = actor->GetFormID();
        const auto* base = actor->GetActorBase();
        const auto  baseId = base ? base->GetFormID() : 0;
        bool        knownDead = false;
        {
            const std::scoped_lock lock(g_knownMutex);
            if (const auto it = g_known.find(id); it != g_known.end() && it->second.base == baseId) {
                if (!(it->second.keyBits & kDead)) {
                    return;
                }
                knownDead = true;
            }
        }
        // 죽은 것으로 적어 둔 폼 ID가 산 액터로 나타났으면 다른 액터다(다시 소환된 것, 임시 참조의 폼 ID가 다시 쓰인 것).
        // 다음 훑기를 기다리지 않고 지금 다시 설명해야, 그 액터의 피해와 죽음이 앞 액터에 붙지 않는다. 시체가 맞은 것이면 그대로 둔다.
        if (knownDead && actor->IsDead()) {
            return;
        }
        Record(actor, Take(actor, nullptr), t, false);
    }

    void EnsureFormDescribed(RE::FormID id, std::int64_t t)
    {
        if (id == 0) {
            return;
        }
        {
            const std::scoped_lock lock(g_knownMutex);
            if (!g_knownForms.insert(id).second) {
                return;
            }
        }
        auto*       form = RE::TESForm::LookupByID(id);
        std::string type = "-";
        std::string name = "-";
        int         weaponType = -1;
        int         attackDamage = -1;
        if (form) {
            type = std::string(RE::FormTypeToString(form->GetFormType()));
            name = Clean(form->GetName());
            if (const auto* weapon = form->As<RE::TESObjectWEAP>()) {
                weaponType = static_cast<int>(weapon->GetWeaponType());
                attackDamage = weapon->GetAttackDamage();
            }
        }
        Append(std::format("F\t{}\t{:08X}\t{}\t{}\t{}\t{}\n", t, id, type.empty() ? std::string("-") : type, weaponType, attackDamage, name));
    }

    // ---------------------------------------------------------------- 체력 피해에 끼어들기

    void OnHealthDamage(RE::Actor* target, RE::Actor* attacker, float damage, float before)
    {
        const auto  t = Micros();
        const float after = Health(target);
        const float maxHealth = target->GetActorValueMax(RE::ActorValue::kHealth);
        const auto  targetId = target->GetFormID();
        EnsureDescribed(target, t);
        EnsureDescribed(attacker, t);
        Append(std::format("D\t{}\t{}\t{:08X}\t{:08X}\t{:.3f}\t{:.2f}\t{:.2f}\t{:.1f}\n", t, Tid(), targetId, attacker ? attacker->GetFormID() : 0, damage, before, after,
            maxHealth));
        if (targetId == kPlayerRef && attacker && damage != 0.0f) {
            ++g_received;
        }
    }

    // 액터의 체력 피해 가상 함수. 원래 함수는 그대로 부르고 값은 바꾸지 않는다. NPC(Character)와 플레이어(PlayerCharacter)의
    // 가상 함수 표가 따로라 두 곳에 끼어들고, 되돌아갈 원래 함수도 따로 둔다.
    template <std::size_t Which>
    struct HealthDamageHook
    {
        static void Thunk(RE::Actor* self, RE::Actor* attacker, float damage)
        {
            const bool record = self && g_recording.load(std::memory_order_relaxed);
            const float before = record ? Health(self) : 0.0f;
            original(self, attacker, damage);
            if (record) {
                try {
                    OnHealthDamage(self, attacker, damage, before);
                } catch (...) {
                    // 기록하다 난 예외(메모리 부족 등)를 엔진으로 내보내지 않는다.
                }
            }
        }

        static void Install(const REL::VariantID& table)
        {
            REL::Relocation<std::uintptr_t> vtable{ table };
            original = vtable.write_vfunc(kHealthDamageSlot, Thunk);
        }

        static inline REL::Relocation<decltype(Thunk)> original;
    };

    // ---------------------------------------------------------------- 엔진의 이벤트

    class HitSink final : public RE::BSTEventSink<RE::TESHitEvent>
    {
    public:
        static HitSink& Get()
        {
            static HitSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* event, RE::BSTEventSource<RE::TESHitEvent>*) override
        {
            if (!event || !g_recording) {
                return RE::BSEventNotifyControl::kContinue;
            }
            try {
                auto* target = event->target ? event->target->As<RE::Actor>() : nullptr;
                if (!target) {
                    return RE::BSEventNotifyControl::kContinue;  // 물건에 맞은 것은 이 측정의 범위 밖이다
                }
                auto*      causeRef = event->cause.get();
                auto*      cause = causeRef ? causeRef->As<RE::Actor>() : nullptr;
                const auto t = Micros();
                EnsureDescribed(target, t);
                EnsureDescribed(cause, t);
                EnsureFormDescribed(event->source, t);
                Append(std::format("H\t{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{}\n", t, Tid(), target->GetFormID(), causeRef ? causeRef->GetFormID() : 0, event->source,
                    event->projectile, static_cast<unsigned>(event->flags.underlying())));
                if (cause && cause->GetFormID() == kPlayerRef && target->GetFormID() != kPlayerRef) {
                    ++g_playerHits;
                }
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class DeathSink final : public RE::BSTEventSink<RE::TESDeathEvent>
    {
    public:
        static DeathSink& Get()
        {
            static DeathSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent* event, RE::BSTEventSource<RE::TESDeathEvent>*) override
        {
            if (!event || !g_recording) {
                return RE::BSEventNotifyControl::kContinue;
            }
            try {
                auto* dying = event->actorDying ? event->actorDying->As<RE::Actor>() : nullptr;
                if (!dying) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto*      killerRef = event->actorKiller.get();
                auto*      killer = killerRef ? killerRef->As<RE::Actor>() : nullptr;
                const auto t = Micros();
                const auto dyingId = dying->GetFormID();
                EnsureDescribed(dying, t);
                EnsureDescribed(killer, t);
                Append(std::format("K\t{}\t{}\t{:08X}\t{:08X}\t{}\n", t, Tid(), dyingId, killerRef ? killerRef->GetFormID() : 0, event->dead ? 1 : 0));
                // 죽음이 끝났으면 본 것에 죽음을 적어 둔다. 훑기가 죽은 모습을 보기 전에 시체가 사라지고 같은 폼 ID가 다시 쓰여도
                // EnsureDescribed가 새 액터로 가를 수 있다. 엔진이 아직 죽은 것으로 치지 않는 순간이면 적지 않는다(산 채로 다시 설명되지 않게).
                if (event->dead && dying->IsDead()) {
                    const std::scoped_lock lock(g_knownMutex);
                    if (const auto it = g_known.find(dyingId); it != g_known.end()) {
                        it->second.keyBits |= kDead;
                    }
                }
                // 상태 키가 보여 주는 수: 동료가 아닌 액터가 죽었고, 그 액터가 적대로 적혀 있었거나 죽인 쪽이 플레이어나 동료일 때.
                if (dyingId != kPlayerRef && !dying->IsPlayerTeammate()) {
                    const bool byParty = killer && (killer->GetFormID() == kPlayerRef || killer->IsPlayerTeammate());
                    const std::scoped_lock lock(g_knownMutex);
                    const auto             it = g_known.find(dyingId);
                    const bool             hostile = it != g_known.end() && (it->second.keyBits & kHostile);
                    if ((hostile || byParty) && g_countedDeaths.insert(dyingId).second) {
                        ++g_deaths;
                    }
                }
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    class CombatSink final : public RE::BSTEventSink<RE::TESCombatEvent>
    {
    public:
        static CombatSink& Get()
        {
            static CombatSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* event, RE::BSTEventSource<RE::TESCombatEvent>*) override
        {
            if (!event || !g_recording) {
                return RE::BSEventNotifyControl::kContinue;
            }
            try {
                auto* actor = event->actor ? event->actor->As<RE::Actor>() : nullptr;
                if (!actor) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto*      targetRef = event->targetActor.get();
                const auto t = Micros();
                EnsureDescribed(actor, t);
                EnsureDescribed(targetRef ? targetRef->As<RE::Actor>() : nullptr, t);
                Append(std::format("E\t{}\t{:08X}\t{:08X}\t{}\n", t, actor->GetFormID(), targetRef ? targetRef->GetFormID() : 0,
                    static_cast<unsigned>(event->newState.underlying())));
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---------------------------------------------------------------- 주기적으로 훑기

    // 난이도 배율은 게임 설정에서 그 순간에 읽는다(이 프로필은 Combat Difficulty Customizer가 실행 중에 바꾼다).
    // 설정의 이름은 엔진의 것이고 플러그인 레코드로는 fDiffMultHPToPCL만 대조했다. 없는 이름이면 nan으로 적힌다.
    float DifficultyMultiplier(const char* prefix, std::int32_t difficulty)
    {
        constexpr std::array<const char*, 6> kSuffix{ "VE", "E", "N", "H", "VH", "L" };
        if (difficulty < 0 || difficulty >= static_cast<std::int32_t>(kSuffix.size())) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        auto*      settings = RE::GameSettingCollection::GetSingleton();
        const auto name = std::format("{}{}", prefix, kSuffix[static_cast<std::size_t>(difficulty)]);
        auto*      setting = settings ? settings->GetSetting(name.c_str()) : nullptr;
        return setting ? setting->GetFloat() : std::numeric_limits<float>::quiet_NaN();
    }

    void WriteContext(RE::PlayerCharacter* player, std::int64_t t)
    {
        const auto  difficulty = player->GetGameStatsData().difficulty;
        const auto* right = player->GetEquippedObject(false);
        const auto* left = player->GetEquippedObject(true);
        const auto* cell = player->GetParentCell();
        const auto* location = player->GetPlayerRuntimeData().currentLocation;
        const auto  rightId = right ? right->GetFormID() : 0;
        const auto  leftId = left ? left->GetFormID() : 0;
        auto        body = std::format("{}\t{}\t{}\t{:.3f}\t{:.3f}\t{:08X}\t{:08X}\t{}\t{:08X}\t{:08X}\t{}\t{}", player->GetLevel(),
            std::lround(player->GetActorValueMax(RE::ActorValue::kHealth)), difficulty, DifficultyMultiplier("fDiffMultHPByPC", difficulty),
            DifficultyMultiplier("fDiffMultHPToPC", difficulty), rightId, leftId, cell && cell->IsInteriorCell() ? 1 : 0, cell ? cell->GetFormID() : 0,
            location ? location->GetFormID() : 0, Clean(cell ? cell->GetName() : nullptr), Clean(location ? location->GetName() : nullptr));
        if (body == g_lastContext) {
            return;
        }
        EnsureFormDescribed(rightId, t);
        EnsureFormDescribed(leftId, t);
        Append(std::format("C\t{}\t{}\n", t, body));
        g_lastContext = std::move(body);
    }

    void DeliverNotices()
    {
        if (g_stopNoticePending.exchange(false)) {
            std::string reason;
            {
                const std::scoped_lock lock(g_stopMutex);
                reason = g_stopReason;
            }
            Notify(std::format("[PoE Proto 7] 기록을 멈췄습니다: {}", reason));
            g_loadNoticePending = false;
            return;
        }
        if (g_loadNoticePending.exchange(false)) {
            if (g_recording) {
                if (g_settings.notify) {
                    Notify(g_settings.statusKey != 0 ? std::format("[PoE Proto 7] 측정 중입니다. 지금까지의 수: {}", StatusKeyName())
                                                     : std::string("[PoE Proto 7] 측정 중입니다."));
                }
            } else {
                // 기록하지 못하는 것은 설정(Notify)과 무관하게 알린다. 모르고 한 시간을 플레이하면 그 시간을 잃는다.
                std::string reason;
                {
                    const std::scoped_lock lock(g_stopMutex);
                    reason = g_stopReason;
                }
                Notify(std::format("[PoE Proto 7] 기록하지 않는 중입니다: {}", reason));
            }
        }
    }

    // SKSE에 맡긴 작업으로 돈다. 플레이어와 가까운 액터 목록을 훑는다.
    void Scan()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        if (!player || !player->Is3DLoaded() || !ui || ui->GameIsPaused()) {
            return;
        }
        DeliverNotices();
        if (!g_recording) {
            return;
        }
        const auto t = Micros();
        WriteContext(player, t);
        Record(player, Take(player, player), t, true);
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!lists) {
            return;
        }
        for (auto& handle : lists->highActorHandles) {
            const auto actor = handle.get();
            if (actor) {
                Record(actor.get(), Take(actor.get(), player), t, true);
            }
        }
    }

    void ShowStatus()
    {
        if (!g_recording) {
            std::string reason;
            {
                const std::scoped_lock lock(g_stopMutex);
                reason = g_stopReason;
            }
            Notify(std::format("[PoE Proto 7] 기록하지 않는 중입니다: {}", reason));
            return;
        }
        Notify(std::format("[PoE Proto 7] 적대 액터 {}/{} (레벨 {} 이하 {}/{}), 죽은 적 {}/{}", g_hostiles.load(), kGoalHostiles, kLowLevel, g_lowHostiles.load(),
            kGoalLowHostiles, g_deaths.load(), kGoalDeaths));
        Notify(std::format("[PoE Proto 7] 내 타격 {}/{}, 받은 피해 {}/{}, 기록 {:.1f}MB{}", g_playerHits.load(), kGoalHits, g_received.load(), kGoalReceived,
            static_cast<double>(g_bytes.load()) / (1024.0 * 1024.0), g_hookInstalled ? "" : " (피해 기록 꺼짐)"));
    }

    // 일정한 간격으로 기록을 내려 쓰고 훑는 작업을 맡기는 스레드. 작업이 스스로 다음 작업을 맡기는 방식은 쓰지 않는다
    // (프로토타입 5의 2차 시험: 그렇게 하면 한 프레임 안에서 다 돌아 버린다).
    void TickerMain()
    {
        const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(g_settings.scanSeconds));
        auto       lastScan = Clock::now();
        for (;;) {
            std::this_thread::sleep_for(kTick);
            try {
                Flush();
            } catch (...) {
                StopRecording("기록을 내려 쓰다 오류가 남");
            }
            const auto now = Clock::now();
            if (now - lastScan < interval || g_scanQueued.exchange(true)) {
                continue;
            }
            lastScan = now;
            const auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                g_scanQueued = false;
                continue;
            }
            tasks->AddTask([] {
                g_scanQueued = false;
                try {
                    Scan();
                } catch (...) {
                }
            });
        }
    }

    class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static InputSink& Get()
        {
            static InputSink instance;
            return instance;
        }

        RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events, RE::BSTEventSource<RE::InputEvent*>*) override
        {
            if (!events || g_settings.statusKey == 0) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto* event = *events; event; event = event->next) {
                const auto* button = event->AsButtonEvent();
                if (button && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == g_settings.statusKey && button->IsDown()) {
                    if (const auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([] {
                            try {
                                ShowStatus();
                            } catch (...) {
                            }
                        });
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // 세이브가 불려 오면 그 전에 본 액터는 그 세이브의 것이 아니다. 본 것을 비워 다시 설명하게 하고, 기록에 구간 표시를 남긴다.
    // 불러오기가 시작될 때(kPreLoadGame) 부른다: 불러오는 동안 생기는 이벤트는 새 세이브의 것이므로 표시가 그 앞에 와야 한다.
    void MarkLoad(const char* kind)
    {
        {
            const std::scoped_lock lock(g_knownMutex);
            g_known.clear();
            g_countedDeaths.clear();
        }
        Append(std::format("S\t{}\t{}\t-\n", Micros(), kind));
    }

    void LogDifficultySettings()
    {
        std::string line;
        for (std::int32_t difficulty = 0; difficulty < 6; ++difficulty) {
            line += std::format("{}{}: 주는 {:.2f} / 받는 {:.2f}", line.empty() ? "" : ", ", difficulty, DifficultyMultiplier("fDiffMultHPByPC", difficulty),
                DifficultyMultiplier("fDiffMultHPToPC", difficulty));
        }
        SKSE::log::info("데이터를 읽은 직후의 난이도 배율(게임 설정): {}", line);
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
                SKSE::log::error("입력 장치 관리자를 찾지 못함. 상태 키를 쓸 수 없습니다.");
            }
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            if (!g_recording) {
                // 기록할 곳이 없으면 게임에 끼어들지 않는다. 얻는 것 없이 위험만 남기 때문이다. 알림을 띄울 스레드만 돌린다.
                SKSE::log::error("기록 파일이 없어 끼어들기와 이벤트 수신부를 설치하지 않음");
                std::thread(TickerMain).detach();
                break;
            }
            if (g_settings.hookDamage) {
                HealthDamageHook<0>::Install(RE::VTABLE_Character[0]);
                HealthDamageHook<1>::Install(RE::VTABLE_PlayerCharacter[0]);
                g_hookInstalled = true;
                SKSE::log::info("액터의 체력 피해 함수(가상 함수 0x{:X})에 끼어듦: Character, PlayerCharacter", kHealthDamageSlot);
            } else {
                SKSE::log::info("체력 피해 함수에 끼어들지 않음(설정 HookDamage = false). 피해 기록(D 줄)은 남지 않습니다.");
            }
            if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
                events->AddEventSink<RE::TESHitEvent>(&HitSink::Get());
                events->AddEventSink<RE::TESDeathEvent>(&DeathSink::Get());
                events->AddEventSink<RE::TESCombatEvent>(&CombatSink::Get());
            } else {
                SKSE::log::error("이벤트 원천을 찾지 못함. 적중·사망·전투 이벤트는 기록되지 않습니다.");
            }
            LogDifficultySettings();
            Append(std::format("S\t{}\tconfig\thook={} scan={:.1f}\n", Micros(), g_hookInstalled ? 1 : 0, g_settings.scanSeconds));
            std::thread(TickerMain).detach();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            MarkLoad("load");
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            // 알림은 불러오기가 끝난 뒤에 띄운다(훑는 작업이 게임이 도는 것을 확인하고 띄운다).
            g_loadNoticePending = true;
            break;
        case SKSE::MessagingInterface::kNewGame:
            MarkLoad("newgame");
            g_loadNoticePending = true;
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse, { .log = false });
    InitializeLogging();
    g_settings = LoadSettings();
    SKSE::log::info("설정: StatusKey=0x{:02X} HookDamage={} ScanSeconds={:.1f} Notify={} MaxSessionMB={}", g_settings.statusKey, g_settings.hookDamage,
        g_settings.scanSeconds, g_settings.notify, g_settings.maxSessionMB);
    OpenDataFile(skse->RuntimeVersion().string("."));
    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("SKSE 메시지 리스너를 등록하지 못함. 측정을 시작할 수 없습니다.");
        g_recording = false;
        return true;
    }
    SKSE::log::info("PoEProtoMeasure 준비 완료 (기록 {})", g_recording ? "켜짐" : "꺼짐");
    return true;
}
