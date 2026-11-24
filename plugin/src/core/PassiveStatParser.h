#pragma once

#include "core/PassiveTreeData.h"
#include "core/Types.h"

#include <cstdint>
#include <string_view>

namespace SkyrimPoE::Core
{
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

        std::size_t parsedStatLines = 0;
        std::size_t unparsedStatLines = 0;
    };

    class PassiveStatParser
    {
    public:
        // 영문 PoB 3.30 단일 스탯 줄을 해석해 out에 누적한다. 매핑 성공 시 true 반환.
        static bool ParseLine(std::string_view statLine, PassiveStatTotals& out);

        // 현재 할당된 패시브 노드 + 마스터리 선택 전체의 스탯을 집계한다.
        [[nodiscard]] static PassiveStatTotals AggregateTree(
            const PassiveTreeData& data,
            const PassiveTreeState& state);
    };
}
