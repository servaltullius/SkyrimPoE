// PoE Proto 9: 받는 마법 피해(35086에 이르는 vtable 15개와 직접 호출 8곳)와 근접을 포함한 일곱 타격 자리를 시험한다.
// README의 실행 전 판정 기준대로 기록한다. 자리는 AE 1.6.1170에서만 설치한다.
// CommonLibSSE-NG v16.4.0을 쓰며 원래 함수는 어떤 모드에서든 정확히 한 번 부른다.

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
    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoReceived.ini";
    constexpr auto kLogName = "PoEProtoReceived";
    constexpr auto kNoticePrefix = "[PoE Proto 9]";

    constexpr std::uint64_t kApplyHitId = 38586;
    constexpr std::uint64_t kModifyActorValueId = 35086;
    constexpr std::uint64_t kSetEffectivenessId = 34052;
    constexpr std::uint64_t kExplosionDamageStepsId = 44016;
    constexpr std::size_t kModifyActorValueSlot = 0x20;
    constexpr REL::Version kVerifiedRuntime{ 1, 6, 1170, 0 };
    constexpr auto kTick = std::chrono::milliseconds(100);
    constexpr auto kLateDelay = std::chrono::milliseconds(300);
    constexpr float kMaxValue = 100.0f;
    constexpr std::size_t kHitSiteCount = 7;

    // 받는 마법은 모두 ValueModifierEffect::ModifyActorValue(ID 35086)를 지난다. 거기에 이르는 길은 두 가지다
    // (실행 파일에서 35086의 주소를 담은 곳과 35086으로 가는 E8/E9를 모두 찾음, 2026-11-18).
    // (1) 0x20 슬롯이 35086인 vtable 15개. 체력을 깎는 것은 앞의 다섯이지만, 체력만 거르므로 모두 건다.
    struct MagicVtable
    {
        std::string_view name;
        REL::VariantID id;
    };
    constexpr std::array kMagicVtables{
        MagicVtable{ "ValueModifierEffect", RE::VTABLE_ValueModifierEffect[0] },
        MagicVtable{ "AccumulatingValueModifierEffect", RE::VTABLE_AccumulatingValueModifierEffect[0] },
        MagicVtable{ "PeakValueModifierEffect", RE::VTABLE_PeakValueModifierEffect[0] },
        MagicVtable{ "AbsorbEffect", RE::VTABLE_AbsorbEffect[0] },
        MagicVtable{ "TargetValueModifierEffect", RE::VTABLE_TargetValueModifierEffect[0] },
        MagicVtable{ "DarknessEffect", RE::VTABLE_DarknessEffect[0] },
        MagicVtable{ "InvisibilityEffect", RE::VTABLE_InvisibilityEffect[0] },
        MagicVtable{ "NightEyeEffect", RE::VTABLE_NightEyeEffect[0] },
        MagicVtable{ "ParalysisEffect", RE::VTABLE_ParalysisEffect[0] },
        MagicVtable{ "RallyEffect", RE::VTABLE_RallyEffect[0] },
        MagicVtable{ "DemoralizeEffect", RE::VTABLE_DemoralizeEffect[0] },
        MagicVtable{ "TurnUndeadEffect", RE::VTABLE_TurnUndeadEffect[0] },
        MagicVtable{ "BanishEffect", RE::VTABLE_BanishEffect[0] },
        MagicVtable{ "CalmEffect", RE::VTABLE_CalmEffect[0] },
        MagicVtable{ "FrenzyEffect", RE::VTABLE_FrenzyEffect[0] },
    };
    // (2) 0x20을 자기 함수로 덮어쓰고 그 안에서 35086을 직접 부르는 자리 8개. 냉기·전격 피해(DualValueModifier)가 여기로 온다.
    // DualValueModifierEffect의 두 번째 호출은 보조 액터 값(지구력·매지카)을 넘기므로 체력 거름에 걸리지 않는다.
    struct MagicCall
    {
        std::string_view name;
        std::uint64_t id;
        std::ptrdiff_t offset;
    };
    constexpr std::array kMagicCalls{
        MagicCall{ "DualValueModifierEffect+0x28", 34324, 0x28 },
        MagicCall{ "DualValueModifierEffect+0x5C", 34324, 0x5C },
        MagicCall{ "GrabActorEffect+0x34", 34332, 0x34 },
        MagicCall{ "GrabActorEffect+0x84", 34332, 0x84 },
        MagicCall{ "ValueAndConditionsEffect+0x36", 35067, 0x36 },
        MagicCall{ "ValueAndConditionsEffect+0x4A", 35067, 0x4A },
        MagicCall{ "ValueAndConditionsEffect+0x5C", 35067, 0x5C },
        MagicCall{ "ValueAndConditionsEffect+0x7E", 35067, 0x7E },
    };
    constexpr std::size_t kMagicVtableCount = kMagicVtables.size();
    constexpr std::size_t kMagicCallCount = kMagicCalls.size();
    constexpr std::size_t kMagicSiteCount = kMagicVtableCount + kMagicCallCount;
    constexpr MagicCall kMagnitudeStepsCall{ "AddTarget::SetEffectiveness", 34547, 0x656 };
    constexpr MagicCall kExplosionStepsCall{ "Explosion::DamageSteps", 44004, 0xC1 };
    constexpr std::array kHitStepsCalls{
        MagicCall{ "populate", 44001, 0x34E },
        MagicCall{ "arrow", 44002, 0x10B },
    };
    // write_call<5> 하나마다 절대 점프 스텁 14바이트를 쓴다. vtable에는 트램펄린이 필요 없다.
    constexpr std::size_t kTrampolineSize = (kHitSiteCount + kMagicCallCount + 2 + kHitStepsCalls.size()) * 14;
    constexpr std::string_view MagicSiteName(std::size_t index)
    {
        return index < kMagicVtableCount ? kMagicVtables[index].name : kMagicCalls[index - kMagicVtableCount].name;
    }

    enum class Mode : int { kObserve, kScale, kCount };
    constexpr std::array<std::string_view, 2> kModeNames{ "observe", "scale" };
    constexpr std::array<std::string_view, 2> kModeLabels{ "관찰", "배율" };
    enum class Site { kExplosion, kBeam, kNonActorArrow, kTrap, kCollision, kUnknown, kMelee };
    struct HitSite
    {
        std::string_view name;
        std::string_view label;
        std::uint64_t id;
        std::ptrdiff_t offset;
    };
    // 디컴파일과 호출 검색에서 찾은 E8 call 자리. 각각 독립적으로 확인한다.
    constexpr HitSite kExplosionSite{ "explosion", "폭발", 43849, 0x412 };
    constexpr HitSite kBeamSite{ "beam", "광선 투사체", 44123, 0x269 };
    constexpr HitSite kNonActorArrowSite{ "nonactor_arrow", "쏜 쪽이 액터가 아닌 화살", 44204, 0x392 };
    constexpr HitSite kTrapSite{ "trap", "함정", 37509, 0x43 };
    constexpr HitSite kCollisionSite{ "collision", "물체 충돌", 38185, 0x21C };
    constexpr HitSite kUnknownSite{ "unknown", "정체를 모르는 호출", 49044, 0x191 };
    constexpr HitSite kMeleeSite{ "melee", "근접 타격", 38627, 0x4A8 };
    constexpr std::array<HitSite, kHitSiteCount> kHitSites{
        kExplosionSite, kBeamSite, kNonActorArrowSite, kTrapSite, kCollisionSite, kUnknownSite, kMeleeSite
    };

    struct Settings
    {
        std::uint32_t modeKey = 0x3C;
        Mode startMode = Mode::kObserve;
        float value = 0.5f;
        bool logAll = false;
        bool readResistance = true;
        bool logMagnitudeSteps = true;
        bool logExplosionSteps = true;
        bool logHitSteps = true;
        bool hookMagic = true;
        bool hookHits = true;
        bool notify = true;
    };
    struct DamageRead
    {
        std::int64_t t = 0;
        unsigned long thread = 0;
        Mode mode = Mode::kObserve;
        bool modified = false;
        RE::FormID targetId = 0;
        float healthBefore = 0.0f;
        float maxHealth = 0.0f;
        float multiplier = 1.0f;
    };
    struct HitRead : DamageRead
    {
        RE::FormID aggressorId = 0;
        RE::FormID weaponId = 0;
        RE::FormID sourceRefId = 0;
        std::string sourceRefType = "-";
        std::uint32_t flags = 0;
        float totalBefore = 0.0f;
        float totalAfter = 0.0f;
        float physical = 0.0f;
        float percentBlocked = 0.0f;
    };
    struct MagicRead : DamageRead
    {
        RE::FormID casterId = 0;
        RE::FormID spellId = 0;
        RE::FormID effectId = 0;
        std::string spellType = "-";
        std::string castingType = "-";
        std::string resistVariable = "-";
        std::string resistance = "-";
        std::uint32_t flags = 0;
        float duration = 0.0f;
        float elapsed = 0.0f;
        float magnitude = 0.0f;
        float baseMagnitude = 0.0f;
        float valueBefore = 0.0f;
        float valueAfter = 0.0f;
    };
    struct MagnitudeRead
    {
        std::int64_t t = 0;
        unsigned long thread = 0;
        RE::Actor* target = nullptr;
        RE::NiPointer<RE::Actor> caster;
        RE::MagicItem* spell = nullptr;
        RE::FormID targetId = 0;
        std::string casterId = "-";
        RE::FormID spellId = 0;
        RE::FormID effectId = 0;
        unsigned spellType = 0;
        unsigned castingType = 0;
        float baseMagnitude = 0.0f;
        float magBefore = 0.0f;
        float durBefore = 0.0f;
        float effectiveness = 0.0f;
        std::string resistance = "-";
        std::string power = "-";
    };
    struct ExplosionRead
    {
        std::int64_t t = 0;
        unsigned long thread = 0;
        RE::FormID targetId = 0;
        float totalBefore = 0.0f;
    };
    struct HitStepRead
    {
        std::int64_t t = 0;
        unsigned long thread = 0;
        RE::FormID targetId = 0;
        float totalBefore = 0.0f;
    };
    struct LateRead
    {
        RE::ActorHandle target;
        RE::FormID targetId = 0;
        std::int64_t hitTime = 0;
        Clock::time_point due;
    };

    Settings g_settings;
    const Clock::time_point g_epoch = Clock::now();
    std::atomic<std::int64_t> g_lastMicros{ -1 };
    std::atomic<int> g_mode{ 0 };
    std::atomic<unsigned> g_installed{ 0 };
    std::atomic<bool> g_noticePending{ false };
    std::atomic<bool> g_noticeQueued{ false };
    std::atomic<bool> g_tickerStarted{ false };
    std::once_flag g_installOnce;
    std::mutex g_installMutex;
    std::string g_installProblem;
    std::mutex g_lateMutex;
    std::vector<LateRead> g_late;

    unsigned long Tid() { return ::GetCurrentThreadId(); }
    std::int64_t Micros()
    {
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_epoch).count();
        auto previous = g_lastMicros.load(std::memory_order_relaxed);
        // 같은 마이크로초에 여러 스레드가 들어와도 LATE가 가리키는 시각은 겹치지 않게 한다.
        for (;;) {
            const auto next = now > previous ? now : previous + 1;
            if (g_lastMicros.compare_exchange_weak(previous, next, std::memory_order_relaxed)) {
                return next;
            }
        }
    }
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
            auto logger = std::make_shared<spdlog::logger>("poe_proto_received", std::move(sink));
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
                    SKSE::log::warn("설정: StartMode '{}'는 observe, scale가 아니어서 기본값 유지", value);
                }
            } else if (key == "Value") {
                float      parsed = 0.0f;
                const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (result.ec == std::errc{} && result.ptr == value.data() + value.size() && parsed >= 0.0f && parsed <= kMaxValue) {
                    settings.value = parsed;
                } else {
                    SKSE::log::warn("설정: Value '{}'는 0 이상 {} 이하의 수가 아니어서 기본값 유지", value, kMaxValue);
                }
            } else if (key == "LogAll") {
                ParseBool(key, value, settings.logAll);
            } else if (key == "ReadResistance") {
                ParseBool(key, value, settings.readResistance);
            } else if (key == "LogMagnitudeSteps") {
                ParseBool(key, value, settings.logMagnitudeSteps);
            } else if (key == "LogExplosionSteps") {
                ParseBool(key, value, settings.logExplosionSteps);
            } else if (key == "LogHitSteps") {
                ParseBool(key, value, settings.logHitSteps);
            } else if (key == "HookMagic") {
                ParseBool(key, value, settings.hookMagic);
            } else if (key == "HookHits") {
                ParseBool(key, value, settings.hookHits);
            } else if (key == "Notify") {
                ParseBool(key, value, settings.notify);
            } else {
                SKSE::log::warn("설정: 알 수 없는 키 '{}'", key);
            }
        }
        return settings;
    }

    // 알림에 붙일 모드 키 안내. 모드 키를 쓰지 않으면(0) 바꾸는 법이 없다고 알린다.
    std::string ModeKeyHint()
    {
        if (g_settings.modeKey == 0) {
            return "(모드 키 없음. INI의 StartMode로 고름)";
        }
        const auto name = g_settings.modeKey == 0x3C ? std::string("F2") : std::format("스캔 코드 0x{:02X}", g_settings.modeKey);
        return std::format("{} 키로 바꿈", name);
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
    void ReadBefore(RE::Actor* target, DamageRead& p)
    {
        p.t = Micros();
        p.thread = Tid();
        p.mode = CurrentMode();
        p.targetId = target->GetFormID();
        p.healthBefore = Health(target);
        p.maxHealth = target->GetActorValueMax(RE::ActorValue::kHealth);
        p.multiplier = MultiplierFor(target);
    }
    void MaybeReadLate(RE::Actor* target, const DamageRead& p, float healthAfter, float valueAfter)
    {
        if (valueAfter != 0.0f && std::fabs(healthAfter - p.healthBefore) < 0.001f && !target->IsDead()) {
            QueueLateRead(target, p.targetId, p.t);
        }
    }

    bool BeginHit(RE::Actor* target, RE::HitData* hit, HitRead& p)
    {
        if (!target || !hit || (!target->IsPlayerRef() && !g_settings.logAll)) {
            return false;
        }
        ReadBefore(target, p);
        if (const auto aggressor = hit->aggressor.get()) {
            p.aggressorId = aggressor->GetFormID();
        }
        p.weaponId = hit->weapon ? hit->weapon->GetFormID() : 0;
        if (const auto source = hit->sourceRef.get()) {
            p.sourceRefId = source->GetFormID();
            p.sourceRefType = std::format("{}", static_cast<unsigned>(source->GetFormType()));
        }
        p.flags = hit->flags.underlying();
        p.totalBefore = hit->totalDamage;
        p.physical = hit->physicalDamage;
        p.percentBlocked = hit->percentBlocked;
        p.modified = target->IsPlayerRef() && p.mode == Mode::kScale;
        p.totalAfter = p.modified ? p.totalBefore * g_settings.value : p.totalBefore;
        // 읽기와 계산을 모두 마친 뒤에만 바꾼다. 앞에서 예외가 나면 원래 값으로 부른다.
        hit->totalDamage = p.totalAfter;
        return true;
    }
    void FinishHit(Site site, RE::Actor* target, const HitRead& p)
    {
        const float healthAfter = Health(target);
        SKSE::log::info("HIT\t{}\t{}\t{}\t{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{}\t{:X}\t{:.6f}\t{:.6f}\t{:.6f}\t{:.6f}\t{:.6f}\t{:.6f}\t{:.6f}\t{:.6f}",
            p.t, p.thread, kHitSites[static_cast<std::size_t>(site)].name, ModeName(p.mode), p.modified ? 1 : 0,
            p.aggressorId, p.targetId, p.weaponId, p.sourceRefId, p.sourceRefType, p.flags, p.totalBefore, p.totalAfter,
            p.physical, p.percentBlocked, p.healthBefore, healthAfter, p.maxHealth, p.multiplier);
        MaybeReadLate(target, p, healthAfter, p.totalAfter);
    }
    template <Site S>
    struct ApplyHitHook
    {
        static void Thunk(RE::Actor* target, RE::HitData* hit)
        {
            HitRead p;
            bool began = false;
            try {
                began = BeginHit(target, hit, p);
            } catch (...) {
            }
            original(target, hit);  // 어떤 경우에도 정확히 한 번 부른다
            if (began) {
                try {
                    FinishHit(S, target, p);
                } catch (...) {
                }
            }
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    bool BeginMagic(RE::ValueModifierEffect* self, RE::Actor* actor, float value, RE::ActorValue av, MagicRead& p)
    {
        if (!self || !actor || !(value < 0.0f)) {
            return false;
        }
        const auto actualValue = av == RE::ActorValue::kNone ? self->actorValue : av;
        if (actualValue != RE::ActorValue::kHealth || (!actor->IsPlayerRef() && !g_settings.logAll)) {
            return false;
        }
        ReadBefore(actor, p);
        if (const auto caster = self->GetCasterActor()) {
            p.casterId = caster->GetFormID();
        }
        if (self->spell) {
            p.spellId = self->spell->GetFormID();
            p.spellType = std::format("{}", static_cast<unsigned>(self->spell->GetSpellType()));
            p.castingType = std::format("{}", static_cast<unsigned>(self->spell->GetCastingType()));
        }
        if (self->effect) {
            p.baseMagnitude = self->effect->effectItem.magnitude;
            if (const auto* base = self->effect->baseEffect) {
                p.effectId = base->GetFormID();
                p.resistVariable = std::format("{}", static_cast<int>(base->data.resistVariable));
                p.flags = base->data.flags.underlying();
            }
        }
        // source가 없으면 nullptr 그대로 넘긴다. spell/effect가 없거나 설정을 끄면 엔진을 부르지 않는다.
        if (g_settings.readResistance && self->spell && self->effect) {
            if (auto* magicTarget = actor->AsMagicTarget()) {
                p.resistance = std::format("{:.6f}", magicTarget->CheckResistance(self->spell, self->effect, self->source));
            }
        }
        p.duration = self->duration;
        p.elapsed = self->elapsedSeconds;
        p.magnitude = self->magnitude;
        p.valueBefore = value;
        p.modified = actor->IsPlayerRef() && p.mode == Mode::kScale;
        p.valueAfter = p.modified ? value * g_settings.value : value;
        return true;
    }
    void FinishMagic(std::string_view site, RE::Actor* actor, const MagicRead& p)
    {
        const float healthAfter = Health(actor);
        SKSE::log::info("MAG\t{}\t{}\t{}\t{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{}\t{}\t{:08X}\t{}\t{:X}\t{:.6f}\t{:.6f}\t{:.6f}\t{:.6f}\t{}\t{:.6f}\t{:.6f}\t{:.6f}\t{:.6f}\t{:.6f}\t{:.6f}",
            p.t, p.thread, ModeName(p.mode), p.modified ? 1 : 0, site, p.targetId,
            p.casterId, p.spellId, p.spellType, p.castingType, p.effectId, p.resistVariable, p.flags, p.duration, p.elapsed,
            p.magnitude, p.baseMagnitude, p.resistance, p.valueBefore, p.valueAfter, p.healthBefore, healthAfter, p.maxHealth, p.multiplier);
        MaybeReadLate(actor, p, healthAfter, p.valueAfter);
    }
    // I는 MagicSiteName의 번호. vtable 슬롯과 직접 호출 자리 모두 같은 인자(this, actor, value, av)로 들어온다.
    template <std::size_t I>
    struct MagicHook
    {
        static void Thunk(RE::ValueModifierEffect* self, RE::Actor* actor, float value, RE::ActorValue av)
        {
            MagicRead p;
            bool began = false;
            try {
                began = BeginMagic(self, actor, value, av, p);
            } catch (...) {
            }
            original(self, actor, began ? p.valueAfter : value, av);  // 어떤 경우에도 정확히 한 번 부른다
            if (began) {
                try {
                    FinishMagic(MagicSiteName(I), actor, p);
                } catch (...) {
                }
            }
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    bool BeginMagnitudeSteps(RE::ActiveEffect* self, float effectiveness, MagnitudeRead& p)
    {
        if (!self || !self->spell || !self->effect || !self->effect->baseEffect) {
            return false;
        }
        auto* target = self->GetTargetActor();
        if (!target || (!target->IsPlayerRef() && !g_settings.logAll) || !self->effect->baseEffect->IsDetrimental()) {
            return false;
        }
        p.t = Micros();
        p.thread = Tid();
        p.target = target;
        p.caster = self->GetCasterActor();
        p.spell = self->spell;
        p.targetId = target->GetFormID();
        if (p.caster) {
            p.casterId = std::format("{:08X}", p.caster->GetFormID());
        }
        p.spellId = p.spell->GetFormID();
        p.effectId = self->effect->baseEffect->GetFormID();
        p.spellType = static_cast<unsigned>(p.spell->GetSpellType());
        p.castingType = static_cast<unsigned>(p.spell->GetCastingType());
        p.baseMagnitude = self->effect->effectItem.magnitude;
        p.magBefore = self->magnitude;
        p.durBefore = self->duration;
        p.effectiveness = effectiveness;
        // AddTarget과 같이 능력이나 저항을 무시하는 주문이면 저항 계산을 부르지 않는다.
        if (g_settings.readResistance) {
            if (auto* magicTarget = target->AsMagicTarget()) {
                const float resistance = p.spell->GetSpellType() == RE::MagicSystem::SpellType::kAbility || p.spell->IgnoresResistance() ?
                    1.0f : magicTarget->CheckResistance(p.spell, self->effect, self->source);
                p.resistance = std::format("{:.6f}", resistance);
                if (resistance != 0.0f) {
                    p.power = std::format("{:.6f}", effectiveness / resistance);
                }
            }
        }
        return true;
    }
    void FinishMagnitudeSteps(RE::ActiveEffect* self, const MagnitudeRead& p)
    {
        const float magAfter = self->magnitude;
        const float durAfter = self->duration;
        std::string casterFromOne = "-";
        std::string targetFromOne = "-";
        std::string predicted = "-";
        // 엔진은 이 호출 뒤에 AdjustForPerks를 부른다. 실제 효과 대신 지역 변수에만 같은 인자로 적용한다.
        if (p.caster) {
            using EntryPoint = RE::BGSEntryPoint::ENTRY_POINT;
            float casterValue = 1.0f;
            RE::BGSEntryPoint::HandleEntryPoint(EntryPoint::kModSpellMagnitude, p.caster.get(), p.spell, p.target, &casterValue);
            float targetValue = 1.0f;
            RE::BGSEntryPoint::HandleEntryPoint(EntryPoint::kModIncomingSpellMagnitude, p.target, p.spell, &targetValue);
            // Set·Add 연산도 재현하도록 1에서 구한 두 값을 곱하지 않고 엔진 순서대로 다시 적용한다.
            float magnitude = magAfter;
            RE::BGSEntryPoint::HandleEntryPoint(EntryPoint::kModSpellMagnitude, p.caster.get(), p.spell, p.target, &magnitude);
            RE::BGSEntryPoint::HandleEntryPoint(EntryPoint::kModIncomingSpellMagnitude, p.target, p.spell, &magnitude);
            casterFromOne = std::format("{:.6f}", casterValue);
            targetFromOne = std::format("{:.6f}", targetValue);
            predicted = std::format("{:.6f}", magnitude);
        }
        SKSE::log::info("AMP\t{}\t{}\t{:08X}\t{}\t{:08X}\t{}\t{}\t{:08X}\t{:.6f}\t{:.6f}\t{:.6f}\t{}\t{}\t{:.6f}\t{:.6f}\t{:.6f}\t{}\t{}\t{}",
            p.t, p.thread, p.targetId, p.casterId, p.spellId, p.spellType, p.castingType, p.effectId,
            p.baseMagnitude, p.magBefore, p.effectiveness, p.resistance, p.power, magAfter, p.durBefore, durAfter,
            casterFromOne, targetFromOne, predicted);
    }
    struct MagnitudeStepsHook
    {
        static void Thunk(RE::ActiveEffect* self, float effectiveness, bool flag)
        {
            MagnitudeRead p;
            bool began = false;
            try {
                began = BeginMagnitudeSteps(self, effectiveness, p);
            } catch (...) {
            }
            original(self, effectiveness, flag);  // 모드와 관계없이 받은 인자 그대로 정확히 한 번 부른다
            if (began) {
                try {
                    FinishMagnitudeSteps(self, p);
                } catch (...) {
                }
            }
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    bool BeginExplosionSteps(RE::HitData* hit, ExplosionRead& p)
    {
        if (!hit) {
            return false;
        }
        const auto target = hit->target.get();
        if (!target || (!target->IsPlayerRef() && !g_settings.logAll)) {
            return false;
        }
        p.t = Micros();
        p.thread = Tid();
        p.targetId = target->GetFormID();
        p.totalBefore = hit->totalDamage;
        return true;
    }
    void FinishExplosionSteps(RE::HitData* hit, const ExplosionRead& p)
    {
        const float totalAfter = hit->totalDamage;
        const float physical = hit->physicalDamage;
        const float percentBlocked = hit->percentBlocked;
        const float sneak = hit->bonusHealthDamageMult;
        const auto flags = hit->flags.underlying();
        RE::FormID aggressorId = 0;
        if (const auto aggressor = hit->aggressor.get()) {
            aggressorId = aggressor->GetFormID();
        }
        const RE::FormID weaponId = hit->weapon ? hit->weapon->GetFormID() : 0;
        RE::FormID sourceRefId = 0;
        if (const auto source = hit->sourceRef.get()) {
            sourceRefId = source->GetFormID();
        }
        // 결정 80의 시작 값. totalBefore가 0이면 비율을 알 수 없어 1로 계산하고 로그에는 '-'로 둔다.
        const float ratio = p.totalBefore != 0.0f ? totalAfter / p.totalBefore : 1.0f;
        const float predicted = physical * ratio * (1.0f - percentBlocked);
        SKSE::log::info("EXP\t{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{:.6f}\t{:.6f}\t{}\t{:.6f}\t{:.6f}\t{:.6f}\t{:X}\t{:.6f}",
            p.t, p.thread, aggressorId, p.targetId, weaponId, sourceRefId, p.totalBefore, totalAfter,
            p.totalBefore != 0.0f ? std::format("{:.6f}", ratio) : "-", physical, percentBlocked, sneak, flags, predicted);
    }
    struct ExplosionStepsHook
    {
        static void Thunk(RE::HitData* hit)
        {
            ExplosionRead p;
            bool began = false;
            try {
                began = BeginExplosionSteps(hit, p);
            } catch (...) {
            }
            original(hit);  // 모드와 관계없이 받은 인자 그대로 정확히 한 번 부른다
            if (began) {
                try {
                    FinishExplosionSteps(hit, p);
                } catch (...) {
                }
            }
        }
        static inline REL::Relocation<decltype(Thunk)> original;
    };

    bool BeginHitSteps(RE::HitData* hit, HitStepRead& p)
    {
        if (!hit) {
            return false;
        }
        const auto target = hit->target.get();
        if (!target || (!target->IsPlayerRef() && !g_settings.logAll)) {
            return false;
        }
        p.t = Micros();
        p.thread = Tid();
        p.targetId = target->GetFormID();
        p.totalBefore = hit->totalDamage;
        return true;
    }
    void FinishHitSteps(std::string_view site, RE::HitData* hit, const HitStepRead& p)
    {
        const float totalAfter = hit->totalDamage;
        const float sneak = hit->bonusHealthDamageMult;
        const auto flags = hit->flags.underlying();
        RE::FormID aggressorId = 0;
        if (const auto aggressor = hit->aggressor.get()) {
            aggressorId = aggressor->GetFormID();
        }
        const RE::FormID weaponId = hit->weapon ? hit->weapon->GetFormID() : 0;
        RE::FormID sourceRefId = 0;
        if (const auto source = hit->sourceRef.get()) {
            sourceRefId = source->GetFormID();
        }
        const auto ratio = p.totalBefore != 0.0f ? std::format("{:.6f}", totalAfter / p.totalBefore) : "-";
        SKSE::log::info("STEP\t{}\t{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{:08X}\t{:.6f}\t{:.6f}\t{}\t{:.6f}\t{:X}",
            p.t, p.thread, site, aggressorId, p.targetId, weaponId, sourceRefId, p.totalBefore, totalAfter, ratio, sneak, flags);
    }
    // 44001과 44002의 44016 호출을 따로 가로챈다. original은 자리마다 독립이다.
    template <std::size_t I>
    struct HitStepsHook
    {
        static void Thunk(RE::HitData* hit)
        {
            HitStepRead p;
            bool began = false;
            try {
                began = BeginHitSteps(hit, p);
            } catch (...) {
            }
            original(hit);  // 모드와 관계없이 받은 인자 그대로 정확히 한 번 부른다
            if (began) {
                try {
                    FinishHitSteps(kHitStepsCalls[I].name, hit, p);
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

    // 대상이 이 플러그인 자신의 코드(직접, 또는 SKSE 분기 풀을 거쳐)인지 본다. 두 자리가 같은 주소로 풀리는 실수가 있으면
    // 같은 경로에 가로채기가 두 겹 생겨 한 피해가 두 번 기록되고 배율이 두 번 곱해지므로, 그런 자리는 설치하지 않는다.
    bool IsOwnCode(std::uintptr_t target)
    {
        const auto self = ModuleOf(reinterpret_cast<std::uintptr_t>(&IsOwnCode));
        if (!self || !IsExecutable(target)) {
            return false;
        }
        if (ModuleOf(target) == self) {
            return true;
        }
        const auto destination = FollowAbsoluteJump(target);
        return destination != 0 && IsExecutable(destination) && ModuleOf(destination) == self;
    }

    // 자리가 기대한 함수를 부르는 E8 call인지 본다. 다른 모드가 먼저 가로챈 자리면 경고만 하고 겹쳐 설치해도 된다고 본다.
    // 설치해도 되면 빈 문자열을, 아니면 그 까닭을 돌려준다.
    std::string CheckCallSite(std::string_view label, std::uint64_t id, std::ptrdiff_t offset, std::uint64_t expectedId)
    {
        const auto site = REL::ID(id).address() + offset;
        const auto target = CallTarget(site);
        if (target == 0) {
            return std::format("{} 자리(ID {} +0x{:X})가 call이 아님", label, id, offset);
        }
        if (IsOwnCode(target)) {
            return std::format("{} 자리(ID {} +0x{:X})는 이미 이 플러그인이 가로챘음. 두 번 걸지 않음", label, id, offset);
        }
        if (target != REL::ID(expectedId).address()) {
            const auto owner = ForeignHookOwner(target);
            if (owner.empty()) {
                return std::format("{} 자리(ID {} +0x{:X})가 ID {}가 아닌 곳(0x{:X})을 부르고, 다른 모드의 가로채기로도 보이지 않음",
                    label, id, offset, expectedId, target);
            }
            SKSE::log::warn("{} 자리는 이미 {}가 가로챘음. 그 위에 겹쳐 설치함", label, owner);
        }
        return {};
    }

    // 호출 대상이 원래 함수이거나 다른 모드이면 겹쳐 설치한다. original은 자리마다 따로 보관한다.
    template <Site S>
    std::string Install()
    {
        constexpr auto spec = kHitSites[static_cast<std::size_t>(S)];
        if (auto problem = CheckCallSite(spec.label, spec.id, spec.offset, kApplyHitId); !problem.empty()) {
            return problem;
        }
        ApplyHitHook<S>::original = SKSE::GetTrampoline().write_call<5>(REL::ID(spec.id).address() + spec.offset, ApplyHitHook<S>::Thunk);
        if constexpr (S == Site::kMelee) {
            SKSE::log::info("{} ({}) 자리(ID {} +0x{:X})를 가로챔", spec.name, spec.label, spec.id, spec.offset);
        } else {
            SKSE::log::info("{} 자리(ID {} +0x{:X})를 가로챔", spec.label, spec.id, spec.offset);
        }
        return {};
    }
    template <std::size_t I>
    std::string InstallMagicCall()
    {
        constexpr auto spec = kMagicCalls[I];
        constexpr auto index = kMagicVtableCount + I;
        if (auto problem = CheckCallSite(spec.name, spec.id, spec.offset, kModifyActorValueId); !problem.empty()) {
            return problem;
        }
        MagicHook<index>::original = SKSE::GetTrampoline().write_call<5>(REL::ID(spec.id).address() + spec.offset, MagicHook<index>::Thunk);
        SKSE::log::info("{} 자리(ID {} +0x{:X}, ID {} 직접 호출)를 가로챔", spec.name, spec.id, spec.offset, kModifyActorValueId);
        return {};
    }
    std::string InstallMagnitudeSteps()
    {
        constexpr auto spec = kMagnitudeStepsCall;
        if (auto problem = CheckCallSite(spec.name, spec.id, spec.offset, kSetEffectivenessId); !problem.empty()) {
            return problem;
        }
        MagnitudeStepsHook::original = SKSE::GetTrampoline().write_call<5>(REL::ID(spec.id).address() + spec.offset, MagnitudeStepsHook::Thunk);
        SKSE::log::info("{} 자리(ID {} +0x{:X}, ID {} 직접 호출)를 가로챔", spec.name, spec.id, spec.offset, kSetEffectivenessId);
        return {};
    }
    std::string InstallExplosionSteps()
    {
        constexpr auto spec = kExplosionStepsCall;
        if (auto problem = CheckCallSite(spec.name, spec.id, spec.offset, kExplosionDamageStepsId); !problem.empty()) {
            return problem;
        }
        ExplosionStepsHook::original = SKSE::GetTrampoline().write_call<5>(REL::ID(spec.id).address() + spec.offset, ExplosionStepsHook::Thunk);
        SKSE::log::info("{} 자리(ID {} +0x{:X}, ID {} 직접 호출)를 가로챔", spec.name, spec.id, spec.offset, kExplosionDamageStepsId);
        return {};
    }
    template <std::size_t I>
    std::string InstallHitSteps()
    {
        constexpr auto spec = kHitStepsCalls[I];
        if (auto problem = CheckCallSite(spec.name, spec.id, spec.offset, kExplosionDamageStepsId); !problem.empty()) {
            return problem;
        }
        HitStepsHook<I>::original = SKSE::GetTrampoline().write_call<5>(REL::ID(spec.id).address() + spec.offset, HitStepsHook<I>::Thunk);
        SKSE::log::info("{} 자리(ID {} +0x{:X}, ID {} 직접 호출)를 가로챔", spec.name, spec.id, spec.offset, kExplosionDamageStepsId);
        return {};
    }
    template <std::size_t I>
    std::string InstallMagicVtable()
    {
        REL::Relocation<std::uintptr_t> table{ kMagicVtables[I].id };
        const auto target = reinterpret_cast<const std::uintptr_t*>(table.address())[kModifyActorValueSlot];
        constexpr auto name = kMagicVtables[I].name;
        if (IsOwnCode(target)) {
            return std::format("{} vtable의 0x{:X} 슬롯은 이미 이 플러그인이 가로챘음. 두 번 걸지 않음", name, kModifyActorValueSlot);
        }
        if (target != REL::ID(kModifyActorValueId).address()) {
            const auto owner = ForeignHookOwner(target);
            if (owner.empty()) {
                return std::format("{} vtable의 0x{:X} 슬롯이 ID {}가 아닌 곳(0x{:X})을 가리키고, 다른 모드의 가로채기로도 보이지 않음",
                    name, kModifyActorValueSlot, kModifyActorValueId, target);
            }
            SKSE::log::warn("{} vtable은 이미 {}가 가로챘음. 그 위에 겹쳐 설치함", name, owner);
        }
        MagicHook<I>::original = table.write_vfunc(kModifyActorValueSlot, MagicHook<I>::Thunk);
        SKSE::log::info("{} vtable의 0x{:X} 슬롯을 가로챔", name, kModifyActorValueSlot);
        return {};
    }
    // 자리마다 따로 시도한다. 하나가 실패해도 나머지는 계속한다.
    template <class Attempt, std::size_t... I>
    void InstallMagicVtables(const Attempt& attempt, std::index_sequence<I...>)
    {
        (attempt([] { return InstallMagicVtable<I>(); }), ...);
    }
    template <class Attempt, std::size_t... I>
    void InstallMagicCalls(const Attempt& attempt, std::index_sequence<I...>)
    {
        (attempt([] { return InstallMagicCall<I>(); }), ...);
    }
    void InstallHooks()
    {
        const unsigned total = (g_settings.hookHits ? static_cast<unsigned>(kHitSiteCount) : 0U) +
                               (g_settings.hookMagic ? static_cast<unsigned>(kMagicSiteCount) : 0U) +
                               (g_settings.logMagnitudeSteps ? 1U : 0U) +
                               (g_settings.logExplosionSteps ? 1U : 0U) +
                               (g_settings.logHitSteps ? static_cast<unsigned>(kHitStepsCalls.size()) : 0U);
        std::string problems;
        unsigned installed = 0;
        const auto record = [&](const std::string& problem) {
            if (problem.empty()) {
                ++installed;
            } else {
                SKSE::log::error("설치하지 못함: {}", problem);
                if (!problems.empty()) {
                    problems += "; ";
                }
                problems += problem;
            }
        };
        if (!REL::Module::IsAE() || REL::Module::get().version() != kVerifiedRuntime) {
            problems = std::format("런타임 {}은 자리를 확인한 판(AE {})이 아니라 설치하지 않음",
                REL::Module::get().version().string("."), kVerifiedRuntime.string("."));
            SKSE::log::error("{}", problems);
        } else {
            // 하나가 실패해도 다른 자리는 독립적으로 계속 설치한다.
            const auto attempt = [&](auto action) {
                try {
                    record(action());
                } catch (const std::exception& error) {
                    record(std::format("설치 중 예외: {}", error.what()));
                } catch (...) {
                    record("설치 중 알 수 없는 예외");
                }
            };
            if (g_settings.hookHits) {
                attempt([] { return Install<Site::kExplosion>(); });
                attempt([] { return Install<Site::kBeam>(); });
                attempt([] { return Install<Site::kNonActorArrow>(); });
                attempt([] { return Install<Site::kTrap>(); });
                attempt([] { return Install<Site::kCollision>(); });
                attempt([] { return Install<Site::kUnknown>(); });
                attempt([] { return Install<Site::kMelee>(); });
            } else {
                SKSE::log::info("일곱 타격 자리는 가로채지 않음(설정 HookHits = false)");
            }
            if (g_settings.hookMagic) {
                InstallMagicVtables(attempt, std::make_index_sequence<kMagicVtableCount>{});
                InstallMagicCalls(attempt, std::make_index_sequence<kMagicCallCount>{});
            } else {
                SKSE::log::info("마법 효과는 가로채지 않음(설정 HookMagic = false)");
            }
            if (g_settings.logMagnitudeSteps) {
                attempt([] { return InstallMagnitudeSteps(); });
            } else {
                SKSE::log::info("마법 크기 계산 자리는 가로채지 않음(설정 LogMagnitudeSteps = false)");
            }
            if (g_settings.logExplosionSteps) {
                attempt([] { return InstallExplosionSteps(); });
            } else {
                SKSE::log::info("폭발 피해 계산 자리는 가로채지 않음(설정 LogExplosionSteps = false)");
            }
            if (g_settings.logHitSteps) {
                attempt([] { return InstallHitSteps<0>(); });
                attempt([] { return InstallHitSteps<1>(); });
            } else {
                SKSE::log::info("근접·화살 피해 계산 자리는 가로채지 않음(설정 LogHitSteps = false)");
            }
        }
        g_installed = installed;
        SKSE::log::info("설치 결과: 성공 {}/{} (설정으로 켠 자리)", installed, total);
        const std::scoped_lock lock(g_installMutex);
        g_installProblem = std::move(problems);
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
            if (!events || g_settings.modeKey == 0 || g_installed == 0) {
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
            ShowNotice(std::format("{} 설치 문제: {}. PoEProtoReceived.log를 보세요", kNoticePrefix, problem), true);
            return;
        }
        ShowNotice(std::format("{} {} 모드. {}", kNoticePrefix, ModeLabel(CurrentMode()), ModeKeyHint()), false);
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
            std::call_once(g_installOnce, InstallHooks);
            if (!g_tickerStarted.exchange(true)) {
                std::thread(TickerMain).detach();
            }
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            if (message->data) {
                g_noticePending = true;
            }
            break;
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
    SKSE::Init(skse, { .log = false, .trampoline = true, .trampolineSize = kTrampolineSize });
    if (!InitializeLogging()) {
        return false;
    }
    g_settings = LoadSettings();
    g_mode = static_cast<int>(g_settings.startMode);
    SKSE::log::info("설정: ModeKey=0x{:02X} StartMode={} Value={:.6f} LogAll={} ReadResistance={} LogMagnitudeSteps={} LogExplosionSteps={} LogHitSteps={} HookMagic={} HookHits={} Notify={}",
        g_settings.modeKey, ModeName(g_settings.startMode), g_settings.value, g_settings.logAll, g_settings.readResistance,
        g_settings.logMagnitudeSteps, g_settings.logExplosionSteps, g_settings.logHitSteps, g_settings.hookMagic, g_settings.hookHits, g_settings.notify);
    SKSE::log::info("런타임 {}", skse->RuntimeVersion().string("."));
    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("SKSE 메시지 리스너를 등록하지 못함. 가로채기와 알림을 설치하지 않습니다.");
        return false;
    }
    SKSE::log::info("PoEProtoReceived 준비 완료. 데이터가 로드되면 설치함");
    return true;
}
