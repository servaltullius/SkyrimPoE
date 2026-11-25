#pragma once

#include "core/PassiveTreeData.h"
#include "core/Types.h"

#include <cstdint>
#include <map>
#include <span>
#include <string_view>

namespace SkyrimPoE::Core
{
    enum class ChargeKind : std::uint8_t { Endurance, Frenzy, Power, Count };
    using DamageTypeValues = std::array<double, static_cast<std::size_t>(DamageType::Count)>;

    struct ExtraDamageStats
    {
        std::array<DamageTypeValues, static_cast<std::size_t>(DamageType::Count)> gain{};
        DamageTypeValues attackGain{};
        DamageTypeValues spellGain{};
        double randomElementGainPct = 0.0;
        double coldAsFireAgainstFrozenPct = 0.0;
        double physicalAsColdAgainstShockedPct = 0.0;
        double physicalAsLightningAgainstChilledPct = 0.0;
    };
    enum class PassiveWeapon : std::uint8_t { OneHand, TwoHand, Sword, Axe, Mace, Dagger, Count };

    struct PassiveWeaponStats
    {
        double damageIncPct = 0.0;
        double speedIncPct = 0.0;
        double critChanceIncPct = 0.0;
        double critBonusIncPct = 0.0;
    };

    struct PassiveAilmentStats
    {
        double chanceIncPct = 0.0;
        double magnitudeIncPct = 0.0;
        double durationIncPct = 0.0;
        double damageFasterPct = 0.0;
    };

    // 패시브 트리 전체에서 집계된 정적 스탯·키스톤 합산치(Layer 1 입력)
    struct PassiveStatTotals
    {
        // 능력치 (1패스)
        double flatStr = 0.0;
        double flatDex = 0.0;
        double flatInt = 0.0;
        double incStrPct = 0.0;
        double incDexPct = 0.0;
        double incIntPct = 0.0;

        // 풀 (생명·마나·에너지 실드)
        double flatLife = 0.0;
        double incLifePct = 0.0;
        double moreLifeMult = 1.0;
        double moreManaMult = 1.0;
        double flatMana = 0.0;
        double incManaPct = 0.0;
        double flatEnergyShield = 0.0;
        double incEnergyShieldPct = 0.0;
        double incEsRechargeRatePct = 0.0;
        double fasterEsRechargeStartPct = 0.0;

        // 방어·회피·저항·막기·억제
        double flatArmour = 0.0;
        double incArmourPct = 0.0;
        double flatEvasion = 0.0;
        double incEvasionPct = 0.0;
        double incArmourAndEvasionPct = 0.0;  // 하이브리드(% increased Armour and Evasion / Global Defences): IR 전환 시 1회만 합산
        double fireResist = 0.0;
        double coldResist = 0.0;
        double lightningResist = 0.0;
        double chaosResist = 0.0;
        double maxFireResistBonus = 0.0;
        double maxColdResistBonus = 0.0;
        double maxLightningResistBonus = 0.0;
        double maxChaosResistBonus = 0.0;
        double physDamageReduction = 0.0;
        double attackBlockChance = 0.0;
        double attackBlockMaxBonusPct = 0.0;
        double spellBlockChance = 0.0;
        double spellBlockMaxBonusPct = 0.0;
        double spellSuppressionChance = 0.0;
        double spellSuppressionPrevent = 0.0;

        // 공격·주문·속도·치명타·범위·비용
        double incGlobalDamagePct = 0.0;
        double incPhysicalDamagePct = 0.0;
        double incMeleeDamagePct = 0.0;
        double incMeleePhysicalDamagePct = 0.0;
        double incAttackDamagePct = 0.0;
        double incPhysicalAttackDamagePct = 0.0;
        double incElementalDamagePct = 0.0;
        double incElementalAttackDamagePct = 0.0;
        double incFireDamagePct = 0.0;
        double incColdDamagePct = 0.0;
        double incLightningDamagePct = 0.0;
        double incChaosDamagePct = 0.0;
        double incSpellDamagePct = 0.0;
        double incAreaDamagePct = 0.0;

        double incAttackSpeedPct = 0.0;
        double incMeleeAttackSpeedPct = 0.0;
        double incCastSpeedPct = 0.0;

        double incGlobalCritChancePct = 0.0;
        double incAttackCritChancePct = 0.0;
        double incMeleeCritChancePct = 0.0;
        double incSpellCritChancePct = 0.0;
        double globalCritMultiplierPct = 0.0;
        double meleeCritMultiplierPct = 0.0;
        double spellCritMultiplierPct = 0.0;

        double incAreaOfEffectPct = 0.0;
        double reducedManaCostPct = 0.0;        // 구버전/일반 reduced(+)·increased(-) mana cost
        double costEfficiencyPct = 0.0;         // PoE 3.30: cost / (1 + eff/100)
        double attackCostEfficiencyPct = 0.0;
        double spellCostEfficiencyPct = 0.0;
        double manaCostEfficiencyPct = 0.0;
        double manaAttackCostEfficiencyPct = 0.0;
        double manaSpellCostEfficiencyPct = 0.0;
        double lifeCostEfficiencyPct = 0.0;

        // 5단계(N-1): 전역 상태이상 유발 확률(%)
        double igniteChancePct = 0.0;
        double freezeChancePct = 0.0;
        double shockChancePct = 0.0;
        double bleedChancePct = 0.0;
        double poisonChancePct = 0.0;
        // 재검수 R-1: 공격·주문 한정 문구("Attacks have N% chance to Ignite", "... Poison on Hit with Attacks/Spell Damage")
        double attackIgniteChancePct = 0.0;
        double attackBleedChancePct = 0.0;
        double attackPoisonChancePct = 0.0;
        double spellPoisonChancePct = 0.0;

        // 충전 상한 가산
        std::int32_t maxEnduranceBonus = 0;
        std::int32_t maxFrenzyBonus = 0;
        std::int32_t maxPowerBonus = 0;

        // 3단계 핵심 키스톤 10종
        bool bloodMagic = false;
        bool chaosInoculation = false;
        bool resoluteTechnique = false;
        bool ironReflexes = false;
        bool unwaveringStance = false;  // 기절 면역 (Unwavering Stance / Juggernaut "Cannot be Stunned")
        bool cannotEvade = false;       // 회피 불가 ("Cannot Evade enemy Attacks", Unwavering Stance 전용)
        bool acrobatics = false;
        double mindOverMatterRatio = 0.0;  // 0.0 또는 0.40
        bool elementalOverload = false;
        bool painAttunement = false;
        bool ironWill = false;

        // Generation 1 uses its own wording and mechanics.
        bool poe2 = false;
        double convertToFirePct = 0.0;
        bool dealOnlyFire = false;
        double convertFireSpellToChaosPct = 0.0;
        bool chaosFireSpellIgniteMagnitude = false;
        bool fireSpellIgniteDealsChaos = false;
        bool chaosAndBleedImmune = false;
        double moreOneHandEmptyOffHandSpeedPct = 0.0;
        double convertEsToManaPct = 0.0;
        double moreManaCostMult = 1.0;
        bool unluckyEvade = false;
        bool luckyDeflect = false;
        double manaRecoveryMult = 1.0;
        double fullLifeCritBonusMult = 1.0;
        double lowLifeCritBonusMult = 1.0;
        std::array<ChargeKind, static_cast<std::size_t>(ChargeKind::Count)> chargeGain{
            ChargeKind::Endurance, ChargeKind::Frenzy, ChargeKind::Power
        };
        bool excessLifeRegenToEs = false;
        bool esCannotRecharge = false;
        bool alwaysAggravateBleed = false;
        double bleedBaseDuration = 0.0;
        double bleedMagnitudeMult = 1.0;

        double flatLifeRegen = 0.0;
        double percentLifeRegen = 0.0;
        double incLifeRegenPct = 0.0;
        double incLifeRecoveryPct = 0.0;
        double flatManaRegen = 0.0;
        double percentManaRegen = 0.0;
        double incManaRegenPct = 0.0;
        double incManaRecoveryPct = 0.0;
        double incBlockChancePct = 0.0;
        double flatDeflection = 0.0;
        double incDeflectionPct = 0.0;
        double evasionAsDeflectionPct = 0.0;
        double deflectReductionBonusPct = 0.0;
        double evasionAsExtraArmourPct = 0.0;
        std::array<double, static_cast<std::size_t>(DamageType::Count)> armourAppliesPct{};
        double globalCritBonusIncPct = 0.0;
        double attackCritBonusIncPct = 0.0;
        double spellCritBonusIncPct = 0.0;
        double baseAttackCritPct = 0.0;
        double baseGlobalCritPct = 0.0;
        double attackAreaIncPct = 0.0;
        double spellAreaIncPct = 0.0;
        double attackAreaDamageIncPct = 0.0;
        double spellAreaDamageIncPct = 0.0;
        double projectileDamageIncPct = 0.0;
        double attackColdDamageIncPct = 0.0;
        double shieldDefencesIncPct = 0.0;
        double shieldAttackDamageIncPct = 0.0;
        double dualWieldAttackDamageIncPct = 0.0;
        double dualWieldAttackSpeedIncPct = 0.0;
        double meleeWeaponSpellDamageIncPct = 0.0;
        double lowLifeAttackDamageIncPct = 0.0;
        double lowLifeLifeRegenIncPct = 0.0;
        double lowLifePercentLifeRegen = 0.0;
        double lowLifeArmourAndEvasionIncPct = 0.0;
        double lowLifeCastSpeedIncPct = 0.0;
        double fullLifeAttackDamageIncPct = 0.0;
        double fullLifeCastSpeedIncPct = 0.0;
        double fullLifeEvasionIncPct = 0.0;
        double fullLifeManaRegenIncPct = 0.0;
        double movingAttackDamageIncPct = 0.0;
        double movingEvasionIncPct = 0.0;
        double movingDeflectionIncPct = 0.0;
        double movingLifeRegenIncPct = 0.0;
        double movingManaRegenIncPct = 0.0;
        double stationaryArmourIncPct = 0.0;
        double stationaryLifeRegenIncPct = 0.0;
        double stationaryManaRegenIncPct = 0.0;
        double stationaryPercentLifeRegen = 0.0;
        double hitDamageIncAgainstElementalAilmentsPct = 0.0;
        double hitDamageIncAgainstAnyAilmentsPct = 0.0;
        double hitDamageIncAgainstIgnitedPct = 0.0;
        double hitDamageIncAgainstBurningPct = 0.0;
        double hitDamageIncAgainstFrozenPct = 0.0;
        double reducedIncomingCritBonusPct = 0.0;
        double reducedIncomingIgniteMagnitudePct = 0.0;
        double reducedIncomingShockMagnitudePct = 0.0;
        double deflectReductionCritBonusPct = 0.0;
        bool deflectPreventsBleed = false;
        double cooldownRecoveryIncPct = 0.0;
        double skillDurationIncPct = 0.0;
        double triggeredSpellDamageIncPct = 0.0;
        double channellingDamageIncPct = 0.0;
        double channellingSpeedIncPct = 0.0;
        std::array<PassiveWeaponStats, static_cast<std::size_t>(PassiveWeapon::Count)> weapons{};
        double flatSpirit = 0.0;
        double incSpiritPct = 0.0;
        double incMovementSpeedPct = 0.0;
        double incSprintingMovementSpeedPct = 0.0;
        double incDualWieldMovementSpeedPct = 0.0;
        double incCharmMovementSpeedPct = 0.0;
        double incEsMovementSpeedPct = 0.0;
        DamageTypeValues penetrationPct{};
        double attackElementalPenetrationPct = 0.0;
        ExtraDamageStats extraDamage{};
        double manaAsLifeCostPct = 0.0;
        double spellManaAsLifeCostPct = 0.0;
        double incLifeCostPct = 0.0;
        bool matchingBaseLifeCost = false;
        double lifeCostSpellDamageIncPct = 0.0;
        double projectilePierceChancePct = 0.0;
        std::array<double, 16> additionalSpellProjectilesChancePct{};
        double additionalProjectileChancePct = 0.0;
        double additionalChainChancePct = 0.0;
        double additionalForkProjectileChancePct = 0.0;
        double additionalProjectiles = 0.0;
        double projectileSpeedIncPct = 0.0;
        double spellProjectileSpeedIncPct = 0.0;
        double projectileAilmentChanceIncPct = 0.0;
        double auraMagnitudeIncPct = 0.0;
        double heraldReservationEfficiencyPct = 0.0;
        double elementalSkillSpeedIncPct = 0.0;
        double lightningSkillSpeedIncPct = 0.0;
        double coldCastSpeedIncPct = 0.0;
        double lifeRecoveryOnKillPct = 0.0;
        double manaRecoveryOnKillPct = 0.0;
        double lifeOnKill = 0.0;
        double armourAsDeflectionPct = 0.0;
        bool noInherentManaRegen = false;
        bool noSpirit = false;
        double bodyArmourIncPct = 0.0;
        double bodyEvasionIncPct = 0.0;
        double bodyEsIncPct = 0.0;
        double helmetEsIncPct = 0.0;
        double armourFromStrRequirementsPct = 0.0;
        double baseBlockCapOverridePct = 0.0;
        double blockedHitDamageTakenPct = 0.0;
        double energyShieldBypassPct = 0.0;
        double maximumEvadePct = 100.0;
        double maximumPhysicalReductionPct = 100.0;
        double addedAttackLightningMin = 0.0;
        double addedAttackLightningMax = 0.0;
        double incFreezeBuildupPct = 0.0;
        double flammabilityIncPct = 0.0;
        double elementalAilmentThresholdIncPct = 0.0;
        double esAsAilmentThresholdPct = 0.0;
        double freezeThresholdIncPct = 0.0;
        double esAsFreezeThresholdPct = 0.0;
        double incAilmentChancePct = 0.0;
        double incAilmentMagnitudePct = 0.0;
        double reducedIncomingAilmentDurationPct = 0.0;
        std::array<PassiveAilmentStats, static_cast<std::size_t>(AilmentKind::Count)> ailments{};

        std::size_t parsedStatLines = 0;
        std::size_t unparsedStatLines = 0;
    };

    class PassiveStatParser
    {
    public:
        // 영문 PoB 3.30 단일 스탯 줄을 해석해 out에 누적한다. 매핑 성공 시 true 반환.
        static bool ParseLine(std::string_view statLine, PassiveStatTotals& out);
        static bool ParsePoe2Line(std::string_view statLine, PassiveStatTotals& out, bool keystone = false);
        [[nodiscard]] static bool IsPoe2Drawback(std::string_view statLine);
        [[nodiscard]] static std::string_view UnimplementedPoe2ManaRegenReplacement(std::span<const std::string> lines);
        [[nodiscard]] static std::map<std::string, double> Describe(const PassiveStatTotals& totals);

        // 현재 할당된 패시브 노드 + 마스터리 선택 전체의 스탯을 집계한다.
        [[nodiscard]] static PassiveStatTotals AggregateTree(
            const PassiveTreeData& data,
            const PassiveTreeState& state);
    };
}
