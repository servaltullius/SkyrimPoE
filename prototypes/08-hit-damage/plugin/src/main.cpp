// PoE Proto 8: 엔진 타격의 피해를 바꾸는 자리를 시험한다.
// 근접 타격 함수(AE ID 38627)는 타격 자료(HitData)를 채운 뒤 +0x4A8에서 HitData 적용 함수(AE ID 38586)를 부르고,
// 38586은 HitData.totalDamage(난이도 배율 전)를 Actor::DoDamage로 체력에 적용한다(설계-현황 4.6, AE 1.6.1170 디컴파일).
// 이 플러그인은 그 호출을 가로채 원래 함수를 부르기 전에 totalDamage를 바꿔 보고, 앞뒤의 체력을 적는다.
// 38586이 다른 스레드에서 불려 작업 큐로 미뤄지면 큐(AE ID 36991)가 복사본으로 38586을 다시 부른다. 그 자리는 보기만 한다
// (바꾸면 같은 타격을 두 번 고친다). 무엇을 묻고 어떻게 판정하는지는 README에 있다.
// 빌드하는 CommonLibSSE-NG는 저장소의 서브모듈(v16.4.0, 결정 60)이다.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

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
#include <limits>
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

    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoHitDamage.ini";
    constexpr auto kLogName = "PoEProtoHitDamage";
    constexpr auto kNoticePrefix = "[PoE Proto 8]";

    // AE 1.6.1170에서 찾은 자리. 설치 전에 그 자리가 정말 38586을 부르는 call인지 바이트로 확인한다.
    constexpr std::uint64_t  kApplyHitId = 38586;  // HitData를 대상에게 적용(적중 이벤트, DoDamage)
    constexpr std::uint64_t  kMeleeHitId = 38627;  // 근접 타격 함수(SE PDB 이름 Character::HitData)
    constexpr std::ptrdiff_t kMeleeCallOffset = 0x4A8;
    constexpr std::uint64_t  kQueueId = 36991;  // BSTaskPool_HandleTask
    constexpr std::ptrdiff_t kQueueCallOffset = 0xA3D;
    constexpr REL::Version   kVerifiedRuntime{ 1, 6, 1170, 0 };  // 위의 자리를 디컴파일과 바이트 검색으로 확인한 판

    constexpr auto  kTick = std::chrono::milliseconds(100);
    constexpr auto  kLateDelay = std::chrono::milliseconds(300);  // 체력이 원래 함수 안에서 줄지 않았을 때 다시 읽기까지
    constexpr float kMaxValue = 10000.0f;

    enum class Mode : int
    {
        kObserve = 0,  // 바꾸지 않고 적기만
        kSet,          // totalDamage를 Value로
        kZero,         // totalDamage를 0으로
        kReplace,      // totalDamage를 0으로 하고, 원래 함수가 끝난 뒤 DoDamage(Value)를 따로 준다
        kCount
    };
    constexpr std::array<std::string_view, 4> kModeNames{ "observe", "set", "zero", "replace" };
    constexpr std::array<std::string_view, 4> kModeLabels{ "관찰", "바꾸기", "지우기", "대신 주기" };

    enum class Site
    {
        kMelee,  // 근접 타격 함수 안의 호출(바꾸는 자리)
        kQueue,  // 작업 큐가 복사본으로 다시 부르는 호출(보기만)
    };

    struct Settings
    {
        std::uint32_t modeKey = 0x42;  // 키보드 DirectInput 스캔 코드. 0x42 = F8, 0 = 쓰지 않음
        Mode          startMode = Mode::kObserve;
        float         value = 10.0f;  // set과 replace에서 쓸 값(난이도 배율 전)
        bool          scopeAll = false;  // false: 공격자가 플레이어인 타격만 바꾼다
        bool          hookQueue = true;
        bool          notify = true;
    };

    // 원래 함수를 부르기 전에 읽어 둔 것.
    struct Pending
    {
        std::int64_t        t = 0;
        Mode                mode = Mode::kObserve;
        bool                modified = false;
        RE::NiPointer<RE::Actor> aggressor;  // replace에서 DoDamage의 공격자로 쓴다. 원래 함수가 도는 동안 붙잡아 둔다
        RE::FormID          aggressorId = 0;
        RE::FormID          targetId = 0;
        RE::FormID          weaponId = 0;
        RE::FormID          sourceRefId = 0;
        unsigned            sourceRefType = 0;
        std::uint32_t       flags = 0;
        float               totalBefore = 0.0f;
        float               totalAfter = 0.0f;
        float               physical = 0.0f;
        float               percentBlocked = 0.0f;
        float               resistedPhysical = 0.0f;
        float               resistedTyped = 0.0f;
        float               sneakBonus = 0.0f;
        float               bonusHealthMult = 0.0f;
        float               reflected = 0.0f;
        float               stagger = 0.0f;
        float               healthBefore = 0.0f;
        float               maxHealth = 0.0f;
        float               multiplier = 1.0f;
    };

    struct LateRead
    {
        RE::ActorHandle    target;
        RE::FormID         targetId = 0;
        std::int64_t       hitTime = 0;
        Clock::time_point  due;
    };

    Settings                g_settings;
    const Clock::time_point g_epoch = Clock::now();
    std::atomic<int>        g_mode{ 0 };
    std::atomic<bool>       g_meleeInstalled{ false };
    std::atomic<bool>       g_queueInstalled{ false };
    std::atomic<bool>       g_noticePending{ false };
    std::atomic<bool>       g_noticeQueued{ false };
    std::atomic<bool>       g_tickerStarted{ false };
    std::mutex              g_installMutex;
    std::string             g_installProblem;  // 비어 있으면 근접 자리는 설치됨
    std::mutex              g_lateMutex;
    std::vector<LateRead>   g_late;

    unsigned long Tid() { return ::GetCurrentThreadId(); }

    std::int64_t Micros() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_epoch).count(); }

    Mode CurrentMode() { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

    std::string_view ModeName(Mode mode) { return kModeNames[static_cast<std::size_t>(mode)]; }

    std::string_view ModeLabel(Mode mode) { return kModeLabels[static_cast<std::size_t>(mode)]; }

    // ---------------------------------------------------------------- 로그와 설정

    // 로그는 직접 연다(SKSE::Init에 맡기면 같은 이름의 파일을 먼저 비운다). 지난 로그는 하나 남긴다.
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
            auto logger = std::make_shared<spdlog::logger>("poe_proto_hit_damage", std::move(sink));
            logger->set_pattern("%Y-%m-%d %H:%M:%S.%e\t%l\t%v");
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

    void ParseBool(std::string_view key, std::string_view value, bool& out)
    {
        if (value == "true" || value == "1") {
            out = true;
        } else if (value == "false" || value == "0") {
            out = false;
        } else {
            SKSE::log::warn("설정: {} '{}'는 true 또는 false가 아니어서 기본값 유지", key, value);
        }
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
            if (key == "ModeKey") {
                std::uint32_t code = 0;
                if (ParseUnsigned(value, code) && code <= 255) {
                    settings.modeKey = code;
                } else {
                    SKSE::log::warn("설정: ModeKey '{}'는 0~255의 키 코드가 아니어서 기본값 유지", value);
                }
            } else if (key == "StartMode") {
                bool found = false;
                for (std::size_t i = 0; i < kModeNames.size(); ++i) {
                    if (value == kModeNames[i]) {
                        settings.startMode = static_cast<Mode>(i);
                        found = true;
                    }
                }
                if (!found) {
                    SKSE::log::warn("설정: StartMode '{}'는 observe, set, zero, replace가 아니어서 기본값 유지", value);
                }
            } else if (key == "Value") {
                float      parsed = 0.0f;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed >= 0.0f && parsed <= kMaxValue) {
                    settings.value = parsed;
                } else {
                    SKSE::log::warn("설정: Value '{}'는 0 이상 {} 이하의 수가 아니어서 기본값 유지", value, kMaxValue);
                }
            } else if (key == "Scope") {
                if (value == "player") {
                    settings.scopeAll = false;
                } else if (value == "all") {
                    settings.scopeAll = true;
                } else {
                    SKSE::log::warn("설정: Scope '{}'는 player 또는 all이 아니어서 기본값 유지", value);
                }
            } else if (key == "HookQueue") {
                ParseBool(key, value, settings.hookQueue);
            } else if (key == "Notify") {
                ParseBool(key, value, settings.notify);
            } else {
                SKSE::log::warn("설정: 알 수 없는 키 '{}'", key);
            }
        }
        return settings;
    }

    std::string ModeKeyName()
    {
        if (g_settings.modeKey == 0) {
            return "모드 키 없음";
        }
        return g_settings.modeKey == 0x42 ? std::string("F8") : std::format("스캔 코드 0x{:02X}", g_settings.modeKey);
    }

    // 화면 알림. 메인 스레드(SKSE 작업)에서 부른다. 실패를 알리는 것은 Notify 설정과 상관없이 띄운다.
    void ShowNotice(const std::string& message, bool always)
    {
        SKSE::log::info("{}", message);
        if (always || g_settings.notify) {
            RE::SendHUDMessage::ShowHUDMessage(message.c_str());
        }
    }

    // ---------------------------------------------------------------- 엔진에서 읽기

    float Health(RE::Actor* actor) { return actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth); }

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

    // 이 타격에 곱해질 난이도 배율(게임 설정을 그 순간에 읽음). 엔진의 GetDifficultyMultiplier(AE ID 26503, 디컴파일)는 체력 피해에서
    // 대상이 플레이어면 받는 배율 표, 아니면 공격자가 누구든 주는 배율 표를 쓴다. 동료가 적을 칠 때도 주는 배율이 곱해진다.
    float MultiplierFor(const RE::Actor* target)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        const auto difficulty = player->GetGameStatsData().difficulty;
        return DifficultyMultiplier(target == player ? "fDiffMultHPToPC" : "fDiffMultHPByPC", difficulty);
    }

    // ---------------------------------------------------------------- 가로채기

    void QueueLateRead(RE::Actor* target, RE::FormID targetId, std::int64_t hitTime)
    {
        const std::scoped_lock lock(g_lateMutex);
        g_late.push_back({ target->GetHandle(), targetId, hitTime, Clock::now() + kLateDelay });
    }

    // 원래 함수를 부르기 전: 읽고, 바꿀 차례면 바꾼다. 여기서 예외가 나면 바꾸지 않은 채 원래 함수를 부른다.
    bool Begin(Site site, RE::Actor* target, RE::HitData* hit, Pending& p)
    {
        if (!target || !hit) {
            return false;
        }
        p.t = Micros();
        p.mode = CurrentMode();
        p.aggressor = hit->aggressor.get();
        auto* aggressor = p.aggressor.get();
        p.aggressorId = aggressor ? aggressor->GetFormID() : 0;
        p.targetId = target->GetFormID();
        p.weaponId = hit->weapon ? hit->weapon->GetFormID() : 0;
        if (const auto source = hit->sourceRef.get()) {
            p.sourceRefId = source->GetFormID();
            p.sourceRefType = static_cast<unsigned>(source->GetFormType());
        }
        p.flags = hit->flags.underlying();
        p.totalBefore = hit->totalDamage;
        p.physical = hit->physicalDamage;
        p.percentBlocked = hit->percentBlocked;
        p.resistedPhysical = hit->resistedPhysicalDamage;
        p.resistedTyped = hit->resistedTypedDamage;
        p.sneakBonus = hit->sneakAttackBonus;
        p.bonusHealthMult = hit->bonusHealthDamageMult;
        p.reflected = hit->reflectedDamage;
        p.stagger = hit->stagger;
        p.healthBefore = Health(target);
        p.maxHealth = target->GetActorValueMax(RE::ActorValue::kHealth);
        p.multiplier = MultiplierFor(target);

        const bool inScope = g_settings.scopeAll || (aggressor && aggressor->IsPlayerRef());
        if (site == Site::kMelee && inScope && p.mode != Mode::kObserve) {
            hit->totalDamage = p.mode == Mode::kSet ? g_settings.value : 0.0f;
            p.modified = true;
        }
        p.totalAfter = hit->totalDamage;
        return true;
    }

    // 원래 함수가 끝난 뒤: replace면 우리 피해를 따로 주고, 한 줄로 적는다.
    void Finish(Site site, RE::Actor* target, Pending& p)
    {
        const float healthAfter = Health(target);
        SKSE::log::info("HIT\t{}\t{}\t{}\t{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{}\t{:X}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.1f}\t{:.3f}",
            p.t, Tid(), site == Site::kMelee ? "melee" : "queue", ModeName(p.mode), p.modified ? 1 : 0, p.aggressorId, p.targetId, p.weaponId, p.sourceRefId,
            p.sourceRefType, p.flags, p.totalBefore, p.totalAfter, p.physical, p.percentBlocked, p.resistedPhysical, p.resistedTyped, p.sneakBonus,
            p.bonusHealthMult, p.reflected, p.stagger, p.healthBefore, healthAfter, p.maxHealth, p.multiplier);

        if (p.modified && p.mode == Mode::kReplace && g_settings.value > 0.0f && !target->IsDead()) {
            const bool dead = target->DoDamage(g_settings.value, p.aggressor.get(), false);
            SKSE::log::info("OWN\t{}\t{}\t{:08X}\t{:.3f}\t{:.3f}\t{}", Micros(), Tid(), p.targetId, g_settings.value, Health(target), dead ? 1 : 0);
            QueueLateRead(target, p.targetId, p.t);
        } else if (p.totalAfter > 0.0f && std::fabs(healthAfter - p.healthBefore) < 0.001f && !target->IsDead()) {
            // 원래 함수 안에서 체력이 줄지 않았다: 작업 큐로 미뤄졌거나, 막혔거나, 무적이다. 조금 뒤에 다시 읽는다.
            QueueLateRead(target, p.targetId, p.t);
        }
    }

    template <Site S>
    struct ApplyHitHook
    {
        static void Thunk(RE::Actor* target, RE::HitData* hit)
        {
            Pending p;
            bool    began = false;
            try {
                began = Begin(S, target, hit, p);
            } catch (...) {
                began = false;
            }
            original(target, hit);  // 어떤 경우에도 정확히 한 번 부른다
            if (began) {
                try {
                    Finish(S, target, p);
                } catch (...) {
                }
            }
        }

        static inline REL::Relocation<decltype(Thunk)> original;
    };

    std::uintptr_t CallTarget(std::uintptr_t site)
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(site);
        if (bytes[0] != 0xE8) {
            return 0;
        }
        std::int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        return site + 5 + static_cast<std::intptr_t>(displacement);
    }

    bool IsExecutable(std::uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) {
            return false;
        }
        constexpr DWORD kExecute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        return (info.Protect & kExecute) != 0;
    }

    HMODULE ModuleOf(std::uintptr_t address)
    {
        HMODULE module = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module)) {
            return nullptr;
        }
        return module;
    }

    std::string FileNameOf(HMODULE module)
    {
        std::wstring buffer(MAX_PATH, L'\0');
        const auto   length = ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        buffer.resize(length);
        const auto text = std::filesystem::path(buffer).filename().u8string();
        return { reinterpret_cast<const char*>(text.data()), text.size() };
    }

    // 다른 SKSE 플러그인이 write_call로 가로챈 자리의 호출 대상은 대개 SKSE 분기 풀의 `jmp [rip+0]`(FF 25 00000000)과
    // 그 뒤의 절대 주소다. 그 모양이면 절대 주소를, 아니면 0을 돌려준다.
    std::uintptr_t FollowAbsoluteJump(std::uintptr_t address)
    {
        constexpr std::array<std::uint8_t, 6> kJump{ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
        if (std::memcmp(reinterpret_cast<const void*>(address), kJump.data(), kJump.size()) != 0) {
            return 0;
        }
        std::uint64_t destination = 0;
        std::memcpy(&destination, reinterpret_cast<const void*>(address + kJump.size()), sizeof(destination));
        return static_cast<std::uintptr_t>(destination);
    }

    // 호출 대상이 이미 다른 모드의 가로채기인지 가린다. 그렇다고 볼 수 있으면 그 모드의 파일 이름을, 아니면 빈 문자열을 돌려준다.
    // 실행 파일 안의 다른 함수, 실행할 수 없는 메모리, 주인을 알 수 없는 코드는 모두 아니라고 본다(명령 중간에 call을 쓰지 않게).
    std::string ForeignHookOwner(std::uintptr_t target)
    {
        const auto mainModule = ::GetModuleHandleW(nullptr);
        if (!IsExecutable(target)) {
            return {};
        }
        auto owner = ModuleOf(target);
        if (!owner) {
            const auto destination = FollowAbsoluteJump(target);
            if (destination == 0 || !IsExecutable(destination)) {
                return {};
            }
            owner = ModuleOf(destination);
        }
        if (!owner || owner == mainModule) {
            return {};
        }
        return FileNameOf(owner);
    }

    // 자리가 38586을 부르는 call인지 확인하고 가로챈다. 설치하지 못한 까닭을 돌려준다(설치하면 빈 문자열).
    template <Site S>
    std::string Install(const char* label, std::uint64_t id, std::ptrdiff_t offset)
    {
        const auto site = REL::ID(id).address() + offset;
        const auto expected = REL::ID(kApplyHitId).address();
        const auto target = CallTarget(site);
        if (target == 0) {
            return std::format("{} 자리(ID {} +0x{:X})가 call이 아님", label, id, offset);
        }
        if (target != expected) {
            const auto owner = ForeignHookOwner(target);
            if (owner.empty()) {
                return std::format("{} 자리(ID {} +0x{:X})가 ID {}가 아닌 곳(0x{:X})을 부르고, 다른 모드의 가로채기로도 보이지 않음", label, id, offset,
                    kApplyHitId, target);
            }
            SKSE::log::warn("{} 자리는 이미 {}가 가로챘음. 그 위에 겹쳐 설치함(그 모드는 이 플러그인이 바꾼 값을 받음)", label, owner);
        }
        ApplyHitHook<S>::original = SKSE::GetTrampoline().write_call<5>(site, ApplyHitHook<S>::Thunk);
        SKSE::log::info("{} 자리(ID {} +0x{:X})를 가로챔", label, id, offset);
        return {};
    }

    void InstallHooks()
    {
        std::string problem;
        if (!REL::Module::IsAE() || REL::Module::get().version() != kVerifiedRuntime) {
            // 오프셋(+0x4A8, +0xA3D)은 이 판의 실행 파일에서만 확인했다. 다른 판에서는 명령 중간을 가리킬 수 있다.
            problem = std::format("런타임 {}은 자리를 확인한 판(AE {})이 아니라 설치하지 않음", REL::Module::get().version().string("."),
                kVerifiedRuntime.string("."));
        } else {
            problem = Install<Site::kMelee>("근접 타격", kMeleeHitId, kMeleeCallOffset);
            if (problem.empty()) {
                g_meleeInstalled = true;
                if (g_settings.hookQueue) {
                    if (const auto queueProblem = Install<Site::kQueue>("작업 큐", kQueueId, kQueueCallOffset); queueProblem.empty()) {
                        g_queueInstalled = true;
                    } else {
                        SKSE::log::error("작업 큐 쪽을 설치하지 못함(근접 쪽은 설치됨): {}", queueProblem);
                    }
                } else {
                    SKSE::log::info("작업 큐 쪽은 가로채지 않음(설정 HookQueue = false)");
                }
            }
        }
        if (!problem.empty()) {
            SKSE::log::error("설치하지 못함: {}", problem);
        }
        const std::scoped_lock lock(g_installMutex);
        g_installProblem = std::move(problem);
    }

    // ---------------------------------------------------------------- 이벤트

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
            if (!event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            try {
                const auto* target = event->target.get();
                const auto* cause = event->cause.get();
                const bool  player = (target && target->IsPlayerRef()) || (cause && cause->IsPlayerRef());
                if (player) {
                    SKSE::log::info("EVHIT\t{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{}", Micros(), Tid(), target ? target->GetFormID() : 0, cause ? cause->GetFormID() : 0,
                        event->source, event->projectile, static_cast<unsigned>(event->flags.underlying()));
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
            if (!event) {
                return RE::BSEventNotifyControl::kContinue;
            }
            try {
                const auto* dying = event->actorDying.get();
                const auto* killer = event->actorKiller.get();
                SKSE::log::info("EVDEATH\t{}\t{:08X}\t{:08X}\t{}", Micros(), dying ? dying->GetFormID() : 0, killer ? killer->GetFormID() : 0, event->dead ? 1 : 0);
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

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
            if (!events || g_settings.modeKey == 0 || !g_meleeInstalled) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto* event = *events; event; event = event->next) {
                const auto* button = event->AsButtonEvent();
                if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard || button->GetIDCode() != g_settings.modeKey || !button->IsDown()) {
                    continue;
                }
                const auto next = static_cast<Mode>((g_mode.load() + 1) % static_cast<int>(Mode::kCount));
                g_mode = static_cast<int>(next);
                SKSE::log::info("MODE\t{}\t{}", Micros(), ModeName(next));
                if (const auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([next] {
                        try {
                            ShowNotice(std::format("{} {} 모드({})", kNoticePrefix, ModeLabel(next), ModeName(next)), false);
                        } catch (...) {
                        }
                    });
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---------------------------------------------------------------- 알림과 늦은 읽기

    // 불러오기가 끝난 뒤 게임이 돌 때 한 번 띄운다. 메인 스레드(SKSE 작업)에서 부른다.
    void DeliverLoadNotice()
    {
        auto*       ui = RE::UI::GetSingleton();
        const auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded() || (ui && ui->GameIsPaused())) {
            return;  // 아직 아님. 다음 틱에 다시 본다
        }
        g_noticePending = false;
        std::string problem;
        {
            const std::scoped_lock lock(g_installMutex);
            problem = g_installProblem;
        }
        if (!problem.empty()) {
            ShowNotice(std::format("{} 설치하지 못함: {}. PoEProtoHitDamage.log를 보세요", kNoticePrefix, problem), true);
            return;
        }
        ShowNotice(std::format("{} {} 모드. {}로 바꿈{}", kNoticePrefix, ModeLabel(CurrentMode()), ModeKeyName(),
                       g_queueInstalled ? "" : " (작업 큐 쪽은 가로채지 않음)"),
            false);
    }

    void ReadLate(const std::vector<LateRead>& items)
    {
        for (const auto& item : items) {
            const auto actor = item.target.get();
            if (!actor) {
                SKSE::log::info("LATE\t{}\t{:08X}\t-\t{}", Micros(), item.targetId, item.hitTime);
                continue;
            }
            SKSE::log::info("LATE\t{}\t{:08X}\t{:.3f}\t{}", Micros(), item.targetId, Health(actor.get()), item.hitTime);
        }
    }

    // 알림과 늦은 읽기를 메인 스레드에 맡기는 스레드. 작업 안에서 다음 작업을 스스로 맡기지 않는다
    // (SKSE가 맡겨진 작업이 없어질 때까지 한자리에서 돌려 게임이 멈춘 적이 있음, 설계-현황 4.6).
    void TickerMain()
    {
        for (;;) {
            std::this_thread::sleep_for(kTick);
            const auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                continue;
            }
            if (g_noticePending && !g_noticeQueued.exchange(true)) {
                tasks->AddTask([] {
                    g_noticeQueued = false;
                    try {
                        DeliverLoadNotice();
                    } catch (...) {
                    }
                });
            }
            std::vector<LateRead> due;
            {
                const std::scoped_lock lock(g_lateMutex);
                const auto             now = Clock::now();
                for (auto it = g_late.begin(); it != g_late.end();) {
                    if (it->due <= now) {
                        due.push_back(std::move(*it));
                        it = g_late.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            if (!due.empty()) {
                tasks->AddTask([due = std::move(due)] {
                    try {
                        ReadLate(due);
                    } catch (...) {
                    }
                });
            }
        }
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
                SKSE::log::error("입력 장치 관리자를 찾지 못함. 모드 키를 쓸 수 없습니다.");
            }
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            if (g_meleeInstalled) {
                if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
                    events->AddEventSink<RE::TESHitEvent>(&HitSink::Get());
                    events->AddEventSink<RE::TESDeathEvent>(&DeathSink::Get());
                } else {
                    SKSE::log::error("이벤트 원천을 찾지 못함. 적중·사망 이벤트는 적히지 않습니다.");
                }
            }
            if (!g_tickerStarted.exchange(true)) {
                std::thread(TickerMain).detach();
            }
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            g_noticePending = true;
            break;
        default:
            break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SKSE::Init(skse, { .log = false, .trampoline = true, .trampolineSize = 64 });
    InitializeLogging();
    g_settings = LoadSettings();
    g_mode = static_cast<int>(g_settings.startMode);
    SKSE::log::info("설정: ModeKey=0x{:02X} StartMode={} Value={:.3f} Scope={} HookQueue={} Notify={}", g_settings.modeKey, ModeName(g_settings.startMode),
        g_settings.value, g_settings.scopeAll ? "all" : "player", g_settings.hookQueue, g_settings.notify);
    SKSE::log::info("런타임 {}", skse->RuntimeVersion().string("."));
    InstallHooks();
    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("SKSE 메시지 리스너를 등록하지 못함. 이벤트와 알림이 없습니다.");
        return true;
    }
    SKSE::log::info("PoEProtoHitDamage 준비 완료(근접 {}, 작업 큐 {})", g_meleeInstalled ? "가로챔" : "안 함", g_queueInstalled ? "가로챔" : "안 함");
    return true;
}
