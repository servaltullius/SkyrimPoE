#pragma once

#include "core/DataPack.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>

namespace SkyrimPoE::Core
{
    struct MagickaScalingResult
    {
        double skyrimMaxMagicka = 100.0;
        double skyrimSkillCost = 0.0;
        bool isZeroManaBuild = false;
    };

    struct EnergyShieldAbsorbResult
    {
        double newEnergyShield = 0.0;
        double absorbedEffectiveDamage = 0.0;
        double remainingRawDamage = 0.0;  // 엔진(난이도 배율 적용 전)에 넘길 잔여 피해
    };

    // 결정 62, 64, 92, 93, 142, 143: 주는 피해(타격당) 환산
    [[nodiscard]] double ExpectedHitDamage(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    [[nodiscard]] double ConvertOffenseHit(
        double poeHitDamage,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    [[nodiscard]] double ConvertOffenseDps(
        double poeDps,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    // 기대 방어·저항·생명력·마나(결정 69, 70, 74, 84, 144)
    [[nodiscard]] double ArmourPassRatio(double armour, double poeHit, double cap = 0.90) noexcept;
    [[nodiscard]] double EvasionHitChance(double evasion, double accuracy, double minHit = 0.05) noexcept;

    [[nodiscard]] double ExpectedPhysicalPass(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    [[nodiscard]] double ExpectedElementalPass(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    [[nodiscard]] double ExpectedChaosPass(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    [[nodiscard]] double ExpectedLife(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    [[nodiscard]] double ExpectedMana(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    // 결정 68, 71, 94, 142: 받는 피해 환산 (난이도 배율 적용 전 스카이림 단위 반환)
    [[nodiscard]] double ConvertDefensePhysicalHit(
        double skyrimPreArmorD,
        std::uint32_t exileLevel,
        const StatSnapshot& snapshot,
        const DataPack& pack,
        const Config& config) noexcept;

    [[nodiscard]] double ConvertDefenseResistDamage(
        double skyrimPreResistD,
        DamageType damageType,
        std::uint32_t exileLevel,
        const StatSnapshot& snapshot,
        const DataPack& pack,
        const Config& config) noexcept;

    // 결정 67, 70, 125, 144: 생명력·에너지 실드·매지카 풀 및 비용 환산
    [[nodiscard]] double ComputeSkyrimMaxHealth(
        double baseSkyrimMaxHealth,
        double poeMaxLife,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    [[nodiscard]] double ComputeSkyrimMaxEnergyShield(
        double baseSkyrimMaxHealth,
        double poeMaxEnergyShield,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    [[nodiscard]] MagickaScalingResult ComputeMagickaScaling(
        double baseSkyrimMaxMagicka,
        double poeMaxMana,
        double poeSkillCost,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    // 검수 C2 해법: 난이도 배율(fDiffMultHPToPC*)을 보정해 ES 풀(체력 단위)에서 선차감하고,
    // 잔여분은 난이도 배율 적용 전 값으로 역산해 엔진에 넘긴다.
    [[nodiscard]] EnergyShieldAbsorbResult AbsorbWithEnergyShield(
        double rawPostDefenseDamage,
        double difficultyToPcMult,
        bool isChaos,
        double currentEnergyShield) noexcept;
}
