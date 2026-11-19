#pragma once

#include <cstdint>
#include <string_view>

namespace SkyrimPoE::Core
{
    enum class CyclonePhase : std::uint8_t
    {
        Idle = 0,
        PendingStart = 1,  // NotifyAnimationGraph에서 승인, 첫 주 갱신에서 비용 차감 대기(결정 128)
        Channeling = 2,    // 회전·비용 차감·범위 타격 중
        Outro = 3          // 버튼 뗌 + 최소 시간 경과 후 마무리 회전(무비용·무타격, 결정 150 ②)
    };

    struct CycloneStaggerDecision
    {
        bool swallowEvent = false;
        bool abortedChannel = false;
    };

    struct CycloneTickInput
    {
        double deltaSec = 0.0;
        bool gamePaused = false;
        bool buttonHeld = false;
        bool playerAllowed = true;
        double weaponSpeed = 1.0;          // 스카이림 무기 레코드 기본 속도(WeaponSpeedMult 제외, 결정 98)
        double poeAttackSpeedMult = 3.0;   // 사이클론 기본 3.0 × PoE 공속 배율
        double currentMagicka = 0.0;
        double cycleMagickaCost = 0.0;
        double minChannelSec = 0.6;
        double outroSec = 0.15;
    };

    struct CycloneTickOutput
    {
        bool startOar = false;
        bool stopOar = false;
        bool consumeMagicka = false;
        double magickaToConsume = 0.0;
        bool performAreaHit = false;
        double spinRadiansDelta = 0.0;
        bool ended = false;
        std::string_view endReason;
    };

    // 결정 49, 50, 54, 56, 97~99, 122, 128, 150을 구현하는 순수 사이클론 상태기계.
    class CycloneStateMachine
    {
    public:
        static constexpr double kMinHitsPerSec = 0.5;
        static constexpr double kMaxHitsPerSec = 20.0;
        static constexpr double kTwoPi = 6.28318530717958647692;

        // 강공격 시작 이벤트에서 호출(결정 97, 122, 128): 읽기만 수행, 부족하면 false(원래 강공격 진행).
        [[nodiscard]] bool TryBeginFromPowerAttack(
            bool hasCycloneEquipped,
            bool yieldingToFrenzy,
            bool playerAllowed,
            double currentMagicka,
            double firstCycleCost);

        // staggerStart 이벤트에서 호출(결정 56, 150 ③): 한도 이하면 삼키고, 초과면 즉시 종료 후 통과.
        [[nodiscard]] CycloneStaggerDecision OnStaggerStart(double magnitude, double ignoreThreshold);

        // 메뉴 진입·사망·로드 등 금지 상태 즉시 종료(결정 150 ④).
        bool ForceStop();

        // 주 갱신 스레드에서 프레임당 1회 호출.
        [[nodiscard]] CycloneTickOutput Tick(const CycloneTickInput& in);

        [[nodiscard]] CyclonePhase Phase() const noexcept { return phase_; }
        [[nodiscard]] bool IsActive() const noexcept { return phase_ != CyclonePhase::Idle; }
        [[nodiscard]] double ElapsedSec() const noexcept { return elapsedSec_; }

        // 이벤트 이름 판별 헬퍼(프로토타입 2 EventNames.cpp 검증 로직)
        [[nodiscard]] static bool IsAttackStartEvent(std::string_view eventName);
        [[nodiscard]] static bool IsPowerAttackStartEvent(std::string_view eventName);
        [[nodiscard]] static bool IsStaggerStartEvent(std::string_view eventName);

    private:
        [[nodiscard]] static double ComputeCyclePeriod(double weaponSpeed, double poeAttackSpeedMult) noexcept;

        CyclonePhase phase_ = CyclonePhase::Idle;
        double elapsedSec_ = 0.0;
        double cycleElapsedSec_ = 0.0;
        bool hitFiredInCycle_ = false;
        double outroRemainingSec_ = 0.0;
    };
}
