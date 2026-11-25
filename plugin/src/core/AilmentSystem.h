#pragma once

#include "core/Types.h"
#include "core/PassiveStatParser.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace SkyrimPoE::Core
{
    struct TargetAilmentHitDamage;
    struct AilmentInstance
    {
        double magnitude = 0.0;  // DPS for damaging ailments, effect ratio otherwise
        double remaining = 0.0;
        bool aggravated = false;
        bool chaosIgnite = false;
    };

    class Flammability
    {
    public:
        void AddHit(double fireDamage, double ailmentThreshold, double duration, double baseChancePct = 0.0,
            double increasedPct = 0.0);
        void Tick(double deltaSec);
        [[nodiscard]] double ChancePercent() const noexcept;
        [[nodiscard]] bool IsEmpty() const noexcept { return stacks_.empty(); }

    private:
        struct Stack
        {
            double chancePercent;
            double remaining;
        };
        std::vector<Stack> stacks_;
    };

    [[nodiscard]] double ThresholdAilmentChance(double damage, double threshold, double coefficient,
        double base = 0.0, double increased = 0.0, double more = 1.0) noexcept;
    [[nodiscard]] double RawChillPercent(double coldDamage, double ailmentThreshold) noexcept;
    [[nodiscard]] DamagePacket RollSpellDamagePacket(const DamagePacket& packet, double roll01, double multiplier) noexcept;
    [[nodiscard]] bool ResolveTargetAilmentExtra(DamagePacket& packet, bool frozen, bool shocked, bool chilled) noexcept;
    [[nodiscard]] bool HasElementalHitDamage(const DamagePacket& packet) noexcept;

    enum class PoiseKind : std::uint8_t { Freeze, Electrocute, HeavyStun, Pin, Count };

    struct PoiseBuildup
    {
        double ratio = 0.0;
        double decayDelayRemaining = 0.0;
        double activeRemaining = 0.0;
        double repeatModifierRemaining = 0.0;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return ratio <= 0.0 && activeRemaining <= 0.0 && repeatModifierRemaining <= 0.0;
        }
    };

    struct ActorAilmentState
    {
        double igniteDps = 0.0;
        double igniteRemaining = 0.0;
        bool igniteDealsChaos = false;

        double chillRatio = 0.0;
        double chillRemaining = 0.0;


        double shockRatio = 0.0;
        double shockRemaining = 0.0;

        double bleedDps = 0.0;
        double bleedRemaining = 0.0;
        bool bleedAggravated = false;

        std::size_t activePoisonCount = 0;
        Flammability flammability;
        std::array<PoiseBuildup, static_cast<std::size_t>(PoiseKind::Count)> poise{};
    };

    bool ResolveTargetAilmentHit(DamagePacket& packet, const TargetAilmentHitDamage& modifiers,
        const ActorAilmentState* state, DamagePacket& ailmentDamage) noexcept;

    struct HitAilmentInput
    {
        double physHitPoe = 0.0;
        double fireHitPoe = 0.0;
        double coldHitPoe = 0.0;
        double coldDamageAfterResistanceMultiplier = 1.0;
        double lightningHitPoe = 0.0;
        double chaosHitPoe = 0.0;

        double targetAilmentThresholdPoe = 100.0;
        double targetPoiseThresholdPoe = 100.0;
        // The damage already reflects the hit. A critical strike does not guarantee any ailment.
        bool isCrit = false;
        bool isBossOrDragon = false;
        bool isMapBoss = false;
        MonsterRarity targetRarity = MonsterRarity::Normal;
        bool starforgeShock = false;         // Starforge: 물리 피해로 감전 부여 가능
        bool targetElementalImmune = false;  // Purity of Elements 등 원소 상태이상 면역
        bool targetFreezeImmune = false;     // Wanderlust / 정화의 참
        bool targetBleedImmune = false;      // 정화의 참

        bool aggravatesBleed = false;
        bool chaosContributesToIgnite = false;
        bool igniteDealsChaos = false;
        double flammabilityIncPct = 0.0;
        double freezeBuildupIncPct = 0.0;
        double ailmentChanceIncPct = 0.0;
        double ailmentMagnitudeIncPct = 0.0;
        double bleedBaseDuration = 0.0;
        double bleedMagnitudeMult = 1.0;
        std::array<PassiveAilmentStats, static_cast<std::size_t>(AilmentKind::Count)> modifiers{};

        double igniteChancePct = 0.0;
        double shockChancePct = 0.0;
        double bleedChancePct = 0.0;
        double poisonChancePct = 0.0;
        double igniteRoll01 = 0.5;
        double shockRoll01 = 0.5;
        double bleedRoll01 = 0.5;
        double poisonRoll01 = 0.5;

        // P4 effects supply the damage that contributes to these kinds of buildup.
        double electrocuteHitPoe = 0.0;
        double heavyStunHitPoe = 0.0;
        double pinHitPoe = 0.0;
    };

    struct AilmentApplyResult
    {
        bool appliedIgnite = false;
        bool appliedChill = false;
        bool appliedFreeze = false;
        bool appliedShock = false;
        bool appliedBleed = false;
        bool appliedPoison = false;
        bool appliedElectrocute = false;
        bool appliedHeavyStun = false;
        bool appliedPin = false;
    };

    struct AilmentTickDamage
    {
        double firePoeDamage = 0.0;
        double physPoeDamage = 0.0;
        double chaosPoeDamage = 0.0;
        double chaosIgnitePoeDamage = 0.0;

        [[nodiscard]] double TotalPoe() const noexcept
        {
            return firePoeDamage + physPoeDamage + chaosPoeDamage + chaosIgnitePoeDamage;
        }
    };

    class AilmentSystem
    {
    public:

        AilmentApplyResult ApplyHitAilments(std::uint32_t targetActorId, const HitAilmentInput& in,
            double elapsedSinceTick = 0.0);
        AilmentApplyResult ApplyHitAilmentsAt(std::uint32_t targetActorId, const HitAilmentInput& in,
            double now, bool isMoving);

        // Berek's Respite 등 처치 시 점화·감전 전파
        void ProliferateIgniteAndShock(
            std::uint32_t targetActorId,
            double igniteDps,
            double igniteRemaining,
            double shockRatio,
            double shockRemaining,
            bool targetElementalImmune, double now = 0.0, bool isMoving = false, bool chaosIgnite = false);

        // 단일 액터 시간 진행 및 DoT 피해 산출(만료 시 자동 정리)
        AilmentTickDamage TickActor(std::uint32_t actorId, double deltaSec, bool isMoving);
        AilmentTickDamage TickActorAt(std::uint32_t actorId, double now, bool isMoving);
        void AdvanceActorTo(std::uint32_t actorId, double now, bool isMoving);

        void ClearAilment(std::uint32_t actorId, AilmentKind kind);
        void RemoveActor(std::uint32_t actorId);
        void ClearAll();

        [[nodiscard]] double ShockDamageMultiplier(std::uint32_t actorId) const noexcept;
        [[nodiscard]] double ChillSlowRatio(std::uint32_t actorId) const noexcept;
        [[nodiscard]] bool IsChilledOrFrozen(std::uint32_t actorId) const noexcept;
        [[nodiscard]] double FlammabilityChancePercent(std::uint32_t actorId) const noexcept;
        [[nodiscard]] bool IsFrozen(std::uint32_t actorId) const noexcept;
        [[nodiscard]] const ActorAilmentState* GetState(std::uint32_t actorId) const noexcept;
        [[nodiscard]] std::vector<std::uint32_t> ActiveActorIds() const;

    private:
        struct TrackedActor : ActorAilmentState
        {
            std::array<std::vector<AilmentInstance>, static_cast<std::size_t>(AilmentKind::Count)> instances;
            AilmentTickDamage pendingDamage;
            double lastTickSec = 0.0;
            double advancedThroughSec = 0.0;

            void AdvanceTo(double now, bool isMoving);
            void RefreshActive();
            [[nodiscard]] bool IsEmpty() const noexcept;
        };

        std::unordered_map<std::uint32_t, TrackedActor> states_;
    };
}
