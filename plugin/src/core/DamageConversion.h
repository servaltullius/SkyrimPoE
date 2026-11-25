#pragma once

#include "core/DataPack.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace SkyrimPoE::Core
{
    enum class VanillaHitSource { Attack, MagicEffect };
    [[nodiscard]] bool CanInflictPoEAilments(VanillaHitSource source) noexcept;

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

    struct MindOverMatterAbsorbResult
    {
        double absorbedMagicka = 0.0;     // 주 갱신 스레드에서 kMagicka에서 차감할 양
        double remainingRawDamage = 0.0;  // 엔진(난이도 배율 적용 전)에 넘길 잔여 피해
    };

    struct ArmourBreakState
    {
        void ApplyFullyBroken(double nowSeconds, bool targetIsPlayer) noexcept;
        [[nodiscard]] bool IsFullyBroken(double nowSeconds) const noexcept;
        [[nodiscard]] double DamageTakenMultiplier(double nowSeconds, DamageType type, bool isDamageOverTime) const noexcept;

    private:
        double expiresAt_ = 0.0;
        bool targetIsPlayer_ = false;
    };

    class EnemyArmourBreaks
    {
    public:
        void ApplyFullyBroken(std::uint32_t actorHandle, std::uint64_t generation);
        void Advance(double elapsedSeconds, std::uint64_t generation);
        [[nodiscard]] double DamageTakenMultiplier(std::uint32_t actorHandle, std::uint64_t generation,
            DamageType type, bool isDamageOverTime) const;
        [[nodiscard]] double ApplyToDamage(std::uint32_t actorHandle, std::uint64_t generation,
            double damage, double physicalFraction, bool isDamageOverTime) const;

    private:
        bool SetGenerationLocked(std::uint64_t generation);
        mutable std::mutex mutex_;
        std::uint64_t generation_ = 0;
        double nowSeconds_ = 0.0;
        std::unordered_map<std::uint32_t, ArmourBreakState> targets_;
    };

    [[nodiscard]] EnemyArmourBreaks& ActiveEnemyArmourBreaks() noexcept;
    [[nodiscard]] double PhysicalDamageFraction(const DamagePacket& packet) noexcept;
    [[nodiscard]] bool PlayerHitMeetsAilmentThreshold(double postDeflectionDamage, double maxHealth) noexcept;
    [[nodiscard]] bool PlayerHitMeetsAilmentThreshold(double damage, double maxHealth, const StatSnapshot& snapshot,
        DamageType type, double skyrimMaximumEnergyShield) noexcept;

    // 결정 62, 64, 92, 93, 142, 143: 주는 피해(타격당) 환산
    [[nodiscard]] double ExpectedHitDamage(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept;

    // 결정 173: 주는 피해 환산과 지도 액터 체력 맞춤이 공유하는 기준 체력 H(L).
    [[nodiscard]] double OffenseReferenceLife(
        std::uint32_t exileLevel, const DataPack& pack, const Config& config) noexcept;

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
    [[nodiscard]] double ArmourReductionPercent(double armour, double poeHit) noexcept;
    [[nodiscard]] double ArmourPassRatio(double armour, double poeHit, double extraReductionPercent = 0.0,
        double reductionCap = 100.0) noexcept;
    [[nodiscard]] double EvasionHitChance(double evasion, double accuracy) noexcept;
    [[nodiscard]] double DeflectionChance(double rating, double accuracy) noexcept;
    [[nodiscard]] double ApplyRollBias(double chance, bool lucky, bool unlucky) noexcept;
    [[nodiscard]] double DeflectionReduction(const StatSnapshot& snapshot, bool critical = false) noexcept;
    void ConvertGlobalHitDamage(DamagePacket& damage, const PassiveStatTotals& passives, bool isFireSpell) noexcept;
    void RestrictToFire(DamagePacket& damage) noexcept;
    void ScaleDamagePacket(DamagePacket& damage, const DamageTypeValues& multipliers) noexcept;
    void ConvertPassiveHitDamage(DamagePacket& damage, const PassiveStatTotals& passives, bool isFireSpell,
        const ExtraDamageStats& extraDamage, const DamageTypeValues& increasedPct, const DamageTypeValues& moreMultipliers,
        bool isAttack, TargetAilmentHitDamage* targetHit = nullptr) noexcept;
    [[nodiscard]] bool HasRandomElementExtra(const DamagePacket& damage) noexcept;
    [[nodiscard]] DamagePacket ResolveRandomElementExtra(const DamagePacket& damage, std::size_t element) noexcept;

    struct RegenerationSplit
    {
        double life = 0.0;
        double energyShield = 0.0;
    };
    [[nodiscard]] RegenerationSplit SplitLifeRegeneration(double recovery, double missingLife,
        bool excessToEnergyShield) noexcept;

    struct PlayerLifeConditions
    {
        bool lowLife = false;
        bool fullLife = false;
    };

    [[nodiscard]] PlayerLifeConditions GetPlayerLifeConditions(double currentLife, double permanentMaxLife,
        double currentMaxLife, const StatSnapshot& snapshot) noexcept;
    [[nodiscard]] RegenerationSplit ConvertLifeRegeneration(double recoveryPoe, double currentLife,
        double currentMaxLife, double skyrimMaximumEnergyShield, const StatSnapshot& snapshot) noexcept;
    [[nodiscard]] double BlockedHitDamageFraction(const StatSnapshot& snapshot) noexcept;
    [[nodiscard]] double PlayerDamageBypassingEnergyShield(double damage, const StatSnapshot& snapshot) noexcept;
    [[nodiscard]] double BlockChanceLimit(double addedMaxPercent) noexcept;
    [[nodiscard]] double SpellHitBlockChance(const StatSnapshot& snapshot, bool isProjectile) noexcept;
    [[nodiscard]] AilmentKind EnergyShieldBypassAilment(AilmentKind ailment, bool shavronneChaosEs) noexcept;

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
        std::uint32_t startingStrength,
        const Config& config) noexcept;

    [[nodiscard]] double ExpectedMana(
        std::uint32_t exileLevel,
        std::uint32_t startingIntelligence,
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
        const Config& config,
        bool isHit = true) noexcept;

    // 결정 67, 70, 125, 144: 생명력·에너지 실드·매지카 풀 및 비용 환산
    [[nodiscard]] double ComputeSkyrimMaxHealth(
        double baseSkyrimMaxHealth,
        double poeMaxLife,
        double expectedPoeLife,
        bool chaosInoculation = false) noexcept;

    [[nodiscard]] double ComputeBloodMagicLifeCost(
        double skyrimMaxHealth,
        double poeMaxLife,
        double poeSkillCost) noexcept;

    [[nodiscard]] double ComputeSkyrimMaxEnergyShield(
        double baseSkyrimMaxHealth,
        double poeMaxEnergyShield,
        double expectedPoeLife) noexcept;

    [[nodiscard]] MagickaScalingResult ComputeMagickaScaling(
        double baseSkyrimMaxMagicka,
        double poeMaxMana,
        double poeSkillCost,
        double expectedPoeMana) noexcept;

    // 검수 C2 해법: 난이도 배율(fDiffMultHPToPC*)을 보정해 ES 풀(체력 단위)에서 선차감하고,
    // 잔여분은 난이도 배율 적용 전 값으로 역산해 엔진에 넘긴다.
    [[nodiscard]] EnergyShieldAbsorbResult AbsorbWithEnergyShield(
        double rawPostDefenseDamage,
        double difficultyToPcMult,
        bool isChaos,
        double currentEnergyShield,
        AilmentKind ailment = AilmentKind::Count) noexcept;

    // 3단계 Mind Over Matter: ES 이후 잔여 피해에 대해 난이도 배율을 보정해 매지카에서 선차감한다.
    [[nodiscard]] MindOverMatterAbsorbResult AbsorbWithMindOverMatter(
        double rawDamageAfterEs,
        double difficultyToPcMult,
        double momRatio,
        double availableMagicka) noexcept;
}
