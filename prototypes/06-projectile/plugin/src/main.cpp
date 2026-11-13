// PoE Proto 6: 투사체를 시험하는 플러그인.
// 설계의 전제(결정 55): 스킬의 피해는 DLL이 직접 주고, 눈에 보이는 투사체는 피해 없는 연출이다. 이 플러그인은 그 구조가
// 날아가는 투사체에서도 되는지를 본다. 묻는 것:
//   1) 주문 없이 쏜 투사체는 스스로 피해를 주지 않는가.
//   2) 그 투사체가 언제, 어디서, 무엇에 맞았는지를 DLL이 알 수 있는가(끼어들기, 상태 살펴보기, 적중 이벤트).
//   3) 맞은 자리에서 다시 쏠 수 있는가: 연쇄(다른 표적으로), 분기(두 발로), 관통(같은 방향으로 이어서).
//   4) 여러 발을 한꺼번에 쏘면 엔진이 받아 주는가, 프레임에 얼마가 드는가.
//   5) 적중마다 DLL의 피해가 한 번만 들어가고, 그 피해로 죽으면 플레이어의 처치인가.
// 시작 키를 누르면 표적(스키버)을 앞에 놓고 정해진 단계를 스스로 돌리며 로그에 남긴다. 끝나면 표적을 치운다.
// 빌드하는 CommonLibSSE-NG는 저장소의 서브모듈(v16.4.0, 결정 60)이다. 이 판으로 빌드한 것을 게임에서 돌리는 첫 프로토타입이다.

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
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

    constexpr auto kConfigPath = "Data/SKSE/Plugins/PoEProtoProjectile.ini";
    constexpr auto kLogName = "PoEProtoProjectile";
    // 시험에 쓰는 폼(Skyrim.esm, houseCARL로 대조).
    constexpr RE::FormID kMissile = 0x00012E84;       // FireboltProjectile01 (종류 Missile, 속도 2500, 사거리 10000)
    constexpr RE::FormID kControlSpell = 0x00012FD0;  // Firebolt (대조용: 주문을 실어 쏘면 엔진이 스스로 피해를 준다)
    constexpr RE::FormID kSkeever = 0x00023AB7;       // EncSkeever (표적)
    constexpr int              kKeptLogs = 3;
    constexpr std::string_view kStepMarker = "=== ";
    constexpr auto             kTick = std::chrono::milliseconds(15);
    constexpr std::size_t      kMaxShots = 400;  // 다시 쏘기가 끝없이 이어지는 것을 막는 한도
    constexpr int              kMaxGeneration = 4;
    constexpr float            kRelaunchLift = 40.0f;  // 연쇄·분기가 다시 쏠 때 맞은 자리보다 올려 잡는 높이

    struct Settings
    {
        std::uint32_t startKey = 0x57;    // 키보드 DirectInput 스캔 코드. 0x57 = F11
        bool          hookImpact = true;  // 미사일 투사체의 적중 함수에 끼어들어 적중을 안다
        bool          pollState = true;   // 끼어들지 않고 투사체의 상태를 주기적으로 살펴 적중을 알 수 있는지도 본다
        float         damage = 1.0f;      // 적중 한 번에 DLL이 주는 피해
    };

    // 표적. A는 정면, B는 A의 바로 뒤(관통용), C와 D는 좌우(연쇄·분기용), E는 대조용(주문을 실은 한 발을 맞음).
    enum Target : std::size_t { kA, kB, kC, kD, kE, kTargetCount };
    constexpr std::array<const char*, kTargetCount> kTargetNames{ "A", "B", "C", "D", "E" };

    enum class Mode { kSingle, kControl, kChain, kFork, kPierce, kProbe, kKill, kVolume };

    constexpr const char* ModeName(Mode mode)
    {
        switch (mode) {
        case Mode::kSingle: return "한 발";
        case Mode::kControl: return "대조(주문 실음)";
        case Mode::kChain: return "연쇄";
        case Mode::kFork: return "분기";
        case Mode::kPierce: return "관통";
        case Mode::kProbe: return "가설 시험";
        case Mode::kKill: return "처치";
        default: return "다량";
        }
    }

    // 우리가 쏜 투사체 하나.
    struct Shot
    {
        int                     id = 0;
        int                     parent = 0;      // 이것을 낳은 투사체. 처음 쏜 것은 0
        int                     generation = 0;  // 다시 쏜 횟수
        Mode                    mode = Mode::kSingle;
        RE::ProjectileHandle    handle;          // 엔진이 돌려준 핸들
        RE::NiPoint3            origin;
        RE::NiPoint3            direction;       // 우리가 겨눈 방향(길이 1)
        std::vector<RE::FormID> alreadyHit;      // 이 줄기가 이미 맞힌 대상
        std::string             test;            // 이 발이 속한 단계의 이름. 늦게 맞은 발이 다음 단계의 결과에 섞이지 않게 한다
        bool                    childShooterIsHit = false;  // 다시 쏠 때 쏘는 사람을 플레이어가 아니라 맞은 대상으로 한다(3차의 관통)
        Clock::time_point       launchedAt;
        bool                    resolved = false;      // 첫 적중을 처리했다
        int                     hookImpacts = 0;       // 끼어들기로 본 적중 수
        bool                    pollSawFirst = false;  // 살펴보기로 처음 봤다
        int                     pollImpacts = 0;       // 살펴보기로 본 적중 기록 수
        std::uint32_t           pollFlags = 0;
        bool                    gone = false;          // 핸들이 비었다(엔진이 치웠다)
    };

    struct TestResult
    {
        std::string              name;
        std::vector<std::string> hits;  // 적중한 차례대로: 표적 이름이나 "다른 것"
        int                      rehits = 0;  // 이미 맞힌 대상에 다시 맞은 수
        int                      damages = 0; // DLL이 피해를 준 수
    };

    Settings          g_settings;
    std::atomic<bool> g_running{ false };
    std::atomic<bool> g_tickQueued{ false };
    std::recursive_timed_mutex g_state;
    using StateLock = std::unique_lock<std::recursive_timed_mutex>;

    std::vector<Shot>                           g_shots;
    int                                         g_nextShotId = 1;
    std::array<RE::ActorHandle, kTargetCount>   g_targets;
    std::array<float, kTargetCount>             g_healthBefore{};  // 단계가 시작될 때의 체력
    std::vector<RE::ProjectileHandle>           g_volume;          // 다량 발사의 핸들
    std::size_t                                 g_volumePeak = 0;
    TestResult                                  g_test;
    std::size_t                                 g_step = 0;
    Clock::time_point                           g_nextAt{};
    bool                                        g_hookInstalled = false;
    std::atomic<int>                            g_foreignImpacts{ 0 };  // 우리 것이 아닌 미사일의 적중(끼어들기가 게임 전체에서 도는지 보는 수)

    // 프레임 간격. 입력 이벤트 수신부가 프레임마다 한 번 불린다는 전제로 잰다(전제가 맞는지는 기준 측정의 표본 수로 본다).
    // 둘째 잣대: 15ms마다 맡기는 작업이 실제로 도는 간격. SKSE는 맡겨진 작업을 프레임마다 한 번 돌리므로, 프레임이 15ms보다 길면
    // 이 간격이 프레임 간격과 같아진다(그보다 짧은 프레임은 이 잣대로 못 잰다). 두 잣대가 같은 값을 내는지로 서로를 확인한다.
    std::mutex            g_frameMutex;
    std::vector<double>   g_taskSamples;
    Clock::time_point     g_taskLast{};
    std::vector<double>   g_frameSamples;
    bool                  g_frameMeasuring = false;
    Clock::time_point     g_frameLast{};

    unsigned long Tid() { return ::GetCurrentThreadId(); }

    bool HasTestRun(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::string   line;
        while (file && std::getline(file, line)) {
            if (line.find(kStepMarker) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    // 로그는 직접 연다(SKSE::Init에 맡기면 같은 이름의 파일을 먼저 비운다). 시험 결과가 든 지난 로그는 세 개까지 남긴다.
    bool InitializeLogging()
    {
        auto directory = SKSE::log::log_directory();
        if (!directory) {
            return false;
        }
        std::error_code error;
        std::filesystem::create_directories(*directory, error);
        const auto path = *directory / std::format("{}.log", kLogName);
        if (HasTestRun(path)) {
            const auto previous = [&](int index) {
                return *directory / (index == 1 ? std::format("{}.previous.log", kLogName) : std::format("{}.previous.{}.log", kLogName, index));
            };
            for (int index = kKeptLogs; index >= 1; --index) {
                std::filesystem::rename(index == 1 ? path : previous(index - 1), previous(index), error);
            }
        }
        try {
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
            auto logger = std::make_shared<spdlog::logger>("poe_proto_projectile", std::move(sink));
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
            if (key == "StartKey") {
                std::uint32_t code = 0;
                const bool    hex = value.starts_with("0x") || value.starts_with("0X");
                const auto    digits = hex ? value.substr(2) : value;
                const auto    result = std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
                if (result.ec == std::errc{} && result.ptr == digits.data() + digits.size() && code >= 1 && code <= 255) {
                    settings.startKey = code;
                } else {
                    SKSE::log::warn("설정: StartKey '{}'는 1~255의 키 코드가 아니어서 기본값 유지", value);
                }
            } else if (key == "HookImpact") {
                ParseBool(key, value, settings.hookImpact);
            } else if (key == "PollState") {
                ParseBool(key, value, settings.pollState);
            } else if (key == "Damage") {
                float       damage = 0.0f;
                const auto  result = std::from_chars(value.data(), value.data() + value.size(), damage);
                if (result.ec == std::errc{} && damage > 0.0f && damage <= 10.0f) {
                    settings.damage = damage;
                } else {
                    SKSE::log::warn("설정: Damage '{}'는 0보다 크고 10 이하인 수가 아니어서 기본값 유지", value);
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

    std::string StartKeyName()
    {
        return g_settings.startKey == 0x57 ? std::string("F11") : std::format("시작 키(스캔 코드 0x{:02X})", g_settings.startKey);
    }

    // ---------------------------------------------------------------- 작은 도구

    std::string Vec(const RE::NiPoint3& p) { return std::format("({:.0f}, {:.0f}, {:.0f})", p.x, p.y, p.z); }

    float Length(const RE::NiPoint3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

    RE::NiPoint3 Normalized(const RE::NiPoint3& v)
    {
        const float length = Length(v);
        return length > 0.001f ? RE::NiPoint3{ v.x / length, v.y / length, v.z / length } : RE::NiPoint3{ 0.0f, 1.0f, 0.0f };
    }

    float Health(RE::Actor* actor) { return actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth); }

    RE::Actor* TargetActor(std::size_t index)
    {
        auto ref = g_targets[index].get();
        return ref ? ref.get() : nullptr;
    }

    // 그 참조가 우리 표적이면 그 번호, 아니면 kTargetCount.
    std::size_t TargetIndexOf(const RE::TESObjectREFR* ref)
    {
        for (std::size_t i = 0; ref && i < kTargetCount; ++i) {
            if (TargetActor(i) == ref) {
                return i;
            }
        }
        return kTargetCount;
    }

    // 표적의 몸통 가운데. 스키버는 키가 작아, 발밑의 좌표를 겨누면 땅에 맞는다.
    RE::NiPoint3 AimPoint(RE::Actor* actor)
    {
        auto       point = actor->GetPosition();
        const auto low = actor->GetBoundMin();
        const auto high = actor->GetBoundMax();
        point.z += std::max(12.0f, (low.z + high.z) * 0.5f);
        return point;
    }

    // 표적의 수평 반지름쯤. 관통이 이어서 쏠 자리를 표적의 몸 밖으로 잡는 데 쓴다.
    float HorizontalRadius(RE::Actor* actor)
    {
        const auto low = actor->GetBoundMin();
        const auto high = actor->GetBoundMax();
        return std::clamp(std::max(high.x - low.x, high.y - low.y) * 0.5f, 20.0f, 120.0f);
    }

    std::string DescribeRef(RE::TESObjectREFR* ref)
    {
        if (!ref) {
            return "참조 없음(지형이나 정적 물체)";
        }
        if (const auto index = TargetIndexOf(ref); index < kTargetCount) {
            return std::format("표적 {}({:08X})", kTargetNames[index], ref->GetFormID());
        }
        const auto* base = ref->GetBaseObject();
        const char* name = base ? base->GetName() : nullptr;
        return std::format("{:08X}({}{})", ref->GetFormID(), name && *name ? name : "이름 없음", ref->As<RE::Actor>() ? ", 액터" : "");
    }

    StateLock LockState(std::chrono::milliseconds wait, const char* what)
    {
        StateLock lock(g_state, wait);
        if (!lock.owns_lock()) {
            SKSE::log::warn("[잠금] {}: {}ms 안에 못 잡음. 잠그지 않고 진행 (스레드 {})", what, wait.count(), Tid());
        }
        return lock;
    }

    void BeginFrames()
    {
        const std::scoped_lock lock(g_frameMutex);
        g_frameSamples.clear();
        g_taskSamples.clear();
        g_frameLast = {};
        g_taskLast = {};
        g_frameMeasuring = true;
    }

    std::string DescribeSamples(std::vector<double>& samples, double& average)
    {
        average = 0.0;
        if (samples.empty()) {
            return "표본 없음";
        }
        double sum = 0.0;
        for (const auto value : samples) {
            sum += value;
        }
        std::sort(samples.begin(), samples.end());
        average = sum / static_cast<double>(samples.size());
        return std::format("표본 {}개, 평균 {:.2f}ms({:.1f} FPS), 가장 느린 1% {:.2f}ms, 가장 긴 것 {:.2f}ms", samples.size(), average, 1000.0 / average,
            samples[std::min(samples.size() - 1, samples.size() * 99 / 100)], samples.back());
    }

    // 잰 프레임 간격을 한 줄로 남기고 평균(ms)을 돌려준다. 표본이 없으면 0.
    double EndFrames(const std::string& label)
    {
        std::vector<double> frames, tasks;
        {
            const std::scoped_lock lock(g_frameMutex);
            g_frameMeasuring = false;
            frames.swap(g_frameSamples);
            tasks.swap(g_taskSamples);
        }
        double frameAverage = 0.0, taskAverage = 0.0;
        SKSE::log::info("[프레임] {}: 입력 수신부의 간격 - {}", label, DescribeSamples(frames, frameAverage));
        SKSE::log::info("[프레임] {}: 작업이 도는 간격 - {}", label, DescribeSamples(tasks, taskAverage));
        return frameAverage > 0.0 ? frameAverage : taskAverage;
    }

    // ---------------------------------------------------------------- 발사

    Shot* FindShot(int id)
    {
        const auto it = std::find_if(g_shots.begin(), g_shots.end(), [&](const Shot& shot) { return shot.id == id; });
        return it == g_shots.end() ? nullptr : &*it;
    }

    // origin에서 direction으로 한 발 쏜다. spell이 있으면 그 주문을 실어 쏜다(대조용). shooter가 없으면 플레이어가 쏜 것으로 한다.
    // 돌려주는 것은 엔진이 준 핸들(실패면 빈 핸들).
    RE::ProjectileHandle LaunchRaw(const RE::NiPoint3& origin, const RE::NiPoint3& direction, RE::SpellItem* spell, RE::Actor* shooter = nullptr)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = RE::TESForm::LookupByID<RE::BGSProjectile>(kMissile);
        if (!player || !base) {
            return {};
        }
        // 스카이림의 각도: 머리 방향(z)은 +Y가 0이고 시계 방향으로 커진다. 고개(x)는 아래가 양수다.
        const RE::Projectile::ProjectileRot angles{ -std::asin(std::clamp(direction.z, -1.0f, 1.0f)), std::atan2(direction.x, direction.y) };
        RE::ProjectileHandle             handle;
        if (spell) {
            RE::Projectile::LaunchData data(player, origin, angles, spell);
            RE::Projectile::Launch(&handle, data);
        } else {
            RE::Projectile::LaunchData data(base, shooter ? shooter : player, origin, angles);
            data.useOrigin = true;  // 쏘는 사람의 손이 아니라 우리가 준 자리에서 나가게 한다
            data.autoAim = false;   // 엔진이 표적 쪽으로 방향을 고치지 않게 한다
            RE::Projectile::Launch(&handle, data);
        }
        return handle;
    }

    struct LaunchSpec
    {
        Mode                    mode = Mode::kSingle;
        RE::NiPoint3            origin;
        RE::NiPoint3            aim;
        std::string             test;  // 이 발이 속한 단계의 이름
        int                     parent = 0;
        int                     generation = 0;
        std::vector<RE::FormID> alreadyHit;
        RE::SpellItem*          spell = nullptr;
        RE::Actor*              shooter = nullptr;  // 없으면 플레이어
        bool                    childShooterIsHit = false;
    };

    int Launch(LaunchSpec spec)
    {
        if (g_shots.size() >= kMaxShots) {
            SKSE::log::warn("[발사] 한도 {}발에 닿아 더 쏘지 않음", kMaxShots);
            return 0;
        }
        Shot shot;
        shot.id = g_nextShotId++;
        shot.parent = spec.parent;
        shot.generation = spec.generation;
        shot.mode = spec.mode;
        shot.origin = spec.origin;
        shot.direction = Normalized(spec.aim - spec.origin);
        shot.alreadyHit = std::move(spec.alreadyHit);
        shot.test = std::move(spec.test);
        shot.childShooterIsHit = spec.childShooterIsHit;
        shot.launchedAt = Clock::now();
        shot.handle = LaunchRaw(spec.origin, shot.direction, spec.spell, spec.shooter);
        SKSE::log::info("[발사] #{} {} (부모 #{}, {}번째 다시 쏨, 쏘는 사람 {}): {} -> {} 방향, 핸들 {:08X}{} (스레드 {})", shot.id, ModeName(spec.mode), spec.parent,
            spec.generation, spec.shooter ? DescribeRef(spec.shooter) : std::string("플레이어"), Vec(spec.origin), Vec(spec.aim), shot.handle.native_handle(),
            shot.handle ? "" : " = 실패(엔진이 핸들을 주지 않음)", Tid());
        const int id = shot.id;
        g_shots.push_back(std::move(shot));
        return id;
    }

    // ---------------------------------------------------------------- 적중의 처리

    // 맞은 뒤의 일(피해, 다시 쏘기)은 적중이 알려진 그 자리에서 하지 않고 다음 작업으로 미룬다. 적중은 엔진이 투사체를 움직이는
    // 도중에 알려지므로, 그 안에서 새 투사체를 만들지 않으려는 것이다.
    // backward는 적중 함수가 받은 셋째 인자다. 헤더의 이름은 a_velocity지만 1차 시험에서 본 값은 날아온 방향의 반대(길이 1)였다
    // (쏜 방향 (0.34, 0.93, -0.11)에 (-0.34, -0.93, 0.11)쯤이 왔다. 적중 기록의 칸 이름도 negativeVelocity다).
    void ResolveImpact(int shotId, RE::ObjectRefHandle refHandle, RE::NiPoint3 location, RE::NiPoint3 backward)
    {
        auto* shot = FindShot(shotId);
        if (!shot || shot->resolved) {
            return;
        }
        shot->resolved = true;
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto  refPtr = refHandle.get();
        auto*       ref = refPtr.get();
        auto*       actor = ref ? ref->As<RE::Actor>() : nullptr;
        const auto  targetIndex = TargetIndexOf(ref);
        const bool  again = ref && std::find(shot->alreadyHit.begin(), shot->alreadyHit.end(), ref->GetFormID()) != shot->alreadyHit.end();
        // 여기서부터 쏘는 호출이 g_shots를 늘려 shot이 가리키던 자리가 옮겨질 수 있으므로, 쓸 값은 미리 떠 둔다.
        const auto  mode = shot->mode;
        const int   generation = shot->generation;
        const bool  childShooterIsHit = shot->childShooterIsHit;
        const auto  testName = shot->test;
        const bool  current = testName == g_test.name;  // 이 발이 지금 단계의 것인가. 늦게 맞은 앞 단계의 발은 결과에 넣지 않는다
        if (!current) {
            SKSE::log::info("[처리] #{}: 앞 단계('{}')의 발이 이제 맞음. 지금 단계의 결과에는 넣지 않음", shotId, testName);
        } else {
            g_test.hits.push_back(targetIndex < kTargetCount ? std::string(kTargetNames[targetIndex]) : ref ? std::string("다른 참조") : std::string("지형"));
        }
        if (again) {
            if (current) {
                ++g_test.rehits;
            }
            SKSE::log::info("[처리] #{}: 이미 맞힌 대상({})에 다시 맞음. 피해를 주지 않고 이 줄기는 여기서 끝남", shot->id, DescribeRef(ref));
            return;
        }
        // 피해: 우리 표적이고 살아 있을 때만. 주문을 실은 대조 발사에는 주지 않는다(엔진이 준 것만 보려는 것).
        if (actor && targetIndex < kTargetCount && !actor->IsDead() && mode != Mode::kControl && mode != Mode::kProbe) {
            const float before = Health(actor);
            const float amount = mode == Mode::kKill ? 100000.0f : g_settings.damage;
            const bool  result = actor->DoDamage(amount, player, true);
            if (current) {
                ++g_test.damages;
            }
            SKSE::log::info("[처리] #{}: 표적 {}에 DLL 피해 {:.1f} (DoDamage 반환 {}). 체력: 단계 시작 {:.2f}, 적중 뒤 피해 전 {:.2f}, 피해 뒤 {:.2f}. "
                            "플레이어에게 적대 {}",
                shot->id, kTargetNames[targetIndex], amount, result, g_healthBefore[targetIndex], before, Health(actor), actor->IsHostileToActor(player));
        } else {
            SKSE::log::info("[처리] #{}: 맞은 것 {}. 피해를 주지 않음({})", shot->id, DescribeRef(ref),
                mode == Mode::kControl ? "대조 발사" : mode == Mode::kProbe ? "가설 시험" : !actor ? "액터가 아님" : targetIndex >= kTargetCount ? "우리 표적이 아님" : "이미 죽음");
        }
        if (!ref || generation >= kMaxGeneration || mode == Mode::kProbe) {
            return;
        }
        auto hitList = shot->alreadyHit;
        hitList.push_back(ref->GetFormID());
        const auto direction = Normalized(RE::NiPoint3{ -backward.x, -backward.y, -backward.z });  // 날아가던 방향
        // 다시 쏠 자리와 방향. 맞은 대상의 몸 안에서 쏘면 그 대상에 곧바로 다시 맞을 수 있으므로 몸 밖으로 나간 자리에서 쏜다.
        const float clearance = (actor ? HorizontalRadius(actor) : 30.0f) * 2.0f + 30.0f;
        if (mode == Mode::kPierce) {
            // 같은 방향(수평)으로 맞은 대상의 뒤쪽에서 잇는다. 2차 시험에서 이렇게 쏜 발은 방금 맞힌 대상에 곧바로 다시 맞았다
            // (가설: 엔진이 쏘는 사람과 발사 원점을 잇는 선을 살핀다). childShooterIsHit이면 쏘는 사람을 맞은 대상으로 바꿔 쏜다.
            const auto level = Normalized(RE::NiPoint3{ direction.x, direction.y, 0.0f });
            const auto origin = location + level * clearance;
            // 겨누는 곳: 뒤의 표적 B가 살아 있으면 그 몸통(땅이 고르지 않아 수평으로만 이으면 B의 위나 아래로 지나갈 수 있다. 수평 방향은 같다).
            auto*      behind = TargetActor(kB);
            const bool aimAtB = behind && !behind->IsDead() && behind != actor && std::find(hitList.begin(), hitList.end(), behind->GetFormID()) == hitList.end();
            Launch({ .mode = Mode::kPierce, .origin = origin, .aim = aimAtB ? AimPoint(behind) : origin + level * 1000.0f, .test = testName, .parent = shotId,
                .generation = generation + 1, .alreadyHit = hitList, .shooter = childShooterIsHit ? actor : nullptr, .childShooterIsHit = childShooterIsHit });
        } else if (mode == Mode::kChain || mode == Mode::kFork) {
            // 아직 맞히지 않은 살아 있는 표적을 가까운 것부터. 연쇄는 하나, 분기는 둘(분기는 한 번만 가른다).
            std::vector<std::pair<float, std::size_t>> candidates;
            for (std::size_t i = 0; i < kTargetCount; ++i) {
                auto* other = TargetActor(i);
                if (i == kB || i == kE || !other || other->IsDead() || std::find(hitList.begin(), hitList.end(), other->GetFormID()) != hitList.end()) {
                    continue;  // B는 A의 그늘에 있고 E는 대조용이라 연쇄·분기의 대상에서 뺀다
                }
                candidates.emplace_back(Length(AimPoint(other) - location), i);
            }
            std::sort(candidates.begin(), candidates.end());
            const std::size_t count = mode == Mode::kFork ? (generation == 0 ? 2 : 0) : 1;
            for (std::size_t n = 0; n < count && n < candidates.size(); ++n) {
                const auto aim = AimPoint(TargetActor(candidates[n].second));
                auto       origin = location + Normalized(aim - location) * clearance;
                // 맞은 높이보다 조금 위에서 내려 쏜다. 1·2차에서 연쇄의 셋째 발은 작은 표적의 몸 높이로 낮게 날다가 솟은 바닥에 걸렸다.
                origin.z += kRelaunchLift;
                Launch({ .mode = mode, .origin = origin, .aim = aim, .test = testName, .parent = shotId, .generation = generation + 1, .alreadyHit = hitList });
            }
        }
    }

    // 끼어들기가 적중을 알려 왔다. 우리 것이면 기록하고 처리를 다음 작업으로 맡긴다. 엔진의 호출 도중이므로 여기서는 오래 붙잡지 않는다.
    void OnHookImpact(RE::Projectile* projectile, RE::TESObjectREFR* ref, const RE::NiPoint3& location, const RE::NiPoint3& velocity)
    {
        const auto handleValue = projectile->GetHandle().native_handle();
        StateLock  lock(g_state, std::chrono::milliseconds(20));
        if (!lock.owns_lock()) {
            SKSE::log::warn("[잠금] 적중 끼어들기: 못 잡음. 이 적중은 기록하지 않음 (스레드 {})", Tid());
            return;
        }
        const auto it = std::find_if(g_shots.begin(), g_shots.end(), [&](const Shot& shot) { return shot.handle && shot.handle.native_handle() == handleValue && !shot.gone; });
        if (it == g_shots.end()) {
            if (std::none_of(g_volume.begin(), g_volume.end(), [&](const RE::ProjectileHandle& handle) { return handle.native_handle() == handleValue; })) {
                ++g_foreignImpacts;
            }
            return;
        }
        ++it->hookImpacts;
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - it->launchedAt).count();
        SKSE::log::info("[적중:끼어들기] #{} {}번째: {} / 자리 {} / 셋째 인자 ({:.2f}, {:.2f}, {:.2f}) / 쏜 방향 ({:.2f}, {:.2f}, {:.2f}) / 쏜 자리에서 {:.0f} / "
                        "쏜 뒤 {:.0f}ms (스레드 {})",
            it->id, it->hookImpacts, DescribeRef(ref), Vec(location), velocity.x, velocity.y, velocity.z, it->direction.x, it->direction.y, it->direction.z,
            Length(location - it->origin), elapsed, Tid());
        if (it->hookImpacts > 1) {
            return;  // 한 투사체의 둘째 적중부터는 기록만 한다
        }
        const int           shotId = it->id;
        const RE::ObjectRefHandle refHandle = ref ? ref->GetHandle() : RE::ObjectRefHandle();
        if (const auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([shotId, refHandle, location, velocity] {
                const auto taskLock = LockState(std::chrono::seconds(2), "적중 처리");
                ResolveImpact(shotId, refHandle, location, velocity);
            });
        }
    }

    // 미사일 투사체가 무엇에 맞았을 때 엔진이 부르는 가상 함수(MissileProjectile의 0xBD번, 헤더의 AddImpact)에 끼어든다.
    struct ImpactHook
    {
        static RE::Projectile::ImpactData* Thunk(RE::Projectile* self, RE::TESObjectREFR* ref, const RE::NiPoint3& location, const RE::NiPoint3& velocity,
            RE::hkpCollidable* collidable, std::int32_t arg6, std::uint32_t arg7)
        {
            auto* result = original(self, ref, location, velocity, collidable, arg6, arg7);
            if (self) {
                OnHookImpact(self, ref, location, velocity);
            }
            return result;
        }

        static void Install()
        {
            REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_MissileProjectile[0] };
            original = vtable.write_vfunc(0xBD, Thunk);
            g_hookInstalled = true;
            SKSE::log::info("미사일 투사체의 적중 함수(가상 함수 0xBD)에 끼어듦");
        }

        static inline REL::Relocation<decltype(Thunk)> original;
    };

    // 끼어들지 않고 아는 길: 우리가 쏜 투사체의 상태를 주기적으로 읽는다. 적중 기록이 생겼는지, 언제 치워지는지를 남긴다.
    void PollShots()
    {
        for (auto& shot : g_shots) {
            if (shot.gone || !shot.handle) {
                continue;
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - shot.launchedAt).count();
            const auto ref = shot.handle.get();
            if (!ref) {
                shot.gone = true;
                SKSE::log::info("[살펴보기] #{}: 핸들이 비었음(엔진이 치움). 쏜 뒤 {:.0f}ms. 끼어들기로 본 적중 {}, 살펴보기로 본 적중 기록 {}", shot.id, elapsed,
                    shot.hookImpacts, shot.pollImpacts);
                continue;
            }
            // (const로 받으면 이 CommonLibSSE-NG의 BSSimpleList가 순회를 컴파일하지 못한다. 읽기만 한다.)
            auto& data = ref->GetProjectileRuntimeData();
            if (!shot.pollSawFirst) {
                shot.pollSawFirst = true;
                const float speed = Length(data.linearVelocity);
                const auto  velocity = Normalized(data.linearVelocity);
                const float dot = std::clamp(velocity.x * shot.direction.x + velocity.y * shot.direction.y + velocity.z * shot.direction.z, -1.0f, 1.0f);
                // 쏜 바로 그 작업 안에서 보면 엔진이 아직 속도를 넣지 않았다(1차 시험: 속도 0, 표시에 준비됨이 없음). 그때는 각을 적지 않는다.
                SKSE::log::info("[살펴보기] #{} 처음 봄({:.0f}ms): 자리 {}, 속도 {} (빠르기 {:.0f}), 겨눈 방향과의 각 {}, 주문 {}, 폭발 {}, 표시 {:08X}", shot.id, elapsed,
                    Vec(ref->GetPosition()), Vec(data.linearVelocity), speed, speed > 1.0f ? std::format("{:.1f}도", std::acos(dot) * 57.29578f) : "아직 움직이지 않음",
                    data.spell ? "있음" : "없음", data.explosion ? "있음" : "없음", data.flags.underlying());
            }
            int impacts = 0;
            RE::NiPointer<RE::TESObjectREFR> firstCollidee;  // 로그를 남길 때까지 그 참조를 쥐고 있는다
            for (auto* impact : data.impacts) {
                if (impact && impacts++ == 0) {
                    firstCollidee = impact->collidee.get();
                }
                if (impacts > 16) {
                    break;
                }
            }
            const auto flags = data.flags.underlying();
            if (impacts != shot.pollImpacts) {
                SKSE::log::info("[적중:살펴보기] #{}: 적중 기록 {}개(전에는 {}개). 첫 기록의 대상 {}. 쏜 뒤 {:.0f}ms, 그때까지 끼어들기로 본 적중 {}", shot.id, impacts,
                    shot.pollImpacts, DescribeRef(firstCollidee.get()), elapsed, shot.hookImpacts);
                shot.pollImpacts = impacts;
            }
            if (flags != shot.pollFlags) {
                if (shot.pollFlags != 0) {
                    SKSE::log::info("[살펴보기] #{}: 표시 {:08X} -> {:08X} (치워짐 {}, 적중 처리됨 {}). 쏜 뒤 {:.0f}ms, 날아간 거리 {:.0f}", shot.id, shot.pollFlags, flags,
                        data.flags.any(RE::Projectile::Flags::kDestroyed), data.flags.any(RE::Projectile::Flags::kProcessedImpacts), elapsed, data.distanceMoved);
                }
                shot.pollFlags = flags;
            }
        }
        if (!g_volume.empty()) {
            std::size_t alive = 0;
            for (const auto& handle : g_volume) {
                if (handle.get()) {
                    ++alive;
                }
            }
            g_volumePeak = std::max(g_volumePeak, alive);
        }
    }

    // 적중을 알려 주는 엔진의 이벤트가 주문 없는 투사체에도 오는지를 본다.
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
            if (event && g_running) {
                const StateLock lock(g_state, std::chrono::milliseconds(20));
                if (TargetIndexOf(event->target.get()) < kTargetCount) {
                    SKSE::log::info("[이벤트] 적중: 대상 {}, 가해자 {}, 수단 {:08X}, 투사체 폼 {:08X}, 표시 {:02X} (스레드 {})", DescribeRef(event->target.get()),
                        DescribeRef(event->cause.get()), event->source, event->projectile, static_cast<unsigned>(event->flags.underlying()), Tid());
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // ---------------------------------------------------------------- 단계

    struct Step
    {
        const char*           name;
        std::function<void()> run;
        int                   waitMs;
    };

    RE::NiPoint3 Forward(const RE::Actor* actor)
    {
        const float heading = actor->data.angle.z;
        return { std::sin(heading), std::cos(heading), 0.0f };
    }

    // 플레이어의 가슴 높이에서 조금 앞. 첫 발은 여기서 나간다.
    RE::NiPoint3 Muzzle()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto  point = player->GetPosition() + Forward(player) * 70.0f;
        point.z += 100.0f;
        return point;
    }

    void LogTargets(const char* label)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (std::size_t i = 0; i < kTargetCount; ++i) {
            auto* actor = TargetActor(i);
            if (!actor) {
                SKSE::log::info("[표적:{}] {}: 없음", label, kTargetNames[i]);
                continue;
            }
            SKSE::log::info("[표적:{}] {}({:08X}): 자리 {}, 플레이어에게서 {:.0f}, 체력 {:.2f}, 죽음 {}, 3D {}, AI {}, 플레이어에게 적대 {}", label, kTargetNames[i],
                actor->GetFormID(), Vec(actor->GetPosition()), Length(actor->GetPosition() - player->GetPosition()), Health(actor), actor->IsDead(),
                actor->Is3DLoaded() ? "있음" : "없음", actor->IsAIEnabled() ? "켜짐" : "꺼짐", actor->IsHostileToActor(player));
        }
    }

    void BeginTest(const char* name)
    {
        g_test = TestResult{ name, {}, 0, 0 };
        for (std::size_t i = 0; i < kTargetCount; ++i) {
            auto* actor = TargetActor(i);
            g_healthBefore[i] = actor ? Health(actor) : 0.0f;
        }
    }

    // 앞 단계의 결과를 한 줄로 남긴다. 판정은 README의 통과 기준에 대어 사람이 읽는다. 여기서는 사실만 적는다.
    void ReportTest()
    {
        if (g_test.name.empty()) {
            return;
        }
        std::string hits;
        for (const auto& hit : g_test.hits) {
            hits += hits.empty() ? "" : " -> ";
            hits += hit;
        }
        std::string health;
        for (std::size_t i = 0; i < kTargetCount; ++i) {
            if (auto* actor = TargetActor(i)) {
                health += std::format("{}{} {:.2f}->{:.2f}{}", health.empty() ? "" : ", ", kTargetNames[i], g_healthBefore[i], Health(actor), actor->IsDead() ? "(죽음)" : "");
            }
        }
        SKSE::log::info("[결과] {}: 처리한 적중 [{}], 이미 맞힌 대상에 다시 맞음 {}번, DLL 피해 {}번 / 체력 {}", g_test.name, hits.empty() ? "없음" : hits, g_test.rehits,
            g_test.damages, health);
        g_test = {};
    }

    void StepSpawn()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(kSkeever);
        const auto origin = player->GetPosition();
        const auto forward = Forward(player);
        const RE::NiPoint3 right{ forward.y, -forward.x, 0.0f };
        // 앞으로, 오른쪽으로(음수는 왼쪽).
        constexpr std::array<std::pair<float, float>, kTargetCount> kOffsets{ { { 700.0f, 0.0f }, { 1100.0f, 0.0f }, { 900.0f, 380.0f }, { 900.0f, -380.0f }, { 450.0f, -330.0f } } };
        for (std::size_t i = 0; i < kTargetCount; ++i) {
            const auto ref = player->PlaceObjectAtMe(base, false);
            auto*      actor = ref ? ref->As<RE::Actor>() : nullptr;
            if (!actor) {
                SKSE::log::error("[표적] {}를 놓지 못함", kTargetNames[i]);
                continue;
            }
            // 표적이 움직이면 같은 조건으로 다시 잴 수 없으므로 AI를 끄고 자리에 세워 둔다.
            actor->EnableAI(false);
            // 액터의 가상 함수(주소 번호가 아니라 가상 함수 표를 거침). 둘째 인자는 캐릭터 컨트롤러도 함께 옮기라는 뜻이다.
            actor->SetPosition(origin + forward * kOffsets[i].first + right * kOffsets[i].second, true);
            g_targets[i] = actor->GetHandle();
        }
        LogTargets("놓은 직후");
    }

    // 표적이 놓은 높이에 그대로 살아 있는가. 1차 시험은 건물의 단 위에서 돌아, 가장자리 밖에 놓인 표적 둘이 떨어졌다(하나는 죽음).
    // 그러면 대조 발사가 돌지 못하고, 떨어진 표적으로 가는 연쇄와 분기가 바닥에 맞는다. 그런 자리에서는 재지 않고 멈춘다.
    bool TargetsStandLevel()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        bool  level = true;
        for (std::size_t i = 0; i < kTargetCount; ++i) {
            auto* actor = TargetActor(i);
            const float drop = actor ? actor->GetPosition().z - player->GetPosition().z : 0.0f;
            // 멈추는 것은 표적이 없거나, 죽었거나, 150 넘게 낮거나 높을 때뿐이다. 3차의 첫 시도에서 이 선을 60으로 두었더니
            // 완만한 비탈(980 떨어진 표적이 81 낮음, 5도쯤)에서 멈췄다. 2차도 44까지 차이가 난 자리에서 돌았다. 떨어진 표적을
            // 가려내려던 것이므로(1차: 196, 611) 선을 올리고, 그 아래의 기울기는 알리기만 한다.
            if (!actor || actor->IsDead() || std::abs(drop) > 150.0f) {
                level = false;
                SKSE::log::warn("[표적] {}: {}", kTargetNames[i], !actor ? "없음" : actor->IsDead() ? "죽어 있음(떨어졌을 수 있음)" : std::format("플레이어의 발보다 {:+.0f}", drop));
            } else if (std::abs(drop) > 60.0f) {
                SKSE::log::info("[표적] {}: 플레이어의 발보다 {:+.0f}. 기울어진 땅이라 결과를 읽을 때 참고할 것(멈추지는 않음)", kTargetNames[i], drop);
            }
        }
        return level;
    }

    void StepBaseline()
    {
        LogTargets("쏘기 전");
        if (!TargetsStandLevel()) {
            g_running = false;
            SKSE::log::error("표적이 제자리에 서 있지 않아 시험을 멈춤. 땅의 높이 차이가 크거나 표적이 떨어진 자리임");
            Notify("[PoE Proto 6] 시험을 멈췄습니다: 땅의 높이 차이가 너무 큽니다.");
            Notify("[PoE Proto 6] 세이브를 불러온 뒤 더 평평한 곳에서 다시 눌러 주세요.");
            return;
        }
        auto* base = RE::TESForm::LookupByID<RE::BGSProjectile>(kMissile);
        if (base) {
            SKSE::log::info("[환경] 투사체 {:08X}: 빠르기 {:.0f}, 사거리 {:.0f}, 충돌 반지름 {:.0f}, 중력 {:.2f}, 폭발 {}", kMissile, base->data.speed, base->data.range,
                base->data.collisionRadius, base->data.gravity, base->data.explosionType ? std::format("{:08X}", base->data.explosionType->GetFormID()) : "없음");
        }
        if (auto* manager = RE::Projectile::Manager::GetSingleton()) {
            SKSE::log::info("[환경] 엔진의 투사체 목록: 제한 없음 {}, 제한 있음 {}, 대기 {}", manager->unlimited.size(), manager->limited.size(), manager->pending.size());
        }
        BeginFrames();
    }

    void FireAt(Mode mode, std::size_t target, const char* name, RE::SpellItem* spell = nullptr, bool childShooterIsHit = false)
    {
        ReportTest();
        BeginTest(name);
        auto* actor = TargetActor(target);
        if (!actor || actor->IsDead()) {
            SKSE::log::warn("[발사] 표적 {}가 없거나 죽어 있어 이 단계를 건너뜀", kTargetNames[target]);
            return;
        }
        Launch({ .mode = mode, .origin = Muzzle(), .aim = AimPoint(actor), .test = name, .spell = spell, .childShooterIsHit = childShooterIsHit });
    }

    // 가설 시험: 플레이어가 보기에 표적 A의 바로 뒤(A의 가운데에서 beyond만큼)에서, A도 다른 표적도 향하지 않는 쪽(오른쪽 위)으로 한 발 쏜다.
    // 가설(엔진이 쏘는 사람과 발사 원점을 잇는 선을 살펴 그 위의 것을 맞은 것으로 친다)이 맞으면, 쏘는 사람이 플레이어일 때는
    // 멀어지는 쪽으로 쐈는데도 A에 맞았다는 알림이 곧바로 오고, 쏘는 사람이 A일 때는 오지 않아야 한다.
    void FireProbe(const char* name, float beyond, bool shooterIsTarget)
    {
        ReportTest();
        BeginTest(name);
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* actor = TargetActor(kA);
        if (!actor || actor->IsDead()) {
            SKSE::log::warn("[발사] 표적 A가 없거나 죽어 있어 이 단계를 건너뜀");
            return;
        }
        const auto         center = AimPoint(actor);
        const auto         away = Normalized(RE::NiPoint3{ center.x - player->GetPosition().x, center.y - player->GetPosition().y, 0.0f });
        const RE::NiPoint3 right{ away.y, -away.x, 0.0f };
        const auto         origin = center + away * beyond;
        // 오른쪽으로, 위로 30도. 오른쪽에 있는 표적 C의 위로 지나가게 한다.
        const RE::NiPoint3 direction{ right.x * 0.866f, right.y * 0.866f, 0.5f };
        SKSE::log::info("[가설 시험] 원점은 A의 가운데에서 {:.0f} 뒤(A의 수평 반지름 {:.0f}), 플레이어에서 {:.0f}. 쏘는 방향은 A에서 멀어지는 오른쪽 위", beyond,
            HorizontalRadius(actor), Length(origin - player->GetPosition()));
        Launch({ .mode = Mode::kProbe, .origin = origin, .aim = origin + direction * 1000.0f, .test = name, .shooter = shooterIsTarget ? actor : nullptr });
    }

    double g_baselineMs = 0.0;

    void StepVolume(int count)
    {
        ReportTest();
        auto* player = RE::PlayerCharacter::GetSingleton();
        g_volume.clear();
        g_volumePeak = 0;
        const float heading = player->data.angle.z;
        const auto  origin = Muzzle();
        int         launched = 0;
        const auto  started = Clock::now();
        for (int i = 0; i < count; ++i) {
            // 앞쪽 하늘로 부채꼴: 좌우 40도, 위로 15~35도. 표적에 맞지 않게 위로 쏜다.
            const float spread = (count == 1 ? 0.0f : (static_cast<float>(i) / static_cast<float>(count - 1) - 0.5f)) * 1.4f;
            const float pitch = 0.26f + 0.35f * static_cast<float>(i % 4) / 3.0f;
            const RE::NiPoint3 direction{ std::sin(heading + spread) * std::cos(pitch), std::cos(heading + spread) * std::cos(pitch), std::sin(pitch) };
            if (const auto handle = LaunchRaw(origin, direction, nullptr); handle) {
                g_volume.push_back(handle);
                ++launched;
            }
        }
        const auto spent = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        SKSE::log::info("[다량] {}발을 요청해 {}발이 핸들을 받음. 쏘는 데 {:.2f}ms (스레드 {})", count, launched, spent, Tid());
        if (auto* manager = RE::Projectile::Manager::GetSingleton()) {
            SKSE::log::info("[다량] 쏜 직후 엔진의 투사체 목록: 제한 없음 {}, 제한 있음 {}, 대기 {}", manager->unlimited.size(), manager->limited.size(), manager->pending.size());
        }
        BeginFrames();
    }

    void EndVolume(int count)
    {
        const double average = EndFrames(std::format("{}발이 나는 동안", count));
        std::size_t  alive = 0;
        for (const auto& handle : g_volume) {
            if (handle.get()) {
                ++alive;
            }
        }
        SKSE::log::info("[다량] {}발: 동시에 살아 있던 최대 {}발, 지금 {}발. 프레임 간격이 기준보다 {:+.2f}ms", count, g_volumePeak, alive,
            average > 0.0 && g_baselineMs > 0.0 ? average - g_baselineMs : 0.0);
        g_volume.clear();
    }

    void StepCleanup()
    {
        ReportTest();
        LogTargets("끝");
        auto* player = RE::PlayerCharacter::GetSingleton();
        for (std::size_t i = 0; i < kTargetCount; ++i) {
            if (auto* actor = TargetActor(i)) {
                if (i == kD) {
                    const auto killer = actor->GetActorRuntimeData().myKiller.get();
                    SKSE::log::info("[결과] 처치: 표적 D 죽음 {}, 죽인 쪽 {}", actor->IsDead(), !killer ? "기록 없음" : killer.get() == player ? "플레이어" : DescribeRef(killer.get()));
                }
                // 표적은 치우지 않고 둔다. 1차 시험에서 여기서 Disable()과 SetDelete(true)를 잇달아 부른 직후 게임이 튕겼다:
                // 크래시 로그에서 엔진의 이동 처리(MovementControllerNPC)가 삭제 표시가 붙고 폼 ID가 0이 된 스키버를 다루다
                // 빈 포인터를 읽었다. 둘 가운데 어느 호출이 방아쇠였는지는 가르지 않았고, 이 프로토타입이 묻는 것도 아니다.
                // 시험 뒤에는 어차피 세이브를 불러오므로 그대로 둔다. 참조를 안전하게 없애는 방법은 따로 확인할 일이다.
            }
            g_targets[i] = RE::ActorHandle();
        }
        SKSE::log::info("[환경] 시험 동안 우리 것이 아닌 미사일의 적중을 끼어들기가 본 수: {}", g_foreignImpacts.load());
        int unresolved = 0;
        for (const auto& shot : g_shots) {
            if (!shot.resolved) {
                ++unresolved;
                SKSE::log::info("[결과] #{} {}: 적중이 처리되지 않음(끼어들기 {}, 살펴보기 기록 {}, 치워짐 {})", shot.id, ModeName(shot.mode), shot.hookImpacts,
                    shot.pollImpacts, shot.gone);
            }
        }
        SKSE::log::info("[결과] 쏜 투사체 {}발 가운데 적중이 처리되지 않은 것 {}발", g_shots.size(), unresolved);
    }

    const std::vector<Step>& Steps()
    {
        static const std::vector<Step> steps{
            { "표적 놓기", StepSpawn, 3000 },
            { "쏘기 전의 상태와 기준 프레임", StepBaseline, 2500 },
            { "한 발(주문 없음) -> A", [] { g_baselineMs = EndFrames("기준(아무것도 쏘지 않음)"); FireAt(Mode::kSingle, kA, "한 발(주문 없음)"); }, 2500 },
            { "대조: 주문을 실은 한 발 -> E", [] { FireAt(Mode::kControl, kE, "대조(주문 실음)", RE::TESForm::LookupByID<RE::SpellItem>(kControlSpell)); }, 2500 },
            { "연쇄: A에서 다른 표적으로", [] { FireAt(Mode::kChain, kA, "연쇄"); }, 4000 },
            { "분기: A에서 두 발로", [] { FireAt(Mode::kFork, kA, "분기"); }, 4000 },
            { "관통(쏘는 사람 플레이어): A를 지나 B로", [] { FireAt(Mode::kPierce, kA, "관통(쏘는 사람 플레이어)"); }, 3000 },
            { "관통(쏘는 사람 맞은 대상): A를 지나 B로", [] { FireAt(Mode::kPierce, kA, "관통(쏘는 사람 맞은 대상)", nullptr, true); }, 3000 },
            { "가설 시험 1: A의 바로 뒤에서 옆으로, 쏘는 사람 플레이어", [] { FireProbe("가설 시험 1(바로 뒤, 플레이어)", 93.0f, false); }, 2500 },
            { "가설 시험 2: A에서 떨어진 뒤에서 옆으로, 쏘는 사람 플레이어", [] { FireProbe("가설 시험 2(떨어진 뒤, 플레이어)", 200.0f, false); }, 2500 },
            { "가설 시험 3: 같은 자리에서 옆으로, 쏘는 사람 A", [] { FireProbe("가설 시험 3(떨어진 뒤, 쏘는 사람 A)", 200.0f, true); }, 2500 },
            { "다량 32발", [] { StepVolume(32); }, 5000 },
            { "처치: D에 치명 피해", [] {
                 EndVolume(32);
                 // 죽는 과정은 액터의 처리가 돌아야 끝날 수 있으므로(프로토타입 5의 10차: 피해 직후가 아니라 조금 뒤에 죽은 상태가 됨)
                 // 이 표적만 AI를 다시 켠다.
                 if (auto* actor = TargetActor(kD)) {
                     actor->EnableAI(true);
                 }
                 FireAt(Mode::kKill, kD, "처치");
             }, 3500 },
            { "정리", StepCleanup, 500 },
        };
        return steps;
    }

    void Tick()
    {
        if (g_settings.pollState) {
            PollShots();
        }
        if (!g_running || Clock::now() < g_nextAt) {
            return;
        }
        const auto& steps = Steps();
        if (g_step >= steps.size()) {
            g_running = false;
            SKSE::log::info("=== 끝 (스레드 {})", Tid());
            Notify("[PoE Proto 6] 끝났습니다. 플레이하던 세이브를 불러오세요(지금 상태로 저장하지 마세요).");
            return;
        }
        const auto& step = steps[g_step++];
        SKSE::log::info("=== 단계 {}/{}: {} (스레드 {})", g_step, steps.size(), step.name, Tid());
        g_nextAt = Clock::now() + std::chrono::milliseconds(step.waitMs);
        step.run();
    }

    // 시험이 도는 동안 일정한 간격으로 작업 하나를 맡기는 스레드. 작업이 스스로 다음 작업을 맡기는 방식은 쓰지 않는다
    // (프로토타입 5의 2차 시험: 그렇게 하면 한 프레임 안에서 다 돌아 버린다).
    void TickerMain()
    {
        for (;;) {
            std::this_thread::sleep_for(kTick);
            if (!g_running || g_tickQueued.exchange(true)) {
                continue;
            }
            const auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                g_tickQueued = false;
                continue;
            }
            tasks->AddTask([] {
                g_tickQueued = false;
                {
                    const std::scoped_lock frameLock(g_frameMutex);
                    if (g_frameMeasuring) {
                        const auto now = Clock::now();
                        if (g_taskLast != Clock::time_point{}) {
                            g_taskSamples.push_back(std::chrono::duration<double, std::milli>(now - g_taskLast).count());
                        }
                        g_taskLast = now;
                    }
                }
                const StateLock lock(g_state, std::chrono::milliseconds(200));
                if (lock.owns_lock()) {
                    Tick();
                }
            });
        }
    }

    void StartScenario()
    {
        if (g_running) {
            Notify("[PoE Proto 6] 이미 시험이 도는 중");
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* ui = RE::UI::GetSingleton();
        if (!player || !player->Is3DLoaded() || !ui || ui->GameIsPaused()) {
            Notify("[PoE Proto 6] 메뉴를 닫고 게임 안에서 눌러 주세요");
            return;
        }
        if (!g_shots.empty() || std::any_of(g_targets.begin(), g_targets.end(), [](const RE::ActorHandle& handle) { return static_cast<bool>(handle); })) {
            // 한 번 돌린(또는 표적을 놓고 멈춘) 세이브에는 표적과 맞은 흔적이 남아 있다. 같은 조건으로 다시 재려면 세이브를 불러온 뒤에 돌린다.
            Notify("[PoE Proto 6] 이미 한 번 돌렸습니다. 플레이하던 세이브를 불러온 뒤 다시 누르세요");
            return;
        }
        if (!RE::TESForm::LookupByID<RE::BGSProjectile>(kMissile) || !RE::TESForm::LookupByID<RE::SpellItem>(kControlSpell) ||
            !RE::TESForm::LookupByID<RE::TESBoundObject>(kSkeever)) {
            SKSE::log::error("시험에 쓸 폼을 찾지 못함");
            Notify("[PoE Proto 6] 시험에 쓸 폼을 찾지 못함(로그 확인)");
            return;
        }
        g_step = 0;
        g_nextAt = Clock::now();
        g_foreignImpacts = 0;
        Notify("[PoE Proto 6] 투사체 시험을 시작합니다. 약 50초, 조작하지 말고 두세요.");
        SKSE::log::info("[환경] 적중 끼어들기 {} / 상태 살펴보기 {} / 적중당 피해 {:.1f} / 시작한 스레드 {}", g_hookInstalled ? "켜짐" : "꺼짐",
            g_settings.pollState ? "켜짐" : "꺼짐", g_settings.damage, Tid());
        g_running = true;
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
            {
                const std::scoped_lock lock(g_frameMutex);
                if (g_frameMeasuring) {
                    const auto now = Clock::now();
                    if (g_frameLast != Clock::time_point{}) {
                        g_frameSamples.push_back(std::chrono::duration<double, std::milli>(now - g_frameLast).count());
                    }
                    g_frameLast = now;
                }
            }
            if (!events) {
                return RE::BSEventNotifyControl::kContinue;
            }
            for (auto* event = *events; event; event = event->next) {
                const auto* button = event->AsButtonEvent();
                if (button && button->GetDevice() == RE::INPUT_DEVICE::kKeyboard && button->GetIDCode() == g_settings.startKey && button->IsDown()) {
                    if (const auto* tasks = SKSE::GetTaskInterface()) {
                        tasks->AddTask([] {
                            const auto lock = LockState(std::chrono::seconds(2), "시험 시작");
                            StartScenario();
                        });
                    }
                }
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    // 다른 세이브가 불려 오면 우리가 놓은 표적과 쏜 투사체는 그 세이브의 것이 아니다. 상태를 비워 다시 돌릴 수 있게 한다.
    void ResetForLoad()
    {
        const auto lock = LockState(std::chrono::seconds(2), "불러오기 뒤의 초기화");
        if (g_running) {
            SKSE::log::warn("시험 도중에 세이브가 불려 와 시험을 멈춤({}단계까지 돎)", g_step);
        }
        g_running = false;
        g_shots.clear();
        g_volume.clear();
        g_targets = {};
        g_test = {};
        g_nextShotId = 1;
        {
            const std::scoped_lock frameLock(g_frameMutex);
            g_frameMeasuring = false;
            g_frameSamples.clear();
            g_taskSamples.clear();
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
                SKSE::log::error("입력 장치 관리자를 찾지 못함. 키로 시작할 수 없습니다.");
            }
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            if (g_settings.hookImpact) {
                ImpactHook::Install();
            } else {
                SKSE::log::info("적중 함수에 끼어들지 않음(설정 HookImpact = false). 맞은 뒤의 처리(피해, 다시 쏘기)는 돌지 않고 관찰만 남습니다.");
            }
            if (auto* events = RE::ScriptEventSourceHolder::GetSingleton()) {
                events->AddEventSink<RE::TESHitEvent>(&HitSink::Get());
            }
            std::thread(TickerMain).detach();
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
        case SKSE::MessagingInterface::kNewGame:
            ResetForLoad();
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
    SKSE::log::info("설정: StartKey=0x{:02X} HookImpact={} PollState={} Damage={:.1f}", g_settings.startKey, g_settings.hookImpact, g_settings.pollState,
        g_settings.damage);
    const auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        SKSE::log::error("SKSE 메시지 리스너를 등록하지 못함. 시험을 시작할 수 없습니다.");
        return true;
    }
    SKSE::log::info("PoEProtoProjectile 준비 완료");
    return true;
}
