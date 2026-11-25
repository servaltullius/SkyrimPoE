#pragma once

#include "core/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace SkyrimPoE::Core
{
    inline constexpr std::size_t kRulesLevelCount = 100;

    // Units are given in the field names. For a level L, use index L - 1.
    struct RuleValues
    {
        double lowLifeFraction{};

        struct Armour
        {
            double damageCoefficient{};
            double physicalReductionCapPercent{};
            double roundingOffset{};
            double zeroOverZeroPercent{};
            double brokenEnemyHitDamageTakenPercent{};
            double brokenEnemyDurationSeconds{};
            double negativeArmourSign{};
            double brokenPlayerDurationSeconds{};
        } armour;

        struct EnergyShield
        {
            double rechargeDelaySeconds{};
            double rechargePercentPerMinute{};
            double chaosConsumptionMultiplier{};
            double normalConsumptionMultiplier{};
            bool bleedBypasses{};
            bool poisonBypasses{};
        } energyShield;

        struct Hit
        {
            double playerAccuracyMultiplier{};
            double playerEvasionMultiplier{};
            double monsterEvasionMultiplier{};
            double monsterAccuracyMultiplier{};
            double maxPercent{};
            double minPercent{};
            double negativeAccuracyPercent{};
            double maxEvadePercent{};
            double baseEvasion{};
            double accuracyPerLevel{};
            double accuracyPerDexterity{};
        } hit;

        struct Block
        {
            double baseMaxPercent{};
            double capPercent{};
        } block;

        struct Deflect
        {
            double capPercent{};
            double baseReductionPercent{};
            double minimumRating{};
            double ratingCoefficient{};
            double chanceScale{};
            double chanceOffsetPercent{};
            double percentScale{};
            double minPercent{};
        } deflect;

        struct Resistance
        {
            double baseMaxPercent{};
            double monsterBaseMaxPercent{};
            double capPercent{};
        } resistance;

        struct Ignite
        {
            double damagePercentPerMinute{};
            double durationSeconds{};
            double chanceMultiplier{};
            double flammabilityEnemyDurationSeconds{};
            double flammabilityPlayerDurationSeconds{};
            double nonHitFlammabilityMultiplier{};
            std::uint32_t nonHitIgniteThresholdPermyriad{};
            bool flammabilityIncludesCurrentHit{};
            bool flammabilityIndependentExpiry{};
            double flammabilityCapPercent{};
        } ignite;

        struct Bleed
        {
            double damagePercentPerMinute{};
            double durationSeconds{};
            double movingOrAggravatedMultiplier{};
        } bleed;

        struct Poison
        {
            double damagePercentPerMinute{};
            double durationSeconds{};
            std::uint32_t baseMaxStacks{};
        } poison;

        struct Shock
        {
            double effectPercent{};
            double chanceMultiplier{};
            double enemyDurationSeconds{};
            double playerDurationSeconds{};
        } shock;

        struct Chill
        {
            double effectMultiplier{};
            double maxPercent{};
            double enemyDurationSeconds{};
            double playerDurationSeconds{};
            double defaultPercent{};
            double minPercent{};
            std::uint32_t precision{};
            bool discardBelowMinimum{};
            double magicSlowReductionPercent{};
            double rareSlowReductionPercent{};
            double uniqueSlowReductionPercent{};

            [[nodiscard]] double SlowEffectMultiplier(MonsterRarity rarity, bool unique) const noexcept;
        } chill;

        struct Freeze
        {
            double damageScale{};
            double playerDamageScale{};
            double enemyDurationSeconds{};
            double playerDurationSeconds{};
        } freeze;

        struct Electrocute
        {
            double damageScale{};
            double enemyDurationMilliseconds{};
            double playerDurationMilliseconds{};
            double repeatThresholdModifierPercent{};
            double repeatModifierDurationSeconds{};
        } electrocute;

        struct HeavyStun
        {
            double damageScale{};
            double playerDamageScale{};
            double repeatThresholdModifierPercent{};
            double repeatModifierDurationSeconds{};
            double playerRepeatThresholdModifierPercent{};
            double playerRepeatModifierDurationSeconds{};
            double playerDurationMilliseconds{};
            double enemyDurationMilliseconds{};
        } heavyStun;

        struct Pin
        {
            double damageScale{};
            double enemyDurationSeconds{};
            double playerDurationSeconds{};
            double repeatThresholdModifierPercent{};
            double repeatModifierDurationSeconds{};
        } pin;

        struct Poise
        {
            double playerDecayDelayMilliseconds{};
            double playerDecayPercentPerSecond{};
            double enemyDecayDelayMilliseconds{};
            double enemyDecayPercentPerSecond{};
            double magicThresholdMorePercent{};
            double rareThresholdMorePercent{};
            double uniqueThresholdMorePercent{};
            double mapBossThresholdMorePercent{};
            double fullBuildupPercent{};

            [[nodiscard]] double ThresholdMultiplier(MonsterRarity rarity, bool unique, bool mapBoss) const noexcept;
        } poise;

        struct AilmentThreshold
        {
            double playerLifeFactor{};
        } ailmentThreshold;

        struct AttributePool
        {
            double perLevel{};
            double base{};
            double perAttribute{};

            [[nodiscard]] double At(unsigned int level, unsigned int attribute = 0) const noexcept
            {
                return base + perLevel * level + perAttribute * attribute;
            }

            [[nodiscard]] double Total(unsigned int level, unsigned int attribute, double flat,
                double increasedPercent, double moreMultiplier = 1.0) const noexcept;
        } life, mana;

        struct Critical
        {
            double playerBonusPercent{};
            double monsterBonusPercent{};
            double playerMinionAdditionalBonusPercent{};
        } critical;

        struct LevelTables
        {
            std::array<double, kRulesLevelCount> life{};
            std::array<double, kRulesLevelCount> damage{};
            std::array<double, kRulesLevelCount> accuracy{};
            std::array<double, kRulesLevelCount> evasion{};
            std::array<double, kRulesLevelCount> ailmentThreshold{};
            std::array<double, kRulesLevelCount> poiseThreshold{};
            std::array<double, kRulesLevelCount> mapLevelLifeMult{};

            [[nodiscard]] static double At(const std::array<double, kRulesLevelCount>& table,
                std::uint32_t level) noexcept;
        } levelTables;

        struct ExperienceReference
        {
            std::uint32_t maxPenaltyFreeAreaLevel{};
            double effectiveAreaLevelPenaltyMultiplier{};
            std::uint32_t safeZoneBase{};
            std::uint32_t safeZoneLevelDivisor{};
            std::uint32_t numeratorLevelOffset{};
            std::uint32_t denominatorLevelOffset{};
            double differenceExponent{};
            double penaltyExponent{};
            std::uint32_t highLevelPenaltyStartsAt{};
            std::uint32_t safeDifferenceMax{};
            double safeMultiplier{};
            double displayMinimumMultiplierExclusive{};
            double highLevelPenaltyPerLevel{};
            std::uint32_t highLevelPenaltyOffset{};
            std::array<double, 5> highLevelMultipliers{};
        } experienceReference;

        struct ProjectSpirit
        {
            std::uint32_t base{};
            std::array<std::uint32_t, 4> achievementRewards{};
            std::uint32_t decision{};
        } projectSpirit;

        struct ProjectExperience
        {
            double higherEnemyBonusPerLevel{};
            double higherEnemyMaxMultiplier{};
            std::uint32_t decision{};
        } projectExperience;
    };

    class RulesPack
    {
    public:
        [[nodiscard]] bool LoadFromJsonString(std::string_view text, std::string* error = nullptr);
        [[nodiscard]] const RuleValues* Values() const noexcept { return values_ ? &*values_ : nullptr; }

    private:
        std::optional<RuleValues> values_;
    };

    // Available after all startup content checks have passed.
    [[nodiscard]] const RulesPack* GetRulesPack() noexcept;
    [[nodiscard]] const RuleValues& ActiveRuleValues() noexcept;
}
