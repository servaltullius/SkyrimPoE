#pragma once

#include "core/ItemData.h"
#include "core/Types.h"

#include <cstdint>
#include <random>
#include <string>

namespace SkyrimPoE::Core
{
    struct MonsterProfile
    {
        MonsterRarity rarity = MonsterRarity::Normal;
        std::uint16_t modMask = 0;  // bit k = MonsterModKind(k)
        double maxEsSkyrim = 0.0;
        double currentEsSkyrim = 0.0;
        double esRechargeCooldown = 0.0;

        [[nodiscard]] bool HasMod(MonsterModKind mod) const noexcept
        {
            const auto b = static_cast<std::uint8_t>(mod);
            return b < kMonsterModCount && ((modMask & static_cast<std::uint16_t>(1u << b)) != 0);
        }
    };

    struct MonsterIncomingResult
    {
        double absorbedByEs = 0.0;
        double remainingHealthDamage = 0.0;
        bool hadAnyImpact = false;  // ES가 100% 흡수해도 true -> KillTracker 기여 보장
    };

    struct MonsterOutgoingModifier
    {
        double damageMultiplier = 1.0;
        double extraFireRatio = 0.0;
        double extraColdRatio = 0.0;
        double extraLightningRatio = 0.0;
        bool isDeadly = false;
        bool isHasted = false;
    };

    // 5단계(결정 164): 매직·레어 몬스터 승격, 12종 속성, 적 ES 흡수/재충전, 헤드헌터 훔치기 순수 엔진
    class MonsterModSystem
    {
    public:
        static constexpr double kEsRechargeDelaySec = 4.0;
        static constexpr double kEsRechargeRatePerSec = 0.15;

        [[nodiscard]] static MonsterProfile RollPromotion(
            bool isBoss,
            bool isDragon,
            double baseSkyrimMaxHealth,
            const Config& config,
            std::mt19937_64& rng);

        // 몬스터 희귀도·속성에 따른 플레이어->몬스터 최종 피해 배율
        [[nodiscard]] static double IncomingDamageMultiplier(
            const MonsterProfile& profile,
            DamageType dominantType) noexcept;

        // 플레이어 타격 시 적 저항·방어 및 적 ES 흡수 적용(카오스는 ES 우회)
        [[nodiscard]] static MonsterIncomingResult ApplyPlayerHitToMonster(
            MonsterProfile& profile,
            double rawSkyrimDamage,
            DamageType dominantType,
            bool bypassesEs = false) noexcept;

        // 몬스터->플레이어 공격 보정(희귀도 + 12종 공격 속성)
        [[nodiscard]] static MonsterOutgoingModifier ComputeOutgoingModifier(
            const MonsterProfile& profile) noexcept;

        // 적 ES 재충전 틱(4초 무피격 후 초당 15%)
        static void TickMonsterEs(MonsterProfile& profile, double deltaSec) noexcept;

        [[nodiscard]] static std::string BuildModSummaryKo(
            const MonsterProfile& profile,
            const ItemData& data);
    };
}
