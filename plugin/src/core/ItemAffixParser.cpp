#include "core/ItemAffixParser.h"

#include <string_view>

namespace SkyrimPoE::Core
{
    void ItemAffixParser::ApplyAffix(
        const RolledAffix& rolled,
        const AffixDefinition& def,
        ItemStatTotals& totals)
    {
        std::size_t valOffset = 0;
        for (const auto& line : def.lines) {
            const std::size_t rangeCount = line.ranges.size();
            auto getVal = [&](std::size_t idx) -> double {
                const std::size_t flatIdx = valOffset + idx;
                if (flatIdx < rolled.values.size()) {
                    return rolled.values[flatIdx];
                }
                if (idx < rangeCount) {
                    return line.ranges[idx].minVal;
                }
                return 0.0;
            };

            const std::string_view tpl = line.templateEn;
            const double v0 = rangeCount >= 1 ? getVal(0) : 0.0;
            const double v1 = rangeCount >= 2 ? getVal(1) : v0;

            if (def.isLocal) {
                // --- 로컬 모디파이어 (해당 장비 베이스에만 곱연산/가산 적용) ---
                if (tpl == "+{0} to Armour") {
                    totals.localFlatArmour += v0;
                } else if (tpl == "{0}% increased Armour") {
                    totals.localIncArmourPct += v0;
                } else if (tpl == "+{0} to Evasion Rating") {
                    totals.localFlatEvasion += v0;
                } else if (tpl == "{0}% increased Evasion Rating") {
                    totals.localIncEvasionPct += v0;
                } else if (tpl == "+{0} to maximum Energy Shield") {
                    totals.localFlatEnergyShield += v0;
                } else if (tpl == "{0}% increased Energy Shield") {
                    totals.localIncEnergyShieldPct += v0;
                } else if (tpl == "{0}% increased Armour and Evasion") {
                    totals.localIncArmourPct += v0;
                    totals.localIncEvasionPct += v0;
                } else if (tpl == "{0}% increased Armour and Energy Shield") {
                    totals.localIncArmourPct += v0;
                    totals.localIncEnergyShieldPct += v0;
                } else if (tpl == "{0}% increased Evasion and Energy Shield") {
                    totals.localIncEvasionPct += v0;
                    totals.localIncEnergyShieldPct += v0;
                } else if (tpl == "{0}% increased Armour, Evasion and Energy Shield") {
                    totals.localIncArmourPct += v0;
                    totals.localIncEvasionPct += v0;
                    totals.localIncEnergyShieldPct += v0;
                } else if (tpl == "{0}% increased Chance to Block") {
                    totals.localIncBlockPct += v0;
                } else if (tpl == "Adds {0} to {1} Physical Damage") {
                    totals.localFlatPhysMin += v0;
                    totals.localFlatPhysMax += v1;
                } else if (tpl == "Adds {0} to {1} Fire Damage") {
                    totals.localFlatFireMin += v0;
                    totals.localFlatFireMax += v1;
                } else if (tpl == "Adds {0} to {1} Cold Damage") {
                    totals.localFlatColdMin += v0;
                    totals.localFlatColdMax += v1;
                } else if (tpl == "Adds {0} to {1} Lightning Damage") {
                    totals.localFlatLightningMin += v0;
                    totals.localFlatLightningMax += v1;
                } else if (tpl == "Adds {0} to {1} Chaos Damage") {
                    totals.localFlatChaosMin += v0;
                    totals.localFlatChaosMax += v1;
                } else if (tpl == "{0}% increased Physical Damage") {
                    totals.localIncPhysPct += v0;
                } else if (tpl == "{0}% increased Attack Speed") {
                    totals.localIncAttackSpeedPct += v0;
                } else if (tpl == "{0}% increased Critical Strike Chance") {
                    totals.localIncCritChancePct += v0;
                } else if (tpl == "+{0} to maximum Life") {
                    // 하이브리드 로컬 방어구+생명력 어픽스(예: BaseLocalDefencesAndLife): 생명력은 글로벌
                    totals.flatLife += v0;
                } else if (tpl == "+{0} to maximum Mana") {
                    totals.flatMana += v0;
                } else if (tpl == "+{0}% to Global Critical Strike Multiplier") {
                    // 검수 C1 방어적 가드: LocalCriticalMultiplier 계열이 로컬로 들어와도 글로벌 치명타 배율 반영
                    totals.globalCritMultiplierPct += v0;
                }
            } else {
                // --- 글로벌 모디파이어 (캐릭터 전체 합산) ---
                if (tpl == "+{0} to Strength") {
                    totals.flatStr += v0;
                } else if (tpl == "+{0} to Dexterity") {
                    totals.flatDex += v0;
                } else if (tpl == "+{0} to Intelligence") {
                    totals.flatInt += v0;
                } else if (tpl == "+{0} to all Attributes") {
                    totals.flatStr += v0;
                    totals.flatDex += v0;
                    totals.flatInt += v0;
                } else if (tpl == "+{0} to maximum Life") {
                    totals.flatLife += v0;
                } else if (tpl == "{0}% increased maximum Life") {
                    totals.incLifePct += v0;
                } else if (tpl == "+{0} to maximum Mana") {
                    totals.flatMana += v0;
                } else if (tpl == "{0}% increased maximum Mana") {
                    totals.incManaPct += v0;
                } else if (tpl == "+{0} to maximum Energy Shield") {
                    totals.flatEnergyShield += v0;
                } else if (tpl == "{0}% increased Energy Shield" || tpl == "{0}% increased maximum Energy Shield") {
                    totals.incEnergyShieldPct += v0;
                } else if (tpl == "+{0} to Armour") {
                    totals.flatArmour += v0;
                } else if (tpl == "{0}% increased Armour") {
                    totals.incArmourPct += v0;
                } else if (tpl == "+{0} to Evasion Rating") {
                    totals.flatEvasion += v0;
                } else if (tpl == "{0}% increased Evasion Rating") {
                    totals.incEvasionPct += v0;
                } else if (tpl == "{0}% increased Armour and Evasion") {
                    totals.incArmourAndEvasionPct += v0;
                } else if (tpl == "{0}% increased Armour and Energy Shield") {
                    totals.incArmourPct += v0;
                    totals.incEnergyShieldPct += v0;
                } else if (tpl == "{0}% increased Evasion and Energy Shield") {
                    totals.incEvasionPct += v0;
                    totals.incEnergyShieldPct += v0;
                } else if (tpl == "{0}% increased Armour, Evasion and Energy Shield") {
                    totals.incArmourAndEvasionPct += v0;
                    totals.incEnergyShieldPct += v0;
                } else if (tpl == "+{0}% to Fire Resistance") {
                    totals.fireResist += v0;
                } else if (tpl == "+{0}% to Cold Resistance") {
                    totals.coldResist += v0;
                } else if (tpl == "+{0}% to Lightning Resistance") {
                    totals.lightningResist += v0;
                } else if (tpl == "+{0}% to Chaos Resistance") {
                    totals.chaosResist += v0;
                } else if (tpl == "+{0}% to Fire and Cold Resistances") {
                    totals.fireResist += v0;
                    totals.coldResist += v0;
                } else if (tpl == "+{0}% to Fire and Lightning Resistances") {
                    totals.fireResist += v0;
                    totals.lightningResist += v0;
                } else if (tpl == "+{0}% to Cold and Lightning Resistances") {
                    totals.coldResist += v0;
                    totals.lightningResist += v0;
                } else if (tpl == "+{0}% to all Elemental Resistances") {
                    totals.fireResist += v0;
                    totals.coldResist += v0;
                    totals.lightningResist += v0;
                } else if (tpl == "{0}% additional Physical Damage Reduction") {
                    totals.physDamageReduction += v0;
                } else if (tpl == "+{0}% chance to Suppress Spell Damage") {
                    totals.spellSuppressionChance += v0;
                } else if (tpl == "+{0}% Chance to Block Attack Damage") {
                    totals.attackBlockChance += v0;
                } else if (tpl == "+{0}% Chance to Block Spell Damage" || tpl == "{0}% Chance to Block Spell Damage") {
                    totals.spellBlockChance += v0;
                } else if (tpl == "{0}% increased Energy Shield Recharge Rate") {
                    totals.incEsRechargeRatePct += v0;
                } else if (tpl == "{0}% faster start of Energy Shield Recharge") {
                    totals.fasterEsRechargeStartPct += v0;
                } else if (tpl == "Adds {0} to {1} Physical Damage to Attacks") {
                    totals.flatAttackPhysMin += v0;
                    totals.flatAttackPhysMax += v1;
                } else if (tpl == "Adds {0} to {1} Fire Damage to Attacks") {
                    totals.flatAttackFireMin += v0;
                    totals.flatAttackFireMax += v1;
                } else if (tpl == "Adds {0} to {1} Cold Damage to Attacks") {
                    totals.flatAttackColdMin += v0;
                    totals.flatAttackColdMax += v1;
                } else if (tpl == "Adds {0} to {1} Lightning Damage to Attacks") {
                    totals.flatAttackLightningMin += v0;
                    totals.flatAttackLightningMax += v1;
                } else if (tpl == "Adds {0} to {1} Chaos Damage to Attacks") {
                    totals.flatAttackChaosMin += v0;
                    totals.flatAttackChaosMax += v1;
                } else if (tpl == "Adds {0} to {1} Fire Damage to Spells") {
                    totals.flatSpellFireMin += v0;
                    totals.flatSpellFireMax += v1;
                } else if (tpl == "Adds {0} to {1} Cold Damage to Spells") {
                    totals.flatSpellColdMin += v0;
                    totals.flatSpellColdMax += v1;
                } else if (tpl == "Adds {0} to {1} Lightning Damage to Spells") {
                    totals.flatSpellLightningMin += v0;
                    totals.flatSpellLightningMax += v1;
                } else if (tpl == "Adds {0} to {1} Chaos Damage to Spells") {
                    totals.flatSpellChaosMin += v0;
                    totals.flatSpellChaosMax += v1;
                } else if (tpl == "Adds {0} to {1} Fire Damage to Spells and Attacks") {
                    totals.flatAttackFireMin += v0;
                    totals.flatAttackFireMax += v1;
                    totals.flatSpellFireMin += v0;
                    totals.flatSpellFireMax += v1;
                } else if (tpl == "Adds {0} to {1} Cold Damage to Spells and Attacks") {
                    totals.flatAttackColdMin += v0;
                    totals.flatAttackColdMax += v1;
                    totals.flatSpellColdMin += v0;
                    totals.flatSpellColdMax += v1;
                } else if (tpl == "Adds {0} to {1} Lightning Damage to Spells and Attacks") {
                    totals.flatAttackLightningMin += v0;
                    totals.flatAttackLightningMax += v1;
                    totals.flatSpellLightningMin += v0;
                    totals.flatSpellLightningMax += v1;
                } else if (tpl == "{0}% increased Physical Damage" || tpl == "{0}% increased Global Physical Damage") {
                    totals.incPhysicalDamagePct += v0;
                } else if (tpl == "{0}% increased Melee Damage") {
                    totals.incMeleeDamagePct += v0;
                } else if (tpl == "{0}% increased Melee Physical Damage") {
                    totals.incMeleePhysicalDamagePct += v0;
                } else if (tpl == "{0}% increased Elemental Damage") {
                    totals.incElementalDamagePct += v0;
                } else if (tpl == "{0}% increased Damage") {
                    totals.incGenericDamagePct += v0;
                } else if (tpl == "{0}% increased Area Damage") {
                    totals.incAreaDamagePct += v0;
                } else if (tpl == "{0}% increased Area of Effect") {
                    totals.incAreaOfEffectPct += v0;
                } else if (tpl == "{0}% increased Area of Effect for Attacks") {
                    totals.incAttackAreaOfEffectPct += v0;
                } else if (tpl == "{0}% increased Projectile Damage") {
                    totals.incProjectileDamagePct += v0;
                } else if (tpl == "{0}% increased Chaos Damage") {
                    totals.incChaosDamagePct += v0;
                } else if (tpl == "{0}% increased Spell Damage") {
                    totals.incSpellDamagePct += v0;
                } else if (tpl == "{0}% increased Fire Damage") {
                    totals.incFireDamagePct += v0;
                } else if (tpl == "{0}% increased Cold Damage") {
                    totals.incColdDamagePct += v0;
                } else if (tpl == "{0}% increased Lightning Damage") {
                    totals.incLightningDamagePct += v0;
                } else if (tpl == "{0}% increased Elemental Damage with Attack Skills") {
                    totals.incElementalAttackDamagePct += v0;
                } else if (tpl == "{0}% increased Attack Speed") {
                    totals.incAttackSpeedPct += v0;
                } else if (tpl == "{0}% increased Cast Speed") {
                    totals.incCastSpeedPct += v0;
                } else if (tpl == "{0}% increased Global Critical Strike Chance" || tpl == "{0}% increased Critical Strike Chance") {
                    totals.incGlobalCritChancePct += v0;
                } else if (tpl == "{0}% increased Spell Critical Strike Chance" || tpl == "{0}% increased Critical Strike Chance for Spells") {
                    totals.incSpellCritChancePct += v0;
                } else if (tpl == "+{0}% to Global Critical Strike Multiplier") {
                    totals.globalCritMultiplierPct += v0;
                } else if (tpl == "+{0}% to Critical Strike Multiplier for Spell Damage") {
                    totals.spellCritMultiPct += v0;
                } else if (tpl == "+{0}% to Melee Critical Strike Multiplier") {
                    totals.meleeCritMultiplierPct += v0;
                } else if (tpl == "+{0}% to Critical Strike Multiplier with Elemental Skills") {
                    totals.elementalCritMultiPct += v0;
                } else if (tpl == "Regenerate {0} Life per second") {
                    totals.lifeRegenPerSec += v0;
                } else if (tpl == "{0}% increased Mana Regeneration Rate") {
                    totals.incManaRegenPct += v0;
                } else if (tpl == "Regenerate {0} Mana per second") {
                    totals.manaRegenPerSec += v0;
                } else if (tpl == "{0}% increased Movement Speed") {
                    totals.incMovementSpeedPct += v0;
                } else if (tpl == "+{0} to maximum Spirit") {
                    totals.flatSpirit += v0;
                } else if (tpl == "-{0} to Total Mana Cost of Skills") {
                    totals.flatManaCostDelta -= v0;
                } else if (tpl == "{0}% increased Damage with Hits against Rare monsters") {
                    totals.incDamageVsRarePct += v0;
                } else if (tpl == "{0}% of Damage taken Recouped as Energy Shield") {
                    totals.esRecoupPct += v0;
                } else if (tpl == "+{0}% to Item Rarity") {
                    totals.itemRarityPct += v0;
                } else if (tpl == "{0}% reduced Damage when on Low Life") {
                    totals.reducedDamageOnLowLifePct += v0;
                } else if (tpl == "{0}% increased Global Evasion Rating when on Low Life") {
                    totals.incEvasionOnLowLifePct += v0;
                } else if (tpl == "-{0} Physical Damage taken from Attack Hits") {
                    totals.flatPhysTakenFromAttacksDelta -= v0;
                } else if (tpl == "Cannot be Frozen") {
                    totals.cannotBeFrozen = true;
                } else if (tpl == "Chaos Damage taken does not bypass Energy Shield") {
                    totals.shavronneChaosEs = true;
                } else if (tpl == "Your Physical Damage can Shock") {
                    totals.starforgeShock = true;
                } else if (tpl == "Deal no Elemental Damage") {
                    totals.dealNoElemental = true;
                } else if (tpl == "When you Kill a Shocked Enemy, inflict an equivalent Shock on each Enemy within {0} metres" ||
                           tpl == "When you Kill an Ignited Enemy, inflict an equivalent Ignite on each Enemy within {0} metres") {
                    totals.berekProlif = true;
                } else if (tpl == "When you Kill a Rare monster, you gain its Modifiers for {0} seconds") {
                    totals.headhunter = true;
                }
            }

            valOffset += rangeCount;
        }
    }

    ItemStatTotals ItemAffixParser::ParseItem(const PoEItemRecord& record, const ItemData& data)
    {
        ItemStatTotals totals{};
        for (const auto& rolled : record.implicitAffixes) {
            if (const auto* def = data.FindAffix(rolled.affixId)) {
                ApplyAffix(rolled, *def, totals);
            }
        }
        for (const auto& rolled : record.affixes) {
            if (const auto* def = data.FindAffix(rolled.affixId)) {
                ApplyAffix(rolled, *def, totals);
            }
        }
        return totals;
    }
}
