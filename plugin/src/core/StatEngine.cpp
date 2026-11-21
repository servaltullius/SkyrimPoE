#include "core/StatEngine.h"

#include <algorithm>
#include <cmath>

namespace SkyrimPoE::Core
{
    namespace
    {
        // PoE 3.30(PoB 2.68.1 Data/Misc.lua) 기본 충전 보너스:
        // 인내: 물리 피해 감소 +4% + 원소 피해 감소 +4%(저항 아님), 격분: 공속 +4%/피해 증폭 +4%, 권능: 치명타 +50%
        constexpr double kEndurancePhysAndElemDR = 4.0;
        constexpr double kFrenzySpeedAndMore = 4.0;
        constexpr double kPowerCritInc = 50.0;

        struct AggregatedGearStats
        {
            double armour = 0.0;
            double evasion = 0.0;
            double energyShield = 0.0;
            double attackBlock = 0.0;

            double flatLife = 0.0;
            double incLifePct = 0.0;
            double flatMana = 0.0;
            double incManaPct = 0.0;
            double flatEnergyShield = 0.0;
            double incEnergyShieldPct = 0.0;

            double flatArmour = 0.0;
            double incArmourPct = 0.0;
            double flatEvasion = 0.0;
            double incEvasionPct = 0.0;
            double incArmourAndEvasionPct = 0.0;

            double fireResist = 0.0;
            double coldResist = 0.0;
            double lightningResist = 0.0;
            double chaosResist = 0.0;
            double physDamageReduction = 0.0;

            double attackBlockChance = 0.0;
            double spellBlockChance = 0.0;
            double spellSuppressionChance = 0.0;
            double incEsRechargeRatePct = 0.0;
            double fasterEsRechargeStartPct = 0.0;

            // 무기 로컬(주무기/쌍수) + 글로벌 공격 추가 피해
            bool hasActiveWeapon = false;
            bool isDualWielding = false;
            double weaponPhysMin = 0.0;
            double weaponPhysMax = 0.0;
            double weaponFireMin = 0.0;
            double weaponFireMax = 0.0;
            double weaponColdMin = 0.0;
            double weaponColdMax = 0.0;
            double weaponLightningMin = 0.0;
            double weaponLightningMax = 0.0;
            double weaponChaosMin = 0.0;
            double weaponChaosMax = 0.0;
            double weaponCritChancePct = 5.0;
            double weaponLocalSpeedMult = 1.0;

            double addedAttackPhysMin = 0.0;
            double addedAttackPhysMax = 0.0;
            double addedAttackFireMin = 0.0;
            double addedAttackFireMax = 0.0;
            double addedAttackColdMin = 0.0;
            double addedAttackColdMax = 0.0;
            double addedAttackLightningMin = 0.0;
            double addedAttackLightningMax = 0.0;
            double addedAttackChaosMin = 0.0;
            double addedAttackChaosMax = 0.0;

            double addedSpellFireMin = 0.0;
            double addedSpellFireMax = 0.0;
            double addedSpellColdMin = 0.0;
            double addedSpellColdMax = 0.0;
            double addedSpellLightningMin = 0.0;
            double addedSpellLightningMax = 0.0;
            double addedSpellChaosMin = 0.0;
            double addedSpellChaosMax = 0.0;

            double incPhysicalDamagePct = 0.0;
            double incSpellDamagePct = 0.0;
            double incFireDamagePct = 0.0;
            double incColdDamagePct = 0.0;
            double incLightningDamagePct = 0.0;
            double incElementalAttackDamagePct = 0.0;

            double incAttackSpeedPct = 0.0;
            double incCastSpeedPct = 0.0;
            double incGlobalCritChancePct = 0.0;
            double incSpellCritChancePct = 0.0;
            double globalCritMultiplierPct = 0.0;

            double lifeRegenPerSec = 0.0;
            double incManaRegenPct = 0.0;
            double manaRegenPerSec = 0.0;
            double incMovementSpeedPct = 0.0;

            // 5단계(정신력·주얼·고유 아이템)
            double flatSpirit = 0.0;
            double flatManaCostDelta = 0.0;
            double incGenericDamagePct = 0.0;
            double incMeleeDamagePct = 0.0;
            double incMeleePhysicalDamagePct = 0.0;
            double incElementalDamagePct = 0.0;
            double incAreaDamagePct = 0.0;
            double incAreaOfEffectPct = 0.0;
            double incAttackAreaOfEffectPct = 0.0;
            double incProjectileDamagePct = 0.0;
            double incChaosDamagePct = 0.0;
            double spellCritMultiPct = 0.0;
            double meleeCritMultiplierPct = 0.0;
            double elementalCritMultiPct = 0.0;
            double incDamageVsRarePct = 0.0;
            double esRecoupPct = 0.0;
            double itemRarityPct = 0.0;
            double reducedDamageOnLowLifePct = 0.0;
            double incEvasionOnLowLifePct = 0.0;
            double flatPhysTakenFromAttacksDelta = 0.0;

            bool cannotBeFrozen = false;
            bool shavronneChaosEs = false;
            bool starforgeShock = false;
            bool dealNoElemental = false;
            bool berekProlif = false;
            bool headhunter = false;
        };

        void AccumulateGlobalItemStats(AggregatedGearStats& gear, const ItemStatTotals& s) noexcept
        {
            gear.flatLife += s.flatLife;
            gear.incLifePct += s.incLifePct;
            gear.flatMana += s.flatMana;
            gear.incManaPct += s.incManaPct;
            gear.flatEnergyShield += s.flatEnergyShield;
            gear.incEnergyShieldPct += s.incEnergyShieldPct;
            gear.flatArmour += s.flatArmour;
            gear.incArmourPct += s.incArmourPct;
            gear.flatEvasion += s.flatEvasion;
            gear.incEvasionPct += s.incEvasionPct;
            gear.incArmourAndEvasionPct += s.incArmourAndEvasionPct;
            gear.fireResist += s.fireResist;
            gear.coldResist += s.coldResist;
            gear.lightningResist += s.lightningResist;
            gear.chaosResist += s.chaosResist;
            gear.physDamageReduction += s.physDamageReduction;
            gear.attackBlockChance += s.attackBlockChance;
            gear.spellBlockChance += s.spellBlockChance;
            gear.spellSuppressionChance += s.spellSuppressionChance;
            gear.incEsRechargeRatePct += s.incEsRechargeRatePct;
            gear.fasterEsRechargeStartPct += s.fasterEsRechargeStartPct;

            gear.addedAttackPhysMin += s.flatAttackPhysMin;
            gear.addedAttackPhysMax += s.flatAttackPhysMax;
            gear.addedAttackFireMin += s.flatAttackFireMin;
            gear.addedAttackFireMax += s.flatAttackFireMax;
            gear.addedAttackColdMin += s.flatAttackColdMin;
            gear.addedAttackColdMax += s.flatAttackColdMax;
            gear.addedAttackLightningMin += s.flatAttackLightningMin;
            gear.addedAttackLightningMax += s.flatAttackLightningMax;
            gear.addedAttackChaosMin += s.flatAttackChaosMin;
            gear.addedAttackChaosMax += s.flatAttackChaosMax;

            gear.addedSpellFireMin += s.flatSpellFireMin;
            gear.addedSpellFireMax += s.flatSpellFireMax;
            gear.addedSpellColdMin += s.flatSpellColdMin;
            gear.addedSpellColdMax += s.flatSpellColdMax;
            gear.addedSpellLightningMin += s.flatSpellLightningMin;
            gear.addedSpellLightningMax += s.flatSpellLightningMax;
            gear.addedSpellChaosMin += s.flatSpellChaosMin;
            gear.addedSpellChaosMax += s.flatSpellChaosMax;

            gear.incPhysicalDamagePct += s.incPhysicalDamagePct;
            gear.incSpellDamagePct += s.incSpellDamagePct;
            gear.incFireDamagePct += s.incFireDamagePct;
            gear.incColdDamagePct += s.incColdDamagePct;
            gear.incLightningDamagePct += s.incLightningDamagePct;
            gear.incElementalAttackDamagePct += s.incElementalAttackDamagePct;

            gear.incAttackSpeedPct += s.incAttackSpeedPct;
            gear.incCastSpeedPct += s.incCastSpeedPct;
            gear.incGlobalCritChancePct += s.incGlobalCritChancePct;
            gear.incSpellCritChancePct += s.incSpellCritChancePct;
            gear.globalCritMultiplierPct += s.globalCritMultiplierPct;
            gear.lifeRegenPerSec += s.lifeRegenPerSec;
            gear.incManaRegenPct += s.incManaRegenPct;
            gear.manaRegenPerSec += s.manaRegenPerSec;
            gear.incMovementSpeedPct += s.incMovementSpeedPct;

            gear.flatSpirit += s.flatSpirit;
            gear.flatManaCostDelta += s.flatManaCostDelta;
            gear.incGenericDamagePct += s.incGenericDamagePct;
            gear.incMeleeDamagePct += s.incMeleeDamagePct;
            gear.incMeleePhysicalDamagePct += s.incMeleePhysicalDamagePct;
            gear.incElementalDamagePct += s.incElementalDamagePct;
            gear.incAreaDamagePct += s.incAreaDamagePct;
            gear.incAreaOfEffectPct += s.incAreaOfEffectPct;
            gear.incAttackAreaOfEffectPct += s.incAttackAreaOfEffectPct;
            gear.incProjectileDamagePct += s.incProjectileDamagePct;
            gear.incChaosDamagePct += s.incChaosDamagePct;
            gear.spellCritMultiPct += s.spellCritMultiPct;
            gear.meleeCritMultiplierPct += s.meleeCritMultiplierPct;
            gear.elementalCritMultiPct += s.elementalCritMultiPct;
            gear.incDamageVsRarePct += s.incDamageVsRarePct;
            gear.esRecoupPct += s.esRecoupPct;
            gear.itemRarityPct += s.itemRarityPct;
            gear.reducedDamageOnLowLifePct += s.reducedDamageOnLowLifePct;
            gear.incEvasionOnLowLifePct += s.incEvasionOnLowLifePct;
            gear.flatPhysTakenFromAttacksDelta += s.flatPhysTakenFromAttacksDelta;

            gear.cannotBeFrozen = gear.cannotBeFrozen || s.cannotBeFrozen;
            gear.shavronneChaosEs = gear.shavronneChaosEs || s.shavronneChaosEs;
            gear.starforgeShock = gear.starforgeShock || s.starforgeShock;
            gear.dealNoElemental = gear.dealNoElemental || s.dealNoElemental;
            gear.berekProlif = gear.berekProlif || s.berekProlif;
            gear.headhunter = gear.headhunter || s.headhunter;
        }

        // 5단계: 활성 오라·전령·헤드헌터 동적 버프 합산
        struct AuraAndBuffTotals
        {
            double flatArmour = 0.0;
            double incArmourPct = 0.0;
            double moreArmourMult = 1.0;
            double flatEvasion = 0.0;
            double moreEvasionMult = 1.0;
            double flatEs = 0.0;
            double incEsPct = 0.0;
            double incEsRechargeRatePct = 0.0;

            double allElemResist = 0.0;
            double fireResist = 0.0;
            double coldResist = 0.0;
            double lightningResist = 0.0;
            double chaosResist = 0.0;
            double physDamageReduction = 0.0;

            double addedAttackFireMin = 0.0;
            double addedAttackFireMax = 0.0;
            double addedAttackColdMin = 0.0;
            double addedAttackColdMax = 0.0;
            double addedAttackLightMin = 0.0;
            double addedAttackLightMax = 0.0;

            double addedSpellFireMin = 0.0;
            double addedSpellFireMax = 0.0;
            double addedSpellColdMin = 0.0;
            double addedSpellColdMax = 0.0;

            double physAddedAsFirePct = 0.0;
            double physAddedAsColdPct = 0.0;
            double physAddedAsLightPct = 0.0;
            double moreSpellLightMult = 1.0;

            double incPhysicalDamagePct = 0.0;
            double incAttackSpeedPct = 0.0;
            double incCastSpeedPct = 0.0;
            double incMovementSpeedPct = 0.0;
            double incGlobalCritChancePct = 0.0;
            double globalCritMultiplierPct = 0.0;

            bool elementalAilmentImmune = false;
        };

        [[nodiscard]] bool MeetsGemRequirements(const GemLevelData& lv, const StatSnapshot& snap) noexcept
        {
            return snap.exileLevel >= lv.levelRequirement &&
                   snap.str >= lv.reqStr &&
                   snap.dex >= lv.reqDex &&
                   snap.intel >= lv.reqInt;
        }

        ComputedSkillStats ComputeSingleSkill(
            const GemInstance& gemInst,
            const DataPack& pack,
            const Config& config,
            const Layer1Input& l1,
            const Layer2Input& l2,
            const StatSnapshot& snap,
            const AggregatedGearStats& gear,
            const AuraAndBuffTotals& aura,
            double strMeleePhysIncPct,
            double armourScaleMult,
            double extraManaMultPct,
            double extraMoreSpellDmgPct,
            bool isTriggered,
            double triggerMinCooldown,
            bool auraIsActive = false,
            const std::unordered_set<std::string>* initialExtraTypes = nullptr)
        {
            ComputedSkillStats out;
            out.gemInstanceId = gemInst.instanceId;
            out.gemId = gemInst.gemId;
            out.level = gemInst.level;
            out.isTriggered = isTriggered;

            const auto* def = pack.FindGem(gemInst.gemId);
            if (!def) {
                return out;
            }
            out.kind = def->kind;
            const auto* lv = def->GetLevel(gemInst.level);
            if (!lv) {
                return out;
            }
            out.requirementMet = MeetsGemRequirements(*lv, snap);
            out.spiritCost = (lv->spiritCost > 0) ? lv->spiritCost : def->spiritCost;
            out.auraActive = auraIsActive;

            const auto& p = l1.passives;
            const std::uint8_t clampedFrenzy = std::min(l2.frenzyCharges, snap.maxFrenzyCharges);
            const std::uint8_t clampedPower = std::min(l2.powerCharges, snap.maxPowerCharges);

            double manaMult = 1.0 + extraManaMultPct / 100.0;
            double morePhysMult = 1.0;
            double moreAreaDmgMult = 1.0;
            double moreProjDmgMult = 1.0;
            double moreHitDmgMult = 1.0;
            double moreSpellDmgMult = 1.0 + extraMoreSpellDmgPct / 100.0;
            // Pain Attunement: 저체력(CI 제외) 시 주문 피해 30% 증폭
            if (!snap.chaosInoculation && p.painAttunement && l2.lowLife) {
                moreSpellDmgMult *= 1.30;
            }
            double moreAllDmgMult = 1.0 + (clampedFrenzy * kFrenzySpeedAndMore) / 100.0;
            double moreAtkSpeedMult = 1.0;
            double moreCastSpeedMult = 1.0;
            double incAreaPct = p.incAreaOfEffectPct + gear.incAreaOfEffectPct;
            double moreAreaMult = 1.0;
            double addBaseCritPct = 0.0;
            double incCritPct = l1.incCritChancePct + p.incGlobalCritChancePct +
                gear.incGlobalCritChancePct + aura.incGlobalCritChancePct + clampedPower * kPowerCritInc;
            bool noElemental = gear.dealNoElemental;
            bool noChaos = false;

            std::int32_t projCount = std::max<std::int32_t>(1, static_cast<std::int32_t>(std::llround(lv->GetStat("base_number_of_projectiles", 1.0))));
            std::int32_t chainCount = static_cast<std::int32_t>(std::llround(lv->GetStat("number_of_chains", 0.0)));
            bool forkEnabled = false;
            std::int32_t pierceCount = static_cast<std::int32_t>(std::llround(lv->GetStat("projectile_base_number_of_targets_to_pierce", 0.0)));
            bool alwaysPierce = lv->GetStat("always_pierce", 0.0) > 0.0;
            std::int32_t repeatCount = 0;

            // 소켓 수 상한 내에서 동일 보조 젬(gemId)은 최고 레벨 1개만 적용(6.1 규칙)
            // 검수 C3: config.enforceGemRequirements가 참이면 요구치 미달 보조 젬을 먼저 제외한다.
            const std::size_t maxSockets = std::min<std::size_t>(gemInst.socketedGems.size(), gemInst.socketCount);
            std::unordered_map<std::string, const GemInstance*> dedupSupports;
            std::vector<std::string> dedupOrder;
            for (std::size_t s = 0; s < maxSockets; ++s) {
                const auto& supInst = gemInst.socketedGems[s];
                if (config.enforceGemRequirements) {
                    const auto* sDef = pack.FindGem(supInst.gemId);
                    const auto* sLv = sDef ? sDef->GetLevel(supInst.level) : nullptr;
                    if (!sLv || !MeetsGemRequirements(*sLv, snap)) {
                        continue;
                    }
                }
                auto it = dedupSupports.find(supInst.gemId);
                if (it == dedupSupports.end()) {
                    dedupSupports[supInst.gemId] = &supInst;
                    dedupOrder.push_back(supInst.gemId);
                } else if (supInst.level > it->second->level) {
                    it->second = &supInst;
                }
            }

            // 보조 젬이 추가하는 스킬 유형(addSkillTypes)을 누적 반영
            std::unordered_set<std::string> extraSkillTypes;
            if (initialExtraTypes) {
                extraSkillTypes = *initialExtraTypes;
            }
            for (const auto& supId : dedupOrder) {
                const auto* supDef = pack.FindGem(supId);
                if (supDef && supDef->isSupport && !supDef->addSkillTypes.empty()) {
                    for (const auto& t : supDef->addSkillTypes) {
                        extraSkillTypes.insert(t);
                    }
                }
            }

            // 보조 젬 적용(호환되는 보조 젬만 반영, 6.1)
            for (const auto& supId : dedupOrder) {
                const auto& supInst = *dedupSupports[supId];
                if (!pack.CanSupportApply(supInst.gemId, gemInst.gemId,
                        extraSkillTypes.empty() ? nullptr : &extraSkillTypes)) {
                    continue;
                }
                const auto* supDef = pack.FindGem(supInst.gemId);
                const auto* supLv = supDef ? supDef->GetLevel(supInst.level) : nullptr;
                if (!supLv) {
                    continue;
                }
                if (supLv->manaMultiplier != 0.0) {
                    manaMult *= (1.0 + supLv->manaMultiplier / 100.0);
                }

                // 치명타 확률 증가: additional_base_critical_strike_chance(100분의 1%) + critical_strike_chance_+%
                addBaseCritPct += supLv->GetStat("additional_base_critical_strike_chance", 0.0) / 100.0;
                incCritPct += supLv->GetStat("critical_strike_chance_+%", 0.0);

                // 근접 물리 피해
                if (const double v = supLv->GetStat("support_melee_physical_damage_+%_final", 0.0); v != 0.0) {
                    morePhysMult *= (1.0 + v / 100.0);
                }
                if (const double v = supLv->GetStat("support_melee_physical_damage_attack_speed_+%_final", 0.0); v != 0.0) {
                    moreAtkSpeedMult *= (1.0 + v / 100.0);
                }

                // 포악함
                if (const double v = supLv->GetStat("support_brutality_physical_damage_+%_final", 0.0); v != 0.0) {
                    morePhysMult *= (1.0 + v / 100.0);
                }
                if (supLv->GetStat("deal_no_elemental_damage", 0.0) > 0.0) {
                    noElemental = true;
                }
                if (supLv->GetStat("base_deal_no_chaos_damage", 0.0) > 0.0) {
                    noChaos = true;
                }

                // 효과 범위 증가 / 집중 효과
                incAreaPct += supLv->GetStat("base_skill_area_of_effect_+%", 0.0);
                if (const double v = supLv->GetStat("support_concentrated_effect_skill_area_of_effect_+%_final", 0.0); v != 0.0) {
                    moreAreaMult *= (1.0 + v / 100.0);
                }
                if (const double v = supLv->GetStat("support_area_concentrate_area_damage_+%_final", 0.0); v != 0.0) {
                    moreAreaDmgMult *= (1.0 + v / 100.0);
                }

                // 5단계 보조 젬: 상위 다중 투사체 / 연쇄 / 분기 / 관통 / 주문 메아리
                projCount += static_cast<std::int32_t>(std::llround(supLv->GetStat("number_of_additional_projectiles", 0.0)));
                if (const double v = supLv->GetStat("support_multiple_projectile_damage_+%_final", 0.0); v != 0.0) {
                    moreProjDmgMult *= (1.0 + v / 100.0);
                }
                chainCount += static_cast<std::int32_t>(std::llround(supLv->GetStat("number_of_chains", 0.0)));
                if (const double v = supLv->GetStat("support_chain_hit_damage_+%_final", 0.0); v != 0.0) {
                    moreHitDmgMult *= (1.0 + v / 100.0);
                }
                if (supLv->GetStat("projectiles_fork", 0.0) > 0.0) {
                    forkEnabled = true;
                }
                if (const double v = supLv->GetStat("support_fork_projectile_damage_+%_final", 0.0); v != 0.0) {
                    moreProjDmgMult *= (1.0 + v / 100.0);
                }
                pierceCount += static_cast<std::int32_t>(std::llround(supLv->GetStat("projectile_base_number_of_targets_to_pierce", 0.0)));
                if (const double v = supLv->GetStat("support_pierce_projectile_damage_+%_final", 0.0); v != 0.0) {
                    moreProjDmgMult *= (1.0 + v / 100.0);
                }
                repeatCount += static_cast<std::int32_t>(std::llround(supLv->GetStat("base_spell_repeat_count", 0.0)));
                if (const double v = supLv->GetStat("support_multicast_cast_speed_+%_final", 0.0); v != 0.0) {
                    moreCastSpeedMult *= (1.0 + v / 100.0);
                }
                if (const double v = supLv->GetStat("support_echo_damage_+%_final", 0.0); v != 0.0) {
                    moreSpellDmgMult *= (1.0 + v / 100.0);
                }
            }

            const bool isArea = def->skillTypes.contains("Area") || extraSkillTypes.contains("Area");
            const bool isProj = def->skillTypes.contains("Projectile") || extraSkillTypes.contains("Projectile") ||
                lv->GetStat("base_is_projectile", 0.0) > 0.0;
            const double areaDmgFactor = isArea ? moreAreaDmgMult : 1.0;
            const double projDmgFactor = isProj ? moreProjDmgMult : 1.0;
            const double areaIncPct = isArea ? (p.incAreaDamagePct + gear.incAreaDamagePct) : 0.0;
            const double projIncPct = isProj ? gear.incProjectileDamagePct : 0.0;

            double totalCostEffPct = p.costEfficiencyPct;
            const double eoElemMoreMult = (p.elementalOverload && l2.elementalOverloadActive) ? 1.40 : 1.0;

            if (def->kind == GemKind::AttackChannel) {
                incAreaPct += gear.incAttackAreaOfEffectPct;
                // 검수 m1: 4~5단계 무기 로컬 + 글로벌/오라 추가 5속성 공격 피해 통합 + 물리→원소 추가(Hatred/HeraldOfAsh/Headhunter)
                const double basePhysMin = gear.hasActiveWeapon ? gear.weaponPhysMin : l1.baseWeaponPhysicalMin;
                const double basePhysMax = gear.hasActiveWeapon ? gear.weaponPhysMax : l1.baseWeaponPhysicalMax;
                const double baseCrit = gear.hasActiveWeapon ? gear.weaponCritChancePct : l1.baseWeaponCritChancePct;
                const double localSpdMult = gear.hasActiveWeapon ? gear.weaponLocalSpeedMult : 1.0;

                const double rawPhysMin = basePhysMin * lv->baseMultiplier + gear.addedAttackPhysMin * lv->damageEffectiveness;
                const double rawPhysMax = basePhysMax * lv->baseMultiplier + gear.addedAttackPhysMax * lv->damageEffectiveness;

                const double lowLifeDmgPenalty = (!snap.chaosInoculation && l2.lowLife) ? gear.reducedDamageOnLowLifePct : 0.0;
                const double commonAtkInc = p.incGlobalDamagePct + gear.incGenericDamagePct - lowLifeDmgPenalty +
                    p.incMeleeDamagePct + gear.incMeleeDamagePct +
                    p.incAttackDamagePct + areaIncPct + projIncPct;
                const double commonElemAtkInc = commonAtkInc + p.incElementalDamagePct + gear.incElementalDamagePct +
                    p.incElementalAttackDamagePct + gear.incElementalAttackDamagePct;
                const double atkCommonMore = areaDmgFactor * projDmgFactor * moreHitDmgMult * moreAllDmgMult;

                // 검수 M1: PoE1 Gain-as-Extra 규칙 — 물리에서 추가 획득된 원소 피해는 물리 %inc·%more와 원소 %inc·%more를 모두 상속
                const double physOnlyInc = p.incPhysicalDamagePct + gear.incPhysicalDamagePct +
                    aura.incPhysicalDamagePct + p.incPhysicalAttackDamagePct +
                    p.incMeleePhysicalDamagePct + gear.incMeleePhysicalDamagePct + strMeleePhysIncPct;

                auto computeAtkDirectAndGained = [&](DamageType dt, double directMin, double directMax,
                                                     double gainedPctOfPhys, double elemTotalInc, double elemMore) {
                    const double directIncMult = std::max(0.0, 1.0 + elemTotalInc / 100.0);
                    const double directFactor = directIncMult * elemMore * atkCommonMore;
                    double outMin = directMin * directFactor;
                    double outMax = directMax * directFactor;
                    if (gainedPctOfPhys > 0.0 && (rawPhysMin > 0.0 || rawPhysMax > 0.0)) {
                        const double gainedIncMult = std::max(0.0, 1.0 + (elemTotalInc + physOnlyInc) / 100.0);
                        const double gainedFactor = gainedIncMult * morePhysMult * elemMore * atkCommonMore * (gainedPctOfPhys / 100.0);
                        outMin += rawPhysMin * gainedFactor;
                        outMax += rawPhysMax * gainedFactor;
                    }
                    if (outMin > 0.0 || outMax > 0.0) {
                        out.baseHitDamage.byType[static_cast<std::size_t>(dt)] = DamageRange{ outMin, outMax };
                    }
                };

                computeAtkDirectAndGained(DamageType::Physical, rawPhysMin, rawPhysMax, 0.0,
                    commonAtkInc + physOnlyInc, morePhysMult);

                computeAtkDirectAndGained(DamageType::Fire,
                    gear.weaponFireMin * lv->baseMultiplier + (gear.addedAttackFireMin + aura.addedAttackFireMin) * lv->damageEffectiveness,
                    gear.weaponFireMax * lv->baseMultiplier + (gear.addedAttackFireMax + aura.addedAttackFireMax) * lv->damageEffectiveness,
                    aura.physAddedAsFirePct,
                    commonElemAtkInc + p.incFireDamagePct + gear.incFireDamagePct, eoElemMoreMult);

                computeAtkDirectAndGained(DamageType::Cold,
                    gear.weaponColdMin * lv->baseMultiplier + (gear.addedAttackColdMin + aura.addedAttackColdMin) * lv->damageEffectiveness,
                    gear.weaponColdMax * lv->baseMultiplier + (gear.addedAttackColdMax + aura.addedAttackColdMax) * lv->damageEffectiveness,
                    aura.physAddedAsColdPct,
                    commonElemAtkInc + p.incColdDamagePct + gear.incColdDamagePct, eoElemMoreMult);

                computeAtkDirectAndGained(DamageType::Lightning,
                    gear.weaponLightningMin * lv->baseMultiplier + (gear.addedAttackLightningMin + aura.addedAttackLightMin) * lv->damageEffectiveness,
                    gear.weaponLightningMax * lv->baseMultiplier + (gear.addedAttackLightningMax + aura.addedAttackLightMax) * lv->damageEffectiveness,
                    aura.physAddedAsLightPct,
                    commonElemAtkInc + p.incLightningDamagePct + gear.incLightningDamagePct, eoElemMoreMult);

                computeAtkDirectAndGained(DamageType::Chaos,
                    gear.weaponChaosMin * lv->baseMultiplier + gear.addedAttackChaosMin * lv->damageEffectiveness,
                    gear.weaponChaosMax * lv->baseMultiplier + gear.addedAttackChaosMax * lv->damageEffectiveness,
                    0.0, commonAtkInc + p.incChaosDamagePct + gear.incChaosDamagePct, 1.0);

                // 결정 50, 98, 검수 m1/M3: 무기 로컬 공속 + PoE1 쌍수 보너스(+10% 증폭 공속) 정확히 1회 적용
                const double skillSpeedBase = 1.0 + lv->attackSpeedMultiplier / 100.0;
                const double dualWieldMoreSpd = gear.isDualWielding ? 1.10 : 1.0;
                const double incSpeed = std::max(0.1,
                    1.0 + (l1.incAttackSpeedPct + p.incAttackSpeedPct + p.incMeleeAttackSpeedPct +
                           gear.incAttackSpeedPct + aura.incAttackSpeedPct + clampedFrenzy * kFrenzySpeedAndMore) / 100.0);
                out.attackSpeedMult = std::max(0.1, skillSpeedBase * localSpdMult * dualWieldMoreSpd * incSpeed * moreAtkSpeedMult);
                incCritPct += p.incAttackCritChancePct + p.incMeleeCritChancePct;
                // Resolute Technique: PoE 3.30 원작 규칙(공격·주문 모두 치명타 불가)
                out.baseCritChancePct = snap.resoluteTechnique ? 0.0 : (baseCrit + addBaseCritPct);
                out.critMultiplierPct = p.elementalOverload
                    ? 100.0
                    : std::max(100.0, l1.critMultiplierPct + p.globalCritMultiplierPct +
                                      p.meleeCritMultiplierPct + gear.meleeCritMultiplierPct +
                                      gear.globalCritMultiplierPct + aura.globalCritMultiplierPct);
                totalCostEffPct += p.attackCostEfficiencyPct;
            } else if (def->kind == GemKind::Spell) {
                const double ironWillBonus = p.ironWill ? strMeleePhysIncPct : 0.0;
                const double lowLifeDmgPenalty = (!snap.chaosInoculation && l2.lowLife) ? gear.reducedDamageOnLowLifePct : 0.0;
                const double commonSpellInc = p.incGlobalDamagePct + gear.incGenericDamagePct - lowLifeDmgPenalty +
                    p.incSpellDamagePct + gear.incSpellDamagePct + areaIncPct + projIncPct + ironWillBonus;
                const double spellScale = moreSpellDmgMult * areaDmgFactor * projDmgFactor * moreHitDmgMult * moreAllDmgMult;

                const double baseSpellPhysMin = lv->GetStat("spell_minimum_base_physical_damage", 0.0);
                const double baseSpellPhysMax = lv->GetStat("spell_maximum_base_physical_damage", 0.0);
                const double spellPhysOnlyInc = p.incPhysicalDamagePct + gear.incPhysicalDamagePct + aura.incPhysicalDamagePct;

                auto applySpellType = [&](DamageType dt, double directMin, double directMax,
                                          double gainedPctOfPhys, double typeMore, double typeInc) {
                    const double directIncMult = std::max(0.0, 1.0 + (commonSpellInc + typeInc) / 100.0);
                    double outMin = directMin * spellScale * typeMore * directIncMult;
                    double outMax = directMax * spellScale * typeMore * directIncMult;
                    if (gainedPctOfPhys > 0.0 && (baseSpellPhysMin > 0.0 || baseSpellPhysMax > 0.0)) {
                        const double gainedIncMult = std::max(0.0, 1.0 + (commonSpellInc + typeInc + spellPhysOnlyInc) / 100.0);
                        const double gainedFactor = spellScale * morePhysMult * typeMore * gainedIncMult * (gainedPctOfPhys / 100.0);
                        outMin += baseSpellPhysMin * gainedFactor;
                        outMax += baseSpellPhysMax * gainedFactor;
                    }
                    if (outMin > 0.0 || outMax > 0.0) {
                        out.baseHitDamage.byType[static_cast<std::size_t>(dt)] = DamageRange{ outMin, outMax };
                    }
                };
                applySpellType(DamageType::Physical, baseSpellPhysMin, baseSpellPhysMax, 0.0,
                    morePhysMult, spellPhysOnlyInc);
                applySpellType(DamageType::Fire,
                    lv->GetStat("spell_minimum_base_fire_damage", 0.0) +
                        (gear.addedSpellFireMin + aura.addedSpellFireMin) * lv->damageEffectiveness,
                    lv->GetStat("spell_maximum_base_fire_damage", 0.0) +
                        (gear.addedSpellFireMax + aura.addedSpellFireMax) * lv->damageEffectiveness,
                    aura.physAddedAsFirePct,
                    eoElemMoreMult, p.incElementalDamagePct + gear.incElementalDamagePct + p.incFireDamagePct + gear.incFireDamagePct);
                applySpellType(DamageType::Cold,
                    lv->GetStat("spell_minimum_base_cold_damage", 0.0) +
                        (gear.addedSpellColdMin + aura.addedSpellColdMin) * lv->damageEffectiveness,
                    lv->GetStat("spell_maximum_base_cold_damage", 0.0) +
                        (gear.addedSpellColdMax + aura.addedSpellColdMax) * lv->damageEffectiveness,
                    aura.physAddedAsColdPct,
                    eoElemMoreMult, p.incElementalDamagePct + gear.incElementalDamagePct + p.incColdDamagePct + gear.incColdDamagePct);
                applySpellType(DamageType::Lightning,
                    lv->GetStat("spell_minimum_base_lightning_damage", 0.0) + gear.addedSpellLightningMin * lv->damageEffectiveness,
                    lv->GetStat("spell_maximum_base_lightning_damage", 0.0) + gear.addedSpellLightningMax * lv->damageEffectiveness,
                    aura.physAddedAsLightPct,
                    eoElemMoreMult * aura.moreSpellLightMult,
                    p.incElementalDamagePct + gear.incElementalDamagePct + p.incLightningDamagePct + gear.incLightningDamagePct);
                applySpellType(DamageType::Chaos,
                    lv->GetStat("spell_minimum_base_chaos_damage", 0.0) + gear.addedSpellChaosMin * lv->damageEffectiveness,
                    lv->GetStat("spell_maximum_base_chaos_damage", 0.0) + gear.addedSpellChaosMax * lv->damageEffectiveness,
                    0.0, 1.0, p.incChaosDamagePct + gear.incChaosDamagePct);

                out.castSpeedMult = std::max(0.1,
                    (1.0 + (p.incCastSpeedPct + gear.incCastSpeedPct + aura.incCastSpeedPct +
                            clampedFrenzy * kFrenzySpeedAndMore) / 100.0) * moreCastSpeedMult);
                incCritPct += p.incSpellCritChancePct + gear.incSpellCritChancePct;
                out.baseCritChancePct = snap.resoluteTechnique ? 0.0 : (lv->critChance + addBaseCritPct);
                const bool isElemSpell = def->skillTypes.contains("Fire") || def->skillTypes.contains("Cold") || def->skillTypes.contains("Lightning");
                out.critMultiplierPct = p.elementalOverload
                    ? 100.0
                    : std::max(100.0, l1.critMultiplierPct + p.globalCritMultiplierPct +
                                      p.spellCritMultiplierPct + gear.globalCritMultiplierPct +
                                      gear.spellCritMultiPct + (isElemSpell ? gear.elementalCritMultiPct : 0.0) +
                                      aura.globalCritMultiplierPct);
                totalCostEffPct += p.spellCostEfficiencyPct;
            } else if (def->kind == GemKind::Guard) {
                // 5단계(결정 163, 검수 M3): Molten Shell 가드 버프 — 자체 고정 방어도에도 방어도 %inc·Determination MORE 적용
                out.moltenShellFlatArmour = lv->GetStat("base_physical_damage_reduction_rating", 115.0);
                out.moltenShellAbsorbPct = lv->GetStat("molten_shell_damage_absorbed_%", 75.0);
                const double pctOfArmour = lv->GetStat("molten_shell_damage_absorb_limit_%_of_armour", 10.0);
                const double maxCap = lv->GetStat("molten_shell_max_damage_absorbed", 5000.0);
                const double armourForShell = std::max(0.0, snap.armour + (out.moltenShellFlatArmour - l2.moltenShellBonusArmour) * armourScaleMult);
                out.moltenShellAbsorbCapPoe = std::clamp(armourForShell * (pctOfArmour / 100.0), 1.0, maxCap);
                out.moltenShellDurationSec = std::max(0.5, lv->GetStat("base_skill_effect_duration", 3000.0) / 1000.0);
                out.moltenShellReflectPct = lv->GetStat("molten_shell_%_of_absorbed_damage_dealt_as_reflected_fire", 100.0);
                out.critMultiplierPct = l1.critMultiplierPct + gear.globalCritMultiplierPct;
            } else {
                out.critMultiplierPct = l1.critMultiplierPct + gear.globalCritMultiplierPct;
            }

            if (noElemental) {
                out.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Fire)] = {};
                out.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Cold)] = {};
                out.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Lightning)] = {};
            }
            if (noChaos) {
                out.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Chaos)] = {};
            }

            out.dealNoElemental = noElemental;
            out.dealNoChaos = noChaos;
            out.critChanceIncPct = incCritPct;
            out.projectileCount = static_cast<std::uint8_t>(std::clamp<std::int32_t>(projCount, 1, 16));
            out.chainCount = static_cast<std::uint8_t>(std::clamp<std::int32_t>(chainCount, 0, 16));
            out.forkEnabled = forkEnabled;
            out.pierceCount = static_cast<std::uint8_t>(std::clamp<std::int32_t>(pierceCount, 0, 16));
            out.alwaysPierce = alwaysPierce;
            out.spellRepeatCount = static_cast<std::uint8_t>(std::clamp<std::int32_t>(repeatCount, 0, 4));
            out.igniteChancePct = lv->GetStat("base_chance_to_ignite_%", 0.0);
            out.arcRemainingChainMorePct = lv->GetStat("arc_damage_+%_final_for_each_remaining_chain", 0.0);

            const double totalAreaFactor = std::max(0.05, (1.0 + incAreaPct / 100.0) * moreAreaMult);
            out.areaMult = std::sqrt(totalAreaFactor);
            const double reducedCostFactor = std::clamp(1.0 - p.reducedManaCostPct / 100.0, 0.0, 10.0);
            const double costEffDiv = std::max(0.1, 1.0 + totalCostEffPct / 100.0);
            const double rawManaCost = lv->manaCost * manaMult * reducedCostFactor / costEffDiv;
            out.poeManaCost = (lv->manaCost > 0.0)
                ? std::max(0.0, rawManaCost + snap.flatManaCostDelta)
                : 0.0;
            const double baseCd = (def->kind == GemKind::Guard) ? std::max(lv->cooldown, 4.0) : lv->cooldown;
            out.cooldownSec = std::max(baseCd, triggerMinCooldown);
            return out;
        }
    }

    StatEngine::StatEngine()
        : snapshot_(std::make_shared<const StatSnapshot>())
    {
    }

    void StatEngine::SetLayer1(const Layer1Input& input)
    {
        {
            std::scoped_lock lock(inputMutex_);
            layer1_ = input;
        }
        dirty_.store(true, std::memory_order_release);
    }

    void StatEngine::SetLayer2(const Layer2Input& input)
    {
        {
            std::scoped_lock lock(inputMutex_);
            layer2_ = input;
        }
        dirty_.store(true, std::memory_order_release);
    }

    void StatEngine::UpdateLayer2(const std::function<void(Layer2Input&)>& mutator)
    {
        if (!mutator) {
            return;
        }
        {
            std::scoped_lock lock(inputMutex_);
            mutator(layer2_);
        }
        dirty_.store(true, std::memory_order_release);
    }

    void StatEngine::MarkDirty() noexcept
    {
        dirty_.store(true, std::memory_order_release);
    }

    bool StatEngine::RebuildIfDirty(const DataPack& pack, const Config& config)
    {
        if (!dirty_.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }

        Layer1Input l1;
        Layer2Input l2;
        {
            std::scoped_lock lock(inputMutex_);
            l1 = layer1_;
            l2 = layer2_;
        }

        const auto ver = nextVersion_++;
        auto next = std::make_shared<const StatSnapshot>(ComputeSnapshotLocked(l1, l2, pack, config, ver));
        {
            std::scoped_lock lock(snapshotMutex_);
            snapshot_ = std::move(next);
        }
        return true;
    }

    std::shared_ptr<const StatSnapshot> StatEngine::GetSnapshot() const
    {
        std::scoped_lock lock(snapshotMutex_);
        return snapshot_;
    }

    StatSnapshot StatEngine::ComputeSnapshotLocked(
        const Layer1Input& l1,
        const Layer2Input& l2,
        const DataPack& pack,
        const Config& config,
        std::uint64_t nextVersion) const
    {
        StatSnapshot snap;
        snap.version = nextVersion;
        snap.exileLevel = std::clamp<std::uint32_t>(l1.exileLevel, 1, kMaxExileLevel);

        const auto& p = l1.passives;
        const auto* cls = pack.FindClass(l1.className);
        const double baseStr = cls ? static_cast<double>(cls->baseStr) : 20.0;
        const double baseDex = cls ? static_cast<double>(cls->baseDex) : 20.0;
        const double baseInt = cls ? static_cast<double>(cls->baseInt) : 20.0;

        // 5단계(결정 164): 패시브 주얼 소켓 능력치는 10칸 장비 고정점 루프 진입 전에 먼저 합산한다.
        double jewelFlatStr = 0.0, jewelFlatDex = 0.0, jewelFlatInt = 0.0;
        for (const auto& jw : l1.socketedJewelStats) {
            jewelFlatStr += jw.flatStr;
            jewelFlatDex += jw.flatDex;
            jewelFlatInt += jw.flatInt;
        }

        // 4~5단계(결정 156, 162): 고정점(Fixed-Point) 능력치 요구치 수렴(최대 10회 반복).
        std::array<bool, kEquipSlotCount> gearActive{};
        std::array<bool, kEquipSlotCount> gearReqFailed{};

        auto computeAttrs = [&](const std::array<bool, kEquipSlotCount>& activeMask)
            -> std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>
        {
            double gStr = jewelFlatStr, gDex = jewelFlatDex, gInt = jewelFlatInt;
            for (std::size_t i = 0; i < kEquipSlotCount; ++i) {
                if (activeMask[i]) {
                    gStr += l1.equippedGear[i].stats.flatStr;
                    gDex += l1.equippedGear[i].stats.flatDex;
                    gInt += l1.equippedGear[i].stats.flatInt;
                }
            }
            const double fStr = std::max(0.0, (baseStr + p.flatStr + gStr) * std::max(0.0, 1.0 + p.incStrPct / 100.0));
            const double fDex = std::max(0.0, (baseDex + p.flatDex + gDex) * std::max(0.0, 1.0 + p.incDexPct / 100.0));
            const double fInt = std::max(0.0, (baseInt + p.flatInt + gInt) * std::max(0.0, 1.0 + p.incIntPct / 100.0));
            return {
                static_cast<std::uint32_t>(std::floor(fStr + 1e-6)),
                static_cast<std::uint32_t>(std::floor(fDex + 1e-6)),
                static_cast<std::uint32_t>(std::floor(fInt + 1e-6))
            };
        };

        for (std::size_t iter = 0; iter < kEquipSlotCount; ++iter) {
            const auto [curStr, curDex, curInt] = computeAttrs(gearActive);
            std::array<bool, kEquipSlotCount> nextActive{};
            std::array<bool, kEquipSlotCount> nextReqFailed{};
            for (std::size_t i = 0; i < kEquipSlotCount; ++i) {
                const auto& g = l1.equippedGear[i];
                if (!g.occupied) {
                    continue;
                }
                if (!g.isCrafted) {
                    // 검수 C3: 미제작 바닐라/모드 장비는 요구치 0(항상 활성)
                    nextActive[i] = true;
                    continue;
                }
                const bool meets = (snap.exileLevel >= g.reqLevel &&
                                    curStr >= g.reqStr &&
                                    curDex >= g.reqDex &&
                                    curInt >= g.reqInt);
                nextActive[i] = meets;
                nextReqFailed[i] = !meets;
            }
            gearReqFailed = nextReqFailed;
            if (nextActive == gearActive) {
                break;
            }
            gearActive = nextActive;
        }

        const auto [finalStr, finalDex, finalInt] = computeAttrs(gearActive);
        snap.str = finalStr;
        snap.dex = finalDex;
        snap.intel = finalInt;
        snap.gearSlotActive = gearActive;
        snap.gearSlotReqFailed = gearReqFailed;

        // 활성 8칸 장비의 로컬 + 글로벌 스탯 합산 (검수 M3: 쌍수 평균 + 주무기 비활성 시 보조무기 폴백 + PoE1 쌍수 +20% 막기/+10% 공속 증폭)
        AggregatedGearStats gear{};
        struct LocalWeaponEval
        {
            bool valid = false;
            double physMin = 0.0, physMax = 0.0;
            double fireMin = 0.0, fireMax = 0.0;
            double coldMin = 0.0, coldMax = 0.0;
            double lightMin = 0.0, lightMax = 0.0;
            double chaosMin = 0.0, chaosMax = 0.0;
            double critPct = 5.0;
            double speedMult = 1.0;
        };
        auto evalWeapon = [](const EquippedSlotInput& g) -> LocalWeaponEval {
            LocalWeaponEval w;
            w.valid = true;
            const auto& s = g.stats;
            const double localPhysMult = std::max(0.0, 1.0 + s.localIncPhysPct / 100.0);
            w.physMin = std::max(0.0, (g.baseWeaponPhysMin + s.localFlatPhysMin) * localPhysMult);
            w.physMax = std::max(w.physMin, (g.baseWeaponPhysMax + s.localFlatPhysMax) * localPhysMult);
            w.fireMin = std::max(0.0, s.localFlatFireMin);
            w.fireMax = std::max(w.fireMin, s.localFlatFireMax);
            w.coldMin = std::max(0.0, s.localFlatColdMin);
            w.coldMax = std::max(w.coldMin, s.localFlatColdMax);
            w.lightMin = std::max(0.0, s.localFlatLightningMin);
            w.lightMax = std::max(w.lightMin, s.localFlatLightningMax);
            w.chaosMin = std::max(0.0, s.localFlatChaosMin);
            w.chaosMax = std::max(w.chaosMin, s.localFlatChaosMax);
            w.critPct = std::max(0.0, g.baseWeaponCritChancePct * std::max(0.0, 1.0 + s.localIncCritChancePct / 100.0));
            w.speedMult = std::max(0.1, 1.0 + s.localIncAttackSpeedPct / 100.0);
            return w;
        };

        // 패시브 주얼 소켓 글로벌 스탯 합산
        for (const auto& jw : l1.socketedJewelStats) {
            AccumulateGlobalItemStats(gear, jw);
        }

        LocalWeaponEval mhEval{}, ohEval{};
        for (std::size_t i = 0; i < kEquipSlotCount; ++i) {
            if (!gearActive[i]) {
                continue;
            }
            const auto& g = l1.equippedGear[i];
            const auto& s = g.stats;

            if (g.isWeapon && i == static_cast<std::size_t>(EquipSlot::MainHand)) {
                mhEval = evalWeapon(g);
            } else if (g.isWeapon && i == static_cast<std::size_t>(EquipSlot::OffHand)) {
                ohEval = evalWeapon(g);
            } else if (!g.isWeapon) {
                gear.armour += std::max(0.0, (g.baseArmour + s.localFlatArmour) * std::max(0.0, 1.0 + s.localIncArmourPct / 100.0));
                gear.evasion += std::max(0.0, (g.baseEvasion + s.localFlatEvasion) * std::max(0.0, 1.0 + s.localIncEvasionPct / 100.0));
                gear.energyShield += std::max(0.0, (g.baseEnergyShield + s.localFlatEnergyShield) * std::max(0.0, 1.0 + s.localIncEnergyShieldPct / 100.0));
                gear.attackBlock += std::max(0.0, g.baseBlockChance * std::max(0.0, 1.0 + s.localIncBlockPct / 100.0));
            }

            // 글로벌 어픽스 합산(주무기·보조무기/방패·방어구·장신구·벨트 공통)
            AccumulateGlobalItemStats(gear, s);
        }

        if (mhEval.valid && ohEval.valid) {
            gear.hasActiveWeapon = true;
            gear.isDualWielding = true;
            gear.weaponPhysMin = 0.5 * (mhEval.physMin + ohEval.physMin);
            gear.weaponPhysMax = 0.5 * (mhEval.physMax + ohEval.physMax);
            gear.weaponFireMin = 0.5 * (mhEval.fireMin + ohEval.fireMin);
            gear.weaponFireMax = 0.5 * (mhEval.fireMax + ohEval.fireMax);
            gear.weaponColdMin = 0.5 * (mhEval.coldMin + ohEval.coldMin);
            gear.weaponColdMax = 0.5 * (mhEval.coldMax + ohEval.coldMax);
            gear.weaponLightningMin = 0.5 * (mhEval.lightMin + ohEval.lightMin);
            gear.weaponLightningMax = 0.5 * (mhEval.lightMax + ohEval.lightMax);
            gear.weaponChaosMin = 0.5 * (mhEval.chaosMin + ohEval.chaosMin);
            gear.weaponChaosMax = 0.5 * (mhEval.chaosMax + ohEval.chaosMax);
            gear.weaponCritChancePct = 0.5 * (mhEval.critPct + ohEval.critPct);
            gear.weaponLocalSpeedMult = 0.5 * (mhEval.speedMult + ohEval.speedMult);
            gear.attackBlock += 20.0;  // PoE1 쌍수 고유 보너스: 공격 막기 +20%
        } else if (mhEval.valid || ohEval.valid) {
            const auto& w = mhEval.valid ? mhEval : ohEval;
            gear.hasActiveWeapon = true;
            gear.weaponPhysMin = w.physMin;
            gear.weaponPhysMax = w.physMax;
            gear.weaponFireMin = w.fireMin;
            gear.weaponFireMax = w.fireMax;
            gear.weaponColdMin = w.coldMin;
            gear.weaponColdMax = w.coldMax;
            gear.weaponLightningMin = w.lightMin;
            gear.weaponLightningMax = w.lightMax;
            gear.weaponChaosMin = w.chaosMin;
            gear.weaponChaosMax = w.chaosMax;
            gear.weaponCritChancePct = w.critPct;
            gear.weaponLocalSpeedMult = w.speedMult;
        }

        // 6단계(결정 45, 166): 지도 인스턴스 전투 환산 레벨·모드 스냅샷 반영(생명/매지카 풀은 exileLevel 유지)
        snap.inActiveMap = l2.inActiveMap;
        snap.combatAreaLevel = (l2.inActiveMap && l2.combatAreaLevel > 0)
            ? std::clamp<std::uint32_t>(l2.combatAreaLevel, 1, kMaxExileLevel)
            : snap.exileLevel;
        snap.activeMapMods = l2.inActiveMap ? l2.activeMapMods : MapModSummary{};

        // 5~6단계(결정 161, 166): 정신력 상한 계산(1~4차 이정표 각 +30, 기본 최대 220) + 활성 오라/전령 중복 제거
        const std::uint32_t milestoneCount = std::min<std::uint32_t>(l1.completedMilestoneCount, 4u);
        const std::uint32_t gearSpirit = static_cast<std::uint32_t>(std::max(0.0, std::floor(gear.flatSpirit + 1e-6)));
        snap.maxSpirit = config.spiritBase + milestoneCount * config.spiritPerMilestone + gearSpirit;

        struct ActiveAuraPick
        {
            std::size_t slotIdx = 0;
            std::string gemId;
            std::uint8_t level = 1;
            std::uint32_t spiritCost = 0;
            const SkillEffectDefinition* def = nullptr;
            const GemLevelData* lv = nullptr;
        };
        std::unordered_map<std::string, ActiveAuraPick> bestByGemId;
        for (std::size_t i = 0; i < kSkillSlotCount; ++i) {
            if ((l1.activeAuraMask & static_cast<std::uint16_t>(1u << i)) == 0) continue;
            const auto& slot = l1.slots[i];
            if (!slot.occupied) continue;
            const auto* def = pack.FindGem(slot.gem.gemId);
            if (!def || (def->kind != GemKind::Aura && def->kind != GemKind::Herald)) continue;
            const auto* lv = def->GetLevel(slot.gem.level);
            if (!lv) continue;
            if (config.enforceGemRequirements && !MeetsGemRequirements(*lv, snap)) continue;
            const std::uint32_t cost = (lv->spiritCost > 0) ? lv->spiritCost : def->spiritCost;
            auto it = bestByGemId.find(def->id);
            if (it == bestByGemId.end() || slot.gem.level > it->second.level) {
                bestByGemId[def->id] = ActiveAuraPick{ i, def->id, slot.gem.level, cost, def, lv };
            }
        }

        std::vector<ActiveAuraPick> activeAuras;
        activeAuras.reserve(bestByGemId.size());
        std::uint32_t totalReserved = 0;
        for (const auto& [_, pick] : bestByGemId) {
            activeAuras.push_back(pick);
            totalReserved += pick.spiritCost;
        }
        std::sort(activeAuras.begin(), activeAuras.end(), [](const ActiveAuraPick& a, const ActiveAuraPick& b) {
            return a.slotIdx < b.slotIdx;
        });
        while (totalReserved > snap.maxSpirit && !activeAuras.empty()) {
            totalReserved -= activeAuras.back().spiritCost;
            activeAuras.pop_back();
        }

        AuraAndBuffTotals aura{};
        snap.reservedSpirit = totalReserved;
        snap.effectiveActiveAuraMask = 0;
        for (const auto& a : activeAuras) {
            snap.effectiveActiveAuraMask |= static_cast<std::uint16_t>(1u << a.slotIdx);
            const auto& lv = *a.lv;
            if (a.gemId == "Determination") {
                aura.flatArmour += lv.GetStat("base_physical_damage_reduction_rating", 0.0);
                if (const double v = lv.GetStat("determination_aura_armour_+%_final", 0.0); v != 0.0) {
                    aura.moreArmourMult *= (1.0 + v / 100.0);
                }
            } else if (a.gemId == "Grace") {
                aura.flatEvasion += lv.GetStat("base_evasion_rating", 0.0);
                if (const double v = lv.GetStat("grace_aura_evasion_rating_+%_final", 0.0); v != 0.0) {
                    aura.moreEvasionMult *= (1.0 + v / 100.0);
                }
            } else if (a.gemId == "Discipline") {
                aura.flatEs += lv.GetStat("base_maximum_energy_shield", 0.0);
                aura.incEsRechargeRatePct += lv.GetStat("energy_shield_recharge_rate_+%", 0.0);
            } else if (a.gemId == "Hatred") {
                aura.physAddedAsColdPct += lv.GetStat("physical_damage_%_to_add_as_cold", 0.0);
            } else if (a.gemId == "Wrath") {
                aura.addedAttackLightMin += lv.GetStat("attack_minimum_added_lightning_damage", 0.0);
                aura.addedAttackLightMax += lv.GetStat("attack_maximum_added_lightning_damage", 0.0);
                if (const double v = lv.GetStat("wrath_aura_spell_lightning_damage_+%_final", 0.0); v != 0.0) {
                    aura.moreSpellLightMult *= (1.0 + v / 100.0);
                }
            } else if (a.gemId == "Anger") {
                aura.addedAttackFireMin += lv.GetStat("attack_minimum_added_fire_damage", 0.0);
                aura.addedAttackFireMax += lv.GetStat("attack_maximum_added_fire_damage", 0.0);
                aura.addedSpellFireMin += lv.GetStat("spell_minimum_added_fire_damage", 0.0);
                aura.addedSpellFireMax += lv.GetStat("spell_maximum_added_fire_damage", 0.0);
            } else if (a.gemId == "PurityOfElements") {
                aura.allElemResist += lv.GetStat("base_resist_all_elements_%", 0.0);
                if (lv.GetStat("immune_to_status_ailments", 0.0) > 0.0) {
                    aura.elementalAilmentImmune = true;
                }
            } else if (a.gemId == "HeraldOfIce") {
                aura.addedAttackColdMin += lv.GetStat("attack_minimum_added_cold_damage", 0.0);
                aura.addedAttackColdMax += lv.GetStat("attack_maximum_added_cold_damage", 0.0);
                aura.addedSpellColdMin += lv.GetStat("spell_minimum_added_cold_damage", 0.0);
                aura.addedSpellColdMax += lv.GetStat("spell_maximum_added_cold_damage", 0.0);
                snap.heraldOfIceActive = true;
                snap.heraldOfIceExplosionMin = lv.GetStat("secondary_minimum_base_cold_damage", 18.0);
                snap.heraldOfIceExplosionMax = lv.GetStat("secondary_maximum_base_cold_damage", 26.0);
            } else if (a.gemId == "HeraldOfAsh") {
                aura.physAddedAsFirePct += lv.GetStat("physical_damage_%_to_add_as_fire", 0.0);
                snap.heraldOfAshActive = true;
                const double burnMore = 1.0 + lv.GetStat("herald_of_ash_burning_damage_+%_final", 0.0) / 100.0;
                snap.heraldOfAshOverkillPct = (lv.GetStat("herald_of_ash_burning_%_overkill_damage_per_minute", 1500.0) / 60.0) * burnMore;
            }
        }

        // 5단계: Headhunter 훔친 레어 몬스터 속성(l2.headhunterModMask) 반영
        if (l2.headhunterModMask != 0) {
            auto hasHh = [&](MonsterModKind m) {
                return (l2.headhunterModMask & static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(m))) != 0;
            };
            if (hasHh(MonsterModKind::Hasted)) {
                aura.incAttackSpeedPct += 20.0;
                aura.incCastSpeedPct += 20.0;
                aura.incMovementSpeedPct += 20.0;
            }
            if (hasHh(MonsterModKind::Armoured)) {
                aura.incArmourPct += 50.0;
                aura.physDamageReduction += 10.0;
            }
            if (hasHh(MonsterModKind::FireResistant))      aura.fireResist += 30.0;
            if (hasHh(MonsterModKind::ColdResistant))      aura.coldResist += 30.0;
            if (hasHh(MonsterModKind::LightningResistant)) aura.lightningResist += 30.0;
            if (hasHh(MonsterModKind::ChaosResistant))     aura.chaosResist += 30.0;
            if (hasHh(MonsterModKind::EnergyShielded)) {
                aura.flatEs += 150.0;
                aura.incEsPct += 25.0;
            }
            if (hasHh(MonsterModKind::ExtraPhysical))  aura.incPhysicalDamagePct += 30.0;
            if (hasHh(MonsterModKind::ExtraFire))      aura.physAddedAsFirePct += 25.0;
            if (hasHh(MonsterModKind::ExtraCold))      aura.physAddedAsColdPct += 25.0;
            if (hasHh(MonsterModKind::ExtraLightning)) aura.physAddedAsLightPct += 25.0;
            if (hasHh(MonsterModKind::Deadly)) {
                aura.incGlobalCritChancePct += 100.0;
                aura.globalCritMultiplierPct += 40.0;
            }
        }

        // 고유·참·오라 플래그 전파
        const auto& cb = l2.charmBonuses;
        snap.elementalAilmentImmune = aura.elementalAilmentImmune;
        snap.cannotBeFrozen = gear.cannotBeFrozen || cb.freezeImmune || aura.elementalAilmentImmune;
        snap.bleedImmune = cb.bleedImmune;
        snap.activeCharmMask = cb.activeMask;
        snap.shavronneChaosEs = gear.shavronneChaosEs;
        snap.starforgeShock = gear.starforgeShock;
        snap.dealNoElemental = gear.dealNoElemental;
        snap.berekProlif = gear.berekProlif;
        snap.headhunter = gear.headhunter;
        snap.flatManaCostDelta = gear.flatManaCostDelta;
        snap.manaRegenPerSec = std::max(0.0, gear.manaRegenPerSec);
        snap.esRecoupPct = std::max(0.0, gear.esRecoupPct);
        snap.incDamageVsRarePct = gear.incDamageVsRarePct;
        snap.itemRarityPct = gear.itemRarityPct;
        snap.flatPhysTakenFromAttacksDelta = gear.flatPhysTakenFromAttacksDelta;

        // 충전 상한
        snap.maxEnduranceCharges = static_cast<std::uint8_t>(std::clamp<std::int32_t>(3 + p.maxEnduranceBonus, 0, 15));
        snap.maxFrenzyCharges = static_cast<std::uint8_t>(std::clamp<std::int32_t>(3 + p.maxFrenzyBonus, 0, 15));
        snap.maxPowerCharges = static_cast<std::uint8_t>(std::clamp<std::int32_t>(3 + p.maxPowerBonus, 0, 15));

        // 키스톤 플래그
        snap.bloodMagic = l1.bloodMagic || p.bloodMagic;
        snap.chaosInoculation = p.chaosInoculation;
        snap.chaosImmune = p.chaosInoculation;
        snap.resoluteTechnique = p.resoluteTechnique;
        snap.unwaveringStance = p.unwaveringStance;

        // 2패스: 확정된 능력치의 내재 보너스 반영 (PoB 2.68.1 Modules/CalcPerform.lua:535-543, Data/Misc.lua:84 대조)
        // PoE1: 힘 2당 생명력 +1, 힘 5당 근접 물리 피해 +1%; 민첩 5당 회피 +1%(내림, Iron Reflexes 시 0); 지능 2당 마나 +1, 지능 10당 ES +1%(내림)
        const double strLife = static_cast<double>(snap.str / 2);
        const double strMeleePhysInc = static_cast<double>(snap.str / 5);
        const double dexEvasionInc = p.ironReflexes ? 0.0 : static_cast<double>(snap.dex / 5);
        const double intMana = static_cast<double>(snap.intel / 2);
        const double intEsInc = static_cast<double>(snap.intel / 10);

        const auto& c = pack.Constants();
        if (snap.chaosInoculation) {
            snap.poeMaxLife = 1.0;
        } else {
            const double baseLife = c.lifeBase + c.lifePerLevel * static_cast<double>(snap.exileLevel) +
                strLife + l1.flatLife + p.flatLife + gear.flatLife;
            const double totalIncLife = l1.incLifePct + p.incLifePct + gear.incLifePct;
            const double moreLife = std::max(0.0, p.moreLifeMult);
            snap.poeMaxLife = std::max(1.0, baseLife * std::max(0.0, 1.0 + totalIncLife / 100.0) * moreLife);
        }

        if (snap.bloodMagic) {
            snap.poeMaxMana = 0.0;
        } else {
            const double baseMana = c.manaBase + c.manaPerLevel * static_cast<double>(snap.exileLevel) +
                intMana + l1.flatMana + p.flatMana + gear.flatMana;
            const double totalIncMana = l1.incManaPct + p.incManaPct + gear.incManaPct;
            snap.poeMaxMana = std::max(0.0, baseMana * std::max(0.0, 1.0 + totalIncMana / 100.0));
        }

        auto sanitizePenalty = [](double raw, double maxCap) noexcept -> double {
            return std::isfinite(raw) ? std::clamp(raw, 0.0, maxCap) : 0.0;
        };
        const double mapLessEsRecharge = snap.inActiveMap ? sanitizePenalty(snap.activeMapMods.playerLessEsRechargePct, 75.0) : 0.0;
        const double mapLessArmour = snap.inActiveMap ? sanitizePenalty(snap.activeMapMods.playerLessArmourPct, 75.0) : 0.0;
        const double mapLessEvasion = snap.inActiveMap ? sanitizePenalty(snap.activeMapMods.playerLessEvasionPct, 75.0) : 0.0;
        const double mapResPen = snap.inActiveMap ? sanitizePenalty(snap.activeMapMods.playerMaxResPenalty, 25.0) : 0.0;

        const double totalFlatEs = l1.flatEnergyShield + p.flatEnergyShield + gear.energyShield + gear.flatEnergyShield + aura.flatEs;
        const double totalIncEs = l1.incEnergyShieldPct + p.incEnergyShieldPct + gear.incEnergyShieldPct + aura.incEsPct + intEsInc;
        snap.poeMaxEnergyShield = std::max(0.0, totalFlatEs * std::max(0.0, 1.0 + totalIncEs / 100.0));
        const double mapEsRechargeFactor = std::max(0.1, 1.0 - mapLessEsRecharge / 100.0);
        snap.esRechargeRateMult = std::max(0.1,
            (1.0 + (p.incEsRechargeRatePct + gear.incEsRechargeRatePct + aura.incEsRechargeRatePct) / 100.0) * mapEsRechargeFactor);
        snap.esRechargeDelayMult = 1.0 / std::max(0.1, 1.0 + (p.fasterEsRechargeStartPct + gear.fasterEsRechargeStartPct) / 100.0);

        // 방어·회피 + Determination/Grace/MoltenShell/BastionCharm + Iron Reflexes / Unwavering Stance
        // C-11: PoB Data/Misc.lua:84 base_evasion_rating = 15 반영
        const double mapArmourFactor = std::max(0.1, 1.0 - mapLessArmour / 100.0);
        const double mapEvasionFactor = std::max(0.1, 1.0 - mapLessEvasion / 100.0);
        const double totalFlatArmour = l1.armour + p.flatArmour + gear.armour + gear.flatArmour +
            aura.flatArmour + cb.flatArmour + l2.moltenShellBonusArmour;
        const double totalFlatEvasion = 15.0 + l1.evasion + p.flatEvasion + gear.evasion + gear.flatEvasion + aura.flatEvasion;
        const double totalArmourIncPct = p.incArmourPct + p.incArmourAndEvasionPct +
            gear.incArmourPct + gear.incArmourAndEvasionPct + aura.incArmourPct;
        const double armourScaleMult = std::max(0.0, 1.0 + totalArmourIncPct / 100.0) * aura.moreArmourMult * mapArmourFactor;
        const double rawArmour = std::max(0.0, totalFlatArmour * std::max(0.0, 1.0 + totalArmourIncPct / 100.0));
        const double lowLifeEvasionInc = (!snap.chaosInoculation && l2.lowLife) ? gear.incEvasionOnLowLifePct : 0.0;
        if (p.ironReflexes) {
            const double convIncPct = p.incArmourPct + p.incEvasionPct + p.incArmourAndEvasionPct +
                gear.incArmourPct + gear.incEvasionPct + gear.incArmourAndEvasionPct + aura.incArmourPct + lowLifeEvasionInc;
            const double convertedEvasion = std::max(0.0, totalFlatEvasion * std::max(0.0, 1.0 + convIncPct / 100.0));
            // 검수 M2 + 6단계 검수 Major 3: PoB Iron Reflexes — 전환 회피에 Grace moreEvasionMult·mapEvasionFactor와 Determination moreArmourMult·mapArmourFactor 모두 곱해짐
            snap.armour = (rawArmour + convertedEvasion * aura.moreEvasionMult * mapEvasionFactor) * aura.moreArmourMult * mapArmourFactor;
            snap.evasion = 0.0;
        } else {
            const double totalEvasionIncPct = p.incEvasionPct + p.incArmourAndEvasionPct +
                gear.incEvasionPct + gear.incArmourAndEvasionPct + dexEvasionInc + lowLifeEvasionInc;
            const double rawEvasion = std::max(0.0, totalFlatEvasion * std::max(0.0, 1.0 + totalEvasionIncPct / 100.0));
            snap.armour = rawArmour * aura.moreArmourMult * mapArmourFactor;
            snap.evasion = p.cannotEvade ? 0.0 : (rawEvasion * aura.moreEvasionMult * mapEvasionFactor);
        }

        // 동적 원소별 최대 저항 상한 (기본 75% + 패시브 + 참 최대 저항 - 지도 최대 모든 저항 감소, PoE 하드캡 90%, 최소 50%)
        snap.maxFireResist = std::clamp(c.resistCap + p.maxFireResistBonus + cb.maxFireResist - mapResPen, 50.0, 90.0);
        snap.maxColdResist = std::clamp(c.resistCap + p.maxColdResistBonus + cb.maxColdResist - mapResPen, 50.0, 90.0);
        snap.maxLightningResist = std::clamp(c.resistCap + p.maxLightningResistBonus + cb.maxLightningResist - mapResPen, 50.0, 90.0);
        snap.maxChaosResist = std::clamp(c.resistCap + p.maxChaosResistBonus - mapResPen, 50.0, 90.0);

        const std::uint8_t clampedEnd = std::min(l2.enduranceCharges, snap.maxEnduranceCharges);
        const double endDR = clampedEnd * kEndurancePhysAndElemDR;
        const double actPenalty = (snap.exileLevel >= config.act10Level) ? -60.0
                                : (snap.exileLevel >= config.act5Level)  ? -30.0
                                                                         : 0.0;
        snap.fireResist = std::min(snap.maxFireResist,
            l1.fireResist + p.fireResist + gear.fireResist + aura.allElemResist + aura.fireResist + cb.fireResist + actPenalty);
        snap.coldResist = std::min(snap.maxColdResist,
            l1.coldResist + p.coldResist + gear.coldResist + aura.allElemResist + aura.coldResist + cb.coldResist + actPenalty);
        snap.lightningResist = std::min(snap.maxLightningResist,
            l1.lightningResist + p.lightningResist + gear.lightningResist + aura.allElemResist + aura.lightningResist + cb.lightningResist + actPenalty);
        snap.chaosResist = snap.chaosImmune
            ? 100.0
            : std::min(snap.maxChaosResist,
                l1.chaosResist + p.chaosResist + gear.chaosResist + aura.chaosResist + cb.chaosResist + actPenalty);

        snap.physDamageReduction = std::clamp(
            l1.physDamageReduction + p.physDamageReduction + gear.physDamageReduction + aura.physDamageReduction + cb.physDamageReduction + endDR,
            0.0, 90.0);
        snap.elementalDamageReduction = std::clamp(l1.elementalDamageReduction + endDR, 0.0, 90.0);
        snap.attackBlockChance = std::clamp(l1.attackBlockChance + p.attackBlockChance + gear.attackBlock + gear.attackBlockChance, 0.0, 75.0);
        snap.spellBlockChance = std::clamp(l1.spellBlockChance + p.spellBlockChance + gear.spellBlockChance, 0.0, 75.0);

        const double rawSupp = std::max(0.0, l1.spellSuppressionChance + p.spellSuppressionChance + gear.spellSuppressionChance);
        if (p.acrobatics) {
            snap.spellDodgeChance = std::clamp(rawSupp * 0.5, 0.0, 75.0);
            snap.spellSuppressionChance = 0.0;
        } else {
            snap.spellDodgeChance = 0.0;
            snap.spellSuppressionChance = std::clamp(rawSupp, 0.0, 100.0);
        }
        snap.spellSuppressionPrevent = std::clamp(l1.spellSuppressionPrevent + p.spellSuppressionPrevent, 0.0, 100.0);

        // Mind Over Matter: Blood Magic/마나 0 빌드에서는 비활성(검수 C4)
        snap.momRatio = (!snap.bloodMagic && snap.poeMaxMana > 0.0)
            ? std::clamp(p.mindOverMatterRatio, 0.0, 1.0)
            : 0.0;
        snap.lifeRegenPerSec = std::max(0.0, gear.lifeRegenPerSec);
        snap.incManaRegenPct = gear.incManaRegenPct;
        snap.incMovementSpeedPct = gear.incMovementSpeedPct + aura.incMovementSpeedPct;

        // 5단계(N-1): 패시브 상태이상 유발 확률 집계
        snap.igniteChancePct = std::clamp(p.igniteChancePct, 0.0, 100.0);
        snap.freezeChancePct = std::clamp(p.freezeChancePct, 0.0, 100.0);
        snap.shockChancePct = std::clamp(p.shockChancePct, 0.0, 100.0);
        snap.bleedChancePct = std::clamp(p.bleedChancePct, 0.0, 100.0);
        snap.poisonChancePct = std::clamp(p.poisonChancePct, 0.0, 100.0);

        // 발동 젬이 어떤 공격 칸에 연결되어 있는지 먼저 수집(결정 131: 공격 1칸당 발동 젬 최대 1개)
        std::array<double, kSkillSlotCount> attackExtraManaMult{};
        std::array<bool, kSkillSlotCount> attackHasTrigger{};
        std::array<bool, kSkillSlotCount> triggerLinkAccepted{};
        for (std::size_t i = 0; i < kSkillSlotCount; ++i) {
            const auto& slot = l1.slots[i];
            if (!slot.occupied) continue;
            const auto* def = pack.FindGem(slot.gem.gemId);
            if (!def || def->kind != GemKind::Trigger) continue;
            const auto* trigLv = def->GetLevel(slot.gem.level);
            if (config.enforceGemRequirements && (!trigLv || !MeetsGemRequirements(*trigLv, snap))) {
                continue;
            }
            if (slot.linkedAttackSlot >= 0 && static_cast<std::size_t>(slot.linkedAttackSlot) < kSkillSlotCount) {
                const auto targetIdx = static_cast<std::size_t>(slot.linkedAttackSlot);
                const auto& atkSlot = l1.slots[targetIdx];
                if (!attackHasTrigger[targetIdx] &&
                    atkSlot.occupied &&
                    pack.CanTriggerLinkAttack(slot.gem.gemId, atkSlot.gem.gemId)) {
                    attackHasTrigger[targetIdx] = true;
                    triggerLinkAccepted[i] = true;
                    if (trigLv) {
                        attackExtraManaMult[targetIdx] += trigLv->manaMultiplier;
                    }
                }
            }
        }

        // 9칸 슬롯 및 발동 젬 안 주문 젬 스탯 집계
        for (std::size_t i = 0; i < kSkillSlotCount; ++i) {
            const auto& slot = l1.slots[i];
            if (!slot.occupied) continue;

            const auto* def = pack.FindGem(slot.gem.gemId);
            if (!def) continue;

            if (def->kind == GemKind::Trigger) {
                snap.slotStats[i].gemInstanceId = slot.gem.instanceId;
                snap.slotStats[i].gemId = slot.gem.gemId;
                snap.slotStats[i].kind = GemKind::Trigger;
                snap.slotStats[i].level = slot.gem.level;
                if (const auto* trigLv = def->GetLevel(slot.gem.level)) {
                    snap.slotStats[i].requirementMet = MeetsGemRequirements(*trigLv, snap);
                }
                if (!triggerLinkAccepted[i]) {
                    continue;
                }
                const auto* trigEffLv = def->GetTriggeredLevel(slot.gem.level);
                const double spellManaMult = trigEffLv ? trigEffLv->manaMultiplier : 20.0;
                const double spellMoreDmg = trigEffLv ? trigEffLv->GetStat("support_cast_on_crit_spell_damage_+%_final", 0.0) : 0.0;
                const double trigCooldown = trigEffLv ? trigEffLv->cooldown : 0.15;

                std::unordered_set<std::string> trigAddedTypes(
                    def->triggeredAddSkillTypes.begin(),
                    def->triggeredAddSkillTypes.end());

                const std::size_t maxSpellSockets = std::min<std::size_t>(
                    slot.gem.socketedGems.size(),
                    slot.gem.socketCount);
                for (std::size_t s = 0; s < maxSpellSockets; ++s) {
                    const auto& spellInst = slot.gem.socketedGems[s];
                    if (!pack.CanTriggerSocketSpell(slot.gem.gemId, spellInst.gemId)) {
                        continue;
                    }
                    auto computed = ComputeSingleSkill(
                        spellInst,
                        pack,
                        config,
                        l1,
                        l2,
                        snap,
                        gear,
                        aura,
                        strMeleePhysInc,
                        armourScaleMult,
                        spellManaMult,
                        spellMoreDmg,
                        true,
                        trigCooldown,
                        false,
                        trigAddedTypes.empty() ? nullptr : &trigAddedTypes);
                    snap.triggeredSpellStats[spellInst.instanceId] = std::move(computed);
                }
            } else {
                const bool isSlotAuraOn = (snap.effectiveActiveAuraMask & static_cast<std::uint16_t>(1u << i)) != 0;
                snap.slotStats[i] = ComputeSingleSkill(
                    slot.gem,
                    pack,
                    config,
                    l1,
                    l2,
                    snap,
                    gear,
                    aura,
                    strMeleePhysInc,
                    armourScaleMult,
                    attackExtraManaMult[i],
                    0.0,
                    false,
                    0.0,
                    isSlotAuraOn);
            }
        }

        return snap;
    }
}
