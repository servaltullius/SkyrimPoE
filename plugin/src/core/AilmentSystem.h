#pragma once

#include "core/Types.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace SkyrimPoE::Core
{
    struct PoisonStack
    {
        double dps = 0.0;
        double remaining = 0.0;
    };

    struct ActorAilmentState
    {
        double igniteDps = 0.0;
        double igniteRemaining = 0.0;

        double chillRatio = 0.0;      // 0.05..0.30
        double chillRemaining = 0.0;

        double freezeRemaining = 0.0; // 0.3..1.5s (보스·드래곤은 최대 냉각 30%로 완화)

        double shockRatio = 0.0;      // 0.05..0.50 (+5%..+50% 받는 피해 증폭)
        double shockRemaining = 0.0;

        double bleedDps = 0.0;
        double bleedRemaining = 0.0;

        std::vector<PoisonStack> poisonStacks;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return igniteRemaining <= 0.0 &&
                   chillRemaining <= 0.0 &&
                   freezeRemaining <= 0.0 &&
                   shockRemaining <= 0.0 &&
                   bleedRemaining <= 0.0 &&
                   poisonStacks.empty();
        }
    };

    struct HitAilmentInput
    {
        double physHitPoe = 0.0;
        double fireHitPoe = 0.0;
        double coldHitPoe = 0.0;
        double lightningHitPoe = 0.0;
        double chaosHitPoe = 0.0;

        double targetAilmentThresholdPoe = 100.0;
        bool isCrit = false;
        bool isBossOrDragon = false;
        bool starforgeShock = false;         // Starforge: 물리 피해로 감전 부여 가능
        bool targetElementalImmune = false;  // Purity of Elements 등 원소 상태이상 면역
        bool targetFreezeImmune = false;     // Wanderlust / 정화의 참
        bool targetBleedImmune = false;      // 정화의 참

        double igniteChancePct = 0.0;
        double freezeChancePct = 0.0;
        double shockChancePct = 0.0;
        double bleedChancePct = 0.0;
        double poisonChancePct = 0.0;
        double roll01 = 0.5;
    };

    struct AilmentApplyResult
    {
        bool appliedIgnite = false;
        bool appliedChill = false;
        bool appliedFreeze = false;
        bool appliedShock = false;
        bool appliedBleed = false;
        bool appliedPoison = false;
    };

    struct AilmentTickDamage
    {
        double firePoeDamage = 0.0;
        double physPoeDamage = 0.0;
        double chaosPoeDamage = 0.0;

        [[nodiscard]] double TotalPoe() const noexcept { return firePoeDamage + physPoeDamage + chaosPoeDamage; }
    };

    // 5단계(결정 163): 핵심 상태이상 5종(점화·냉각·빙결·감전·출혈/중독) 순수 계산 엔진
    class AilmentSystem
    {
    public:
        static constexpr std::size_t kMaxPoisonStacks = 20;
        static constexpr double kIgniteDurationSec = 4.0;
        static constexpr double kChillDurationSec = 2.0;
        static constexpr double kShockDurationSec = 2.0;
        static constexpr double kBleedDurationSec = 5.0;
        static constexpr double kPoisonDurationSec = 2.0;

        AilmentApplyResult ApplyHitAilments(std::uint32_t targetActorId, const HitAilmentInput& in);

        // Berek's Respite 등 처치 시 점화·감전 전파
        void ProliferateIgniteAndShock(
            std::uint32_t targetActorId,
            double igniteDps,
            double igniteRemaining,
            double shockRatio,
            double shockRemaining,
            bool targetElementalImmune);

        // 단일 액터 시간 진행 및 DoT 피해 산출(만료 시 자동 정리)
        AilmentTickDamage TickActor(std::uint32_t actorId, double deltaSec, bool isMoving);

        void ClearAilment(std::uint32_t actorId, AilmentKind kind);
        void RemoveActor(std::uint32_t actorId);
        void ClearAll();

        [[nodiscard]] double ShockDamageMultiplier(std::uint32_t actorId) const noexcept;
        [[nodiscard]] double ChillSlowRatio(std::uint32_t actorId) const noexcept;
        [[nodiscard]] bool IsFrozen(std::uint32_t actorId) const noexcept;
        [[nodiscard]] const ActorAilmentState* GetState(std::uint32_t actorId) const noexcept;
        [[nodiscard]] std::vector<std::uint32_t> ActiveActorIds() const;

    private:
        std::unordered_map<std::uint32_t, ActorAilmentState> states_;
    };
}
