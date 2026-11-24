#include "core/RulesPack.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace SkyrimPoE::Core
{
    double RuleValues::AttributePool::Total(unsigned int level, unsigned int attribute, double flat,
        double increasedPercent, double moreMultiplier) const noexcept
    {
        // CalcDefence.lua:86–95 rounds the complete pool after increased and more modifiers.
        const double total = (At(level, attribute) + flat) * (1.0 + increasedPercent / 100.0) * moreMultiplier;
        return std::max(1.0, std::floor(total + 0.5));
    }

    double RuleValues::Chill::SlowEffectMultiplier(MonsterRarity rarity, bool unique) const noexcept
    {
        if (unique) return 1.0 - uniqueSlowReductionPercent / 100.0;
        if (rarity == MonsterRarity::Rare) return 1.0 - rareSlowReductionPercent / 100.0;
        if (rarity == MonsterRarity::Magic) return 1.0 - magicSlowReductionPercent / 100.0;
        return 1.0;
    }

    double RuleValues::Poise::ThresholdMultiplier(MonsterRarity rarity, bool unique, bool mapBoss) const noexcept
    {
        // The rarity modifier occupies one slot. PoB adds a separate MORE modifier for map bosses.
        if (unique || mapBoss) {
            const double uniqueMultiplier = 1.0 + uniqueThresholdMorePercent / 100.0;
            return mapBoss ? uniqueMultiplier * (1.0 + mapBossThresholdMorePercent / 100.0) : uniqueMultiplier;
        }
        if (rarity == MonsterRarity::Rare) return 1.0 + rareThresholdMorePercent / 100.0;
        if (rarity == MonsterRarity::Magic) return 1.0 + magicThresholdMorePercent / 100.0;
        return 1.0;
    }

    double RuleValues::LevelTables::At(const std::array<double, kRulesLevelCount>& table,
        std::uint32_t level) noexcept
    {
        return table[std::clamp<std::size_t>(level, 1, table.size()) - 1];
    }

    const RuleValues& ActiveRuleValues() noexcept
    {
        if (const auto* pack = GetRulesPack()) {
            if (const auto* values = pack->Values()) {
                return *values;
            }
        }
        std::terminate();
    }

    namespace
    {
        using nlohmann::json;
        constexpr double kPositive = std::numeric_limits<double>::min();
        constexpr double kMaxRuleNumber = std::numeric_limits<float>::max();

        double CheckedNumber(const json& value, std::string_view key, double min, double max)
        {
            if (!value.is_number()) {
                throw std::runtime_error(std::format("{} must be a number", key));
            }
            const auto number = value.get<double>();
            if (!std::isfinite(number) || number < min || number > max) {
                throw std::runtime_error(std::format("{} is outside the allowed range", key));
            }
            return number;
        }

        double Number(const json& object, std::string_view key, double min, double max = kMaxRuleNumber)
        {
            return CheckedNumber(object.at(key), key, min, max);
        }

        bool Boolean(const json& object, std::string_view key)
        {
            return object.at(key).get<bool>();
        }

        constexpr auto kMaxU32 = std::numeric_limits<std::uint32_t>::max();

        std::uint32_t CheckedInteger(const json& value, std::string_view key, std::uint32_t min, std::uint32_t max)
        {
            if (!value.is_number_integer()) {
                throw std::runtime_error(std::format("{} must be an integer", key));
            }
            return static_cast<std::uint32_t>(CheckedNumber(value, key, min, max));
        }

        std::uint32_t Integer(const json& object, std::string_view key, std::uint32_t min = 0, std::uint32_t max = kMaxU32)
        {
            return CheckedInteger(object.at(key), key, min, max);
        }

        template <std::size_t N, class T = double>
        std::array<T, N> Numbers(const json& object, std::string_view key,
            T min, T max = kMaxRuleNumber)
        {
            const auto& array = object.at(key);
            if (!array.is_array() || array.size() != N) {
                throw std::runtime_error(std::format("{} must contain exactly {} entries", key, N));
            }
            std::array<T, N> result{};
            for (std::size_t index = 0; index < N; ++index) {
                const auto name = std::format("{}[{}]", key, index);
                if constexpr (std::is_same_v<T, std::uint32_t>) {
                    result[index] = CheckedInteger(array.at(index), name, min, max);
                } else {
                    result[index] = CheckedNumber(array.at(index), name, min, max);
                }
            }
            return result;
        }

        RuleValues ReadRules(const json& root)
        {
            const auto& meta = root.at("meta");
            const auto& version = meta.at("schemaVersion");
            if (meta.at("schema") != "skyrimpoe.rules" || !version.is_number_integer() || version != 1) {
                throw std::runtime_error("unsupported rules pack schema");
            }

            RuleValues rules;
            {
                const auto& object = root.at("armour");
                rules.armour = {
                    .damageCoefficient = Number(object, "damageCoefficient", kPositive),
                    .physicalReductionCapPercent = Number(object, "physicalReductionCapPercent", kPositive, 100),
                    .roundingOffset = Number(object, "roundingOffset", 0, 1),
                    .zeroOverZeroPercent = Number(object, "zeroOverZeroPercent", 0, 100),
                    .brokenEnemyHitDamageTakenPercent = Number(object, "brokenEnemyHitDamageTakenPercent", 0, 100),
                    .brokenEnemyDurationSeconds = Number(object, "brokenEnemyDurationSeconds", kPositive),
                    .negativeArmourSign = Number(object, "negativeArmourSign", -1, -1),
                    .brokenPlayerDurationSeconds = Number(object, "brokenPlayerDurationSeconds", kPositive),
                };
            }
            {
                const auto& object = root.at("energyShield");
                rules.energyShield = {
                    .rechargeDelaySeconds = Number(object, "rechargeDelaySeconds", kPositive),
                    .rechargePercentPerMinute = Number(object, "rechargePercentPerMinute", kPositive),
                    .chaosConsumptionMultiplier = Number(object, "chaosConsumptionMultiplier", kPositive),
                    .normalConsumptionMultiplier = Number(object, "normalConsumptionMultiplier", kPositive),
                    .bleedBypasses = Boolean(object, "bleedBypasses"),
                    .poisonBypasses = Boolean(object, "poisonBypasses"),
                };
            }
            {
                const auto& object = root.at("hit");
                rules.hit = {
                    .playerAccuracyMultiplier = Number(object, "playerAccuracyMultiplier", kPositive),
                    .playerEvasionMultiplier = Number(object, "playerEvasionMultiplier", kPositive),
                    .monsterEvasionMultiplier = Number(object, "monsterEvasionMultiplier", kPositive),
                    .monsterAccuracyMultiplier = Number(object, "monsterAccuracyMultiplier", kPositive),
                    .maxPercent = Number(object, "maxPercent", 0, 100),
                    .minPercent = Number(object, "minPercent", 0, 100),
                    .negativeAccuracyPercent = Number(object, "negativeAccuracyPercent", 0, 100),
                    .maxEvadePercent = Number(object, "maxEvadePercent", 0, 100),
                    .baseEvasion = Number(object, "baseEvasion", 0),
                    .accuracyPerLevel = Number(object, "accuracyPerLevel", 0),
                    .accuracyPerDexterity = Number(object, "accuracyPerDexterity", 0),
                };
            }
            {
                const auto& object = root.at("block");
                rules.block = {
                    .baseMaxPercent = Number(object, "baseMaxPercent", 0, 100),
                    .capPercent = Number(object, "capPercent", 0, 100),
                };
            }
            {
                const auto& object = root.at("deflect");
                rules.deflect = {
                    .capPercent = Number(object, "capPercent", 0, 100),
                    .baseReductionPercent = Number(object, "baseReductionPercent", 0, 100),
                    .minimumRating = Number(object, "minimumRating", kPositive),
                    .ratingCoefficient = Number(object, "ratingCoefficient", kPositive),
                    .chanceScale = Number(object, "chanceScale", kPositive),
                    .chanceOffsetPercent = Number(object, "chanceOffsetPercent", kPositive),
                    .percentScale = Number(object, "percentScale", 0, 100),
                    .minPercent = Number(object, "minPercent", 0, 100),
                };
            }
            {
                const auto& object = root.at("resistance");
                rules.resistance = {
                    .baseMaxPercent = Number(object, "baseMaxPercent", 0, 100),
                    .monsterBaseMaxPercent = Number(object, "monsterBaseMaxPercent", 0, 100),
                    .capPercent = Number(object, "capPercent", 0, 100),
                };
            }
            {
                const auto& object = root.at("ignite");
                rules.ignite = {
                    .damagePercentPerMinute = Number(object, "damagePercentPerMinute", kPositive),
                    .durationSeconds = Number(object, "durationSeconds", kPositive),
                    .chanceMultiplier = Number(object, "chanceMultiplier", kPositive),
                    .flammabilityEnemyDurationSeconds = Number(object, "flammabilityEnemyDurationSeconds", kPositive),
                    .flammabilityPlayerDurationSeconds = Number(object, "flammabilityPlayerDurationSeconds", kPositive),
                    .nonHitFlammabilityMultiplier = Number(object, "nonHitFlammabilityMultiplier", 0, 1),
                    .nonHitIgniteThresholdPermyriad = Integer(object, "nonHitIgniteThresholdPermyriad", 0, 10000),
                    .flammabilityIncludesCurrentHit = Boolean(object, "flammabilityIncludesCurrentHit"),
                    .flammabilityIndependentExpiry = Boolean(object, "flammabilityIndependentExpiry"),
                    .flammabilityCapPercent = Number(object, "flammabilityCapPercent", 0, 100),
                };
            }
            {
                const auto& object = root.at("bleed");
                rules.bleed = {
                    .damagePercentPerMinute = Number(object, "damagePercentPerMinute", kPositive),
                    .durationSeconds = Number(object, "durationSeconds", kPositive),
                    .movingOrAggravatedMultiplier = Number(object, "movingOrAggravatedMultiplier", kPositive),
                };
            }
            {
                const auto& object = root.at("poison");
                rules.poison = {
                    .damagePercentPerMinute = Number(object, "damagePercentPerMinute", kPositive),
                    .durationSeconds = Number(object, "durationSeconds", kPositive),
                    .baseMaxStacks = Integer(object, "baseMaxStacks", 1),
                };
            }
            {
                const auto& object = root.at("shock");
                rules.shock = {
                    .effectPercent = Number(object, "effectPercent", 0, 100),
                    .chanceMultiplier = Number(object, "chanceMultiplier", kPositive),
                    .enemyDurationSeconds = Number(object, "enemyDurationSeconds", kPositive),
                    .playerDurationSeconds = Number(object, "playerDurationSeconds", kPositive),
                };
            }
            {
                const auto& object = root.at("chill");
                rules.chill = {
                    .effectMultiplier = Number(object, "effectMultiplier", kPositive),
                    .maxPercent = Number(object, "maxPercent", 0, 100),
                    .enemyDurationSeconds = Number(object, "enemyDurationSeconds", kPositive),
                    .playerDurationSeconds = Number(object, "playerDurationSeconds", kPositive),
                    .defaultPercent = Number(object, "defaultPercent", 0, 100),
                    .minPercent = Number(object, "minPercent", 0, 100),
                    .precision = Integer(object, "precision", 0, 15),
                    .discardBelowMinimum = Boolean(object, "discardBelowMinimum"),
                    .magicSlowReductionPercent = Number(object, "magicSlowReductionPercent", 0, 100),
                    .rareSlowReductionPercent = Number(object, "rareSlowReductionPercent", 0, 100),
                    .uniqueSlowReductionPercent = Number(object, "uniqueSlowReductionPercent", 0, 100),
                };
            }
            {
                const auto& object = root.at("freeze");
                rules.freeze = {
                    .damageScale = Number(object, "damageScale", kPositive),
                    .playerDamageScale = Number(object, "playerDamageScale", kPositive),
                    .enemyDurationSeconds = Number(object, "enemyDurationSeconds", kPositive),
                    .playerDurationSeconds = Number(object, "playerDurationSeconds", kPositive),
                };
            }
            {
                const auto& object = root.at("electrocute");
                rules.electrocute = {
                    .damageScale = Number(object, "damageScale", kPositive),
                    .enemyDurationMilliseconds = Number(object, "enemyDurationMilliseconds", kPositive),
                    .playerDurationMilliseconds = Number(object, "playerDurationMilliseconds", kPositive),
                    .repeatThresholdModifierPercent = Number(object, "repeatThresholdModifierPercent", kPositive),
                    .repeatModifierDurationSeconds = Number(object, "repeatModifierDurationSeconds", kPositive),
                };
            }
            {
                const auto& object = root.at("heavyStun");
                rules.heavyStun = {
                    .damageScale = Number(object, "damageScale", kPositive),
                    .playerDamageScale = Number(object, "playerDamageScale", kPositive),
                    .repeatThresholdModifierPercent = Number(object, "repeatThresholdModifierPercent", kPositive),
                    .repeatModifierDurationSeconds = Number(object, "repeatModifierDurationSeconds", kPositive),
                    .playerRepeatThresholdModifierPercent = Number(object, "playerRepeatThresholdModifierPercent", kPositive),
                    .playerRepeatModifierDurationSeconds = Number(object, "playerRepeatModifierDurationSeconds", kPositive),
                    .playerDurationMilliseconds = Number(object, "playerDurationMilliseconds", kPositive),
                    .enemyDurationMilliseconds = Number(object, "enemyDurationMilliseconds", kPositive),
                };
            }
            {
                const auto& object = root.at("pin");
                rules.pin = {
                    .damageScale = Number(object, "damageScale", kPositive),
                    .enemyDurationSeconds = Number(object, "enemyDurationSeconds", kPositive),
                    .playerDurationSeconds = Number(object, "playerDurationSeconds", kPositive),
                    .repeatThresholdModifierPercent = Number(object, "repeatThresholdModifierPercent", kPositive),
                    .repeatModifierDurationSeconds = Number(object, "repeatModifierDurationSeconds", kPositive),
                };
            }
            {
                const auto& object = root.at("poise");
                rules.poise = {
                    .playerDecayDelayMilliseconds = Number(object, "playerDecayDelayMilliseconds", kPositive),
                    .playerDecayPercentPerSecond = Number(object, "playerDecayPercentPerSecond", 0, 100),
                    .enemyDecayDelayMilliseconds = Number(object, "enemyDecayDelayMilliseconds", kPositive),
                    .enemyDecayPercentPerSecond = Number(object, "enemyDecayPercentPerSecond", 0, 100),
                    .magicThresholdMorePercent = Number(object, "magicThresholdMorePercent", 0),
                    .rareThresholdMorePercent = Number(object, "rareThresholdMorePercent", 0),
                    .uniqueThresholdMorePercent = Number(object, "uniqueThresholdMorePercent", 0),
                    .mapBossThresholdMorePercent = Number(object, "mapBossThresholdMorePercent", 0),
                    .fullBuildupPercent = Number(object, "fullBuildupPercent", kPositive, 100),
                };
            }
            {
                const auto& object = root.at("ailmentThreshold");
                rules.ailmentThreshold = {
                    .playerLifeFactor = Number(object, "playerLifeFactor", 0, 1),
                };
            }
            {
                const auto& object = root.at("life");
                rules.life = {
                    .perLevel = Number(object, "perLevel", 0),
                    .base = Number(object, "base", 0),
                    .perAttribute = Number(object, "perAttribute", 0),
                };
            }
            {
                const auto& object = root.at("mana");
                rules.mana = {
                    .perLevel = Number(object, "perLevel", 0),
                    .base = Number(object, "base", 0),
                    .perAttribute = Number(object, "perAttribute", 0),
                };
            }
            {
                const auto& object = root.at("critical");
                rules.critical = {
                    .playerBonusPercent = Number(object, "playerBonusPercent", 0),
                    .monsterBonusPercent = Number(object, "monsterBonusPercent", 0),
                    .playerMinionAdditionalBonusPercent = Number(object, "playerMinionAdditionalBonusPercent", 0),
                };
            }
            {
                const auto& object = root.at("levelTables");
                rules.levelTables = {
                    .life = Numbers<kRulesLevelCount>(object, "life", kPositive),
                    .damage = Numbers<kRulesLevelCount>(object, "damage", kPositive),
                    .accuracy = Numbers<kRulesLevelCount>(object, "accuracy", kPositive),
                    .evasion = Numbers<kRulesLevelCount>(object, "evasion", kPositive),
                    .ailmentThreshold = Numbers<kRulesLevelCount>(object, "ailmentThreshold", kPositive),
                    .poiseThreshold = Numbers<kRulesLevelCount>(object, "poiseThreshold", kPositive),
                    .mapLevelLifeMult = Numbers<kRulesLevelCount>(object, "mapLevelLifeMult", kPositive),
                };
            }
            {
                const auto& object = root.at("experienceReference");
                rules.experienceReference = {
                    .maxPenaltyFreeAreaLevel = Integer(object, "maxPenaltyFreeAreaLevel", 1, kRulesLevelCount),
                    .effectiveAreaLevelPenaltyMultiplier = Number(object, "effectiveAreaLevelPenaltyMultiplier", 0),
                    .safeZoneBase = Integer(object, "safeZoneBase"),
                    .safeZoneLevelDivisor = Integer(object, "safeZoneLevelDivisor", 1),
                    .numeratorLevelOffset = Integer(object, "numeratorLevelOffset", 1),
                    .denominatorLevelOffset = Integer(object, "denominatorLevelOffset", 1),
                    .differenceExponent = Number(object, "differenceExponent", kPositive),
                    .penaltyExponent = Number(object, "penaltyExponent", kPositive),
                    .highLevelPenaltyStartsAt = Integer(object, "highLevelPenaltyStartsAt", 1, kRulesLevelCount),
                    .safeDifferenceMax = Integer(object, "safeDifferenceMax"),
                    .safeMultiplier = Number(object, "safeMultiplier", 0, 1),
                    .displayMinimumMultiplierExclusive = Number(object, "displayMinimumMultiplierExclusive", 0, 1),
                    .highLevelPenaltyPerLevel = Number(object, "highLevelPenaltyPerLevel", 0),
                    .highLevelPenaltyOffset = Integer(object, "highLevelPenaltyOffset", 1, kRulesLevelCount),
                    .highLevelMultipliers = Numbers<5>(object, "highLevelMultipliers", 0.0, 1.0),
                };
            }
            {
                const auto& object = root.at("project").at("spirit");
                rules.projectSpirit = {
                    .base = Integer(object, "base"),
                    .achievementRewards = Numbers<4>(object, "achievementRewards", std::uint32_t{ 0 }, kMaxU32),
                    .decision = Integer(object, "decision", 1),
                };
            }
            {
                const auto& object = root.at("project").at("experience");
                rules.projectExperience = {
                    .higherEnemyBonusPerLevel = Number(object, "higherEnemyBonusPerLevel", 0),
                    .higherEnemyMaxMultiplier = Number(object, "higherEnemyMaxMultiplier", 1),
                    .decision = Integer(object, "decision", 1),
                };
            }

            if (!rules.ignite.flammabilityIncludesCurrentHit ||
                !rules.ignite.flammabilityIndependentExpiry || !rules.chill.discardBelowMinimum) {
                throw std::runtime_error("rules pack has unsupported ailment behavior");
            }
            if (rules.hit.minPercent > rules.hit.maxPercent ||
                rules.hit.negativeAccuracyPercent < rules.hit.minPercent ||
                rules.hit.negativeAccuracyPercent > rules.hit.maxPercent ||
                rules.chill.minPercent > rules.chill.maxPercent ||
                rules.chill.defaultPercent < rules.chill.minPercent ||
                rules.chill.defaultPercent > rules.chill.maxPercent ||
                rules.block.baseMaxPercent > rules.block.capPercent ||
                rules.deflect.minPercent > rules.deflect.capPercent ||
                rules.resistance.baseMaxPercent > rules.resistance.capPercent ||
                rules.resistance.monsterBaseMaxPercent > rules.resistance.capPercent) {
                throw std::runtime_error("rules pack contains inconsistent limits");
            }
            return rules;
        }
    }

    bool RulesPack::LoadFromJsonString(std::string_view text, std::string* error)
    {
        try {
            values_ = ReadRules(json::parse(text));
            if (error) {
                error->clear();
            }
            return true;
        } catch (const std::exception& failure) {
            if (error) {
                *error = failure.what();
            }
            return false;
        }
    }
}
