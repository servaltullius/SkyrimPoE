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

        ComputedSkillStats ComputeSingleSkill(
            const GemInstance& gemInst,
            const DataPack& pack,
            const Layer1Input& l1,
            const Layer2Input& l2,
            const StatSnapshot& snap,
            double strMeleePhysIncPct,
            double extraManaMultPct,
            double extraMoreSpellDmgPct,
            bool isTriggered,
            double triggerMinCooldown,
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

            const auto& p = l1.passives;
            const std::uint8_t clampedFrenzy = std::min(l2.frenzyCharges, snap.maxFrenzyCharges);
            const std::uint8_t clampedPower = std::min(l2.powerCharges, snap.maxPowerCharges);

            double manaMult = 1.0 + extraManaMultPct / 100.0;
            double morePhysMult = 1.0;
            double moreAreaDmgMult = 1.0;
            double moreSpellDmgMult = 1.0 + extraMoreSpellDmgPct / 100.0;
            // Pain Attunement: 저체력(CI 제외) 시 주문 피해 30% 증폭
            if (!snap.chaosInoculation && p.painAttunement && l2.lowLife) {
                moreSpellDmgMult *= 1.30;
            }
            double moreAllDmgMult = 1.0 + (clampedFrenzy * kFrenzySpeedAndMore) / 100.0;
            double moreAtkSpeedMult = 1.0;
            double incAreaPct = p.incAreaOfEffectPct;
            double moreAreaMult = 1.0;
            double addBaseCritPct = 0.0;
            double incCritPct = l1.incCritChancePct + p.incGlobalCritChancePct + clampedPower * kPowerCritInc;
            bool noElemental = false;
            bool noChaos = false;

            // 소켓 수 상한 내에서 동일 보조 젬(gemId)은 최고 레벨 1개만 적용(6.1 규칙)
            const std::size_t maxSockets = std::min<std::size_t>(gemInst.socketedGems.size(), gemInst.socketCount);
            std::unordered_map<std::string, const GemInstance*> dedupSupports;
            std::vector<std::string> dedupOrder;
            for (std::size_t s = 0; s < maxSockets; ++s) {
                const auto& supInst = gemInst.socketedGems[s];
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
            }

            const bool isArea = def->skillTypes.contains("Area") || extraSkillTypes.contains("Area");
            const double areaDmgFactor = isArea ? moreAreaDmgMult : 1.0;
            const double areaIncPct = isArea ? p.incAreaDamagePct : 0.0;

            double totalCostEffPct = p.costEfficiencyPct;
            const double eoElemMoreMult = (p.elementalOverload && l2.elementalOverloadActive) ? 1.40 : 1.0;

            if (def->kind == GemKind::AttackChannel) {
                // 사이클론: 기본 무기 물리 피해 × baseMultiplier × (1 + inc%/100) × 물리/범위/전체 증폭
                // (p.incElementalAttackDamagePct는 4단계 장비/오라 추가 원소 공격 피해 확장용으로 파싱·예약됨)
                const double totalIncPct = p.incGlobalDamagePct + p.incPhysicalDamagePct +
                    p.incPhysicalAttackDamagePct + p.incMeleeDamagePct +
                    p.incMeleePhysicalDamagePct + p.incAttackDamagePct + areaIncPct + strMeleePhysIncPct;
                const double incMult = std::max(0.0, 1.0 + totalIncPct / 100.0);
                const double scale = lv->baseMultiplier * incMult * morePhysMult * areaDmgFactor * moreAllDmgMult;
                out.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Physical)] = DamageRange{
                    l1.baseWeaponPhysicalMin * scale,
                    l1.baseWeaponPhysicalMax * scale
                };
                // 결정 50, 98: 무기 레코드 속도 × (1 + attackSpeedMultiplier/100 = 3.0) × PoE 공속 배율
                const double skillSpeedBase = 1.0 + lv->attackSpeedMultiplier / 100.0;
                const double incSpeed = std::max(0.1,
                    1.0 + (l1.incAttackSpeedPct + p.incAttackSpeedPct + p.incMeleeAttackSpeedPct +
                           clampedFrenzy * kFrenzySpeedAndMore) / 100.0);
                out.attackSpeedMult = std::max(0.1, skillSpeedBase * incSpeed * moreAtkSpeedMult);
                incCritPct += p.incAttackCritChancePct + p.incMeleeCritChancePct;
                // Resolute Technique: PoE 3.30 원작 규칙(공격·주문 모두 치명타 불가)
                out.baseCritChancePct = snap.resoluteTechnique ? 0.0 : (l1.baseWeaponCritChancePct + addBaseCritPct);
                out.critMultiplierPct = p.elementalOverload
                    ? 100.0
                    : std::max(100.0, l1.critMultiplierPct + p.globalCritMultiplierPct + p.meleeCritMultiplierPct);
                totalCostEffPct += p.attackCostEfficiencyPct;
            } else if (def->kind == GemKind::Spell) {
                const double ironWillBonus = p.ironWill ? strMeleePhysIncPct : 0.0;
                const double commonSpellInc = p.incGlobalDamagePct + p.incSpellDamagePct + areaIncPct + ironWillBonus;
                const double spellScale = moreSpellDmgMult * areaDmgFactor * moreAllDmgMult;
                auto applySpellType = [&](DamageType dt, const char* minKey, const char* maxKey, double typeMore, double typeInc) {
                    const double mn = lv->GetStat(minKey, 0.0);
                    const double mx = lv->GetStat(maxKey, 0.0);
                    if (mn > 0.0 || mx > 0.0) {
                        const double incMult = std::max(0.0, 1.0 + (commonSpellInc + typeInc) / 100.0);
                        const double factor = spellScale * typeMore * incMult;
                        out.baseHitDamage.byType[static_cast<std::size_t>(dt)] = DamageRange{
                            mn * factor,
                            mx * factor
                        };
                    }
                };
                applySpellType(DamageType::Physical, "spell_minimum_base_physical_damage", "spell_maximum_base_physical_damage",
                    morePhysMult, p.incPhysicalDamagePct);
                applySpellType(DamageType::Fire, "spell_minimum_base_fire_damage", "spell_maximum_base_fire_damage",
                    eoElemMoreMult, p.incElementalDamagePct + p.incFireDamagePct);
                applySpellType(DamageType::Cold, "spell_minimum_base_cold_damage", "spell_maximum_base_cold_damage",
                    eoElemMoreMult, p.incElementalDamagePct + p.incColdDamagePct);
                applySpellType(DamageType::Lightning, "spell_minimum_base_lightning_damage", "spell_maximum_base_lightning_damage",
                    eoElemMoreMult, p.incElementalDamagePct + p.incLightningDamagePct);
                applySpellType(DamageType::Chaos, "spell_minimum_base_chaos_damage", "spell_maximum_base_chaos_damage",
                    1.0, p.incChaosDamagePct);
                out.castSpeedMult = std::max(0.1,
                    1.0 + (p.incCastSpeedPct + clampedFrenzy * kFrenzySpeedAndMore) / 100.0);
                incCritPct += p.incSpellCritChancePct;
                out.baseCritChancePct = snap.resoluteTechnique ? 0.0 : (lv->critChance + addBaseCritPct);
                out.critMultiplierPct = p.elementalOverload
                    ? 100.0
                    : std::max(100.0, l1.critMultiplierPct + p.globalCritMultiplierPct + p.spellCritMultiplierPct);
                totalCostEffPct += p.spellCostEfficiencyPct;
            } else {
                out.critMultiplierPct = l1.critMultiplierPct;
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
            const double totalAreaFactor = std::max(0.05, (1.0 + incAreaPct / 100.0) * moreAreaMult);
            out.areaMult = std::sqrt(totalAreaFactor);
            const double reducedCostFactor = std::clamp(1.0 - p.reducedManaCostPct / 100.0, 0.0, 10.0);
            const double costEffDiv = std::max(0.1, 1.0 + totalCostEffPct / 100.0);
            out.poeManaCost = std::max(0.0, lv->manaCost * manaMult * reducedCostFactor / costEffDiv);
            out.cooldownSec = std::max(lv->cooldown, triggerMinCooldown);
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

        // 1패스: 클래스 기본 + 패시브 고정/증가 능력치 수렴
        const double finalStr = std::max(0.0, (baseStr + p.flatStr) * std::max(0.0, 1.0 + p.incStrPct / 100.0));
        const double finalDex = std::max(0.0, (baseDex + p.flatDex) * std::max(0.0, 1.0 + p.incDexPct / 100.0));
        const double finalInt = std::max(0.0, (baseInt + p.flatInt) * std::max(0.0, 1.0 + p.incIntPct / 100.0));
        snap.str = static_cast<std::uint32_t>(std::floor(finalStr + 1e-6));
        snap.dex = static_cast<std::uint32_t>(std::floor(finalDex + 1e-6));
        snap.intel = static_cast<std::uint32_t>(std::floor(finalInt + 1e-6));

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

        // 2패스: 확정된 능력치의 내재 보너스 반영
        // PoE1: 힘 2당 생명력 +1, 힘 5당 근접 물리 피해 +1%; 민첩 1당 회피 +0.2%(Iron Reflexes 시 0); 지능 2당 마나 +1, 지능 5당 ES +1%
        const double strLife = static_cast<double>(snap.str / 2);
        const double strMeleePhysInc = static_cast<double>(snap.str / 5);
        const double dexEvasionInc = p.ironReflexes ? 0.0 : (static_cast<double>(snap.dex) * 0.2);
        const double intMana = static_cast<double>(snap.intel / 2);
        const double intEsInc = static_cast<double>(snap.intel / 5);

        const auto& c = pack.Constants();
        if (snap.chaosInoculation) {
            snap.poeMaxLife = 1.0;
        } else {
            const double baseLife = c.lifeBase + c.lifePerLevel * static_cast<double>(snap.exileLevel) +
                strLife + l1.flatLife + p.flatLife;
            const double totalIncLife = l1.incLifePct + p.incLifePct;
            const double moreLife = std::max(0.0, p.moreLifeMult);
            snap.poeMaxLife = std::max(1.0, baseLife * std::max(0.0, 1.0 + totalIncLife / 100.0) * moreLife);
        }

        if (snap.bloodMagic) {
            snap.poeMaxMana = 0.0;
        } else {
            const double baseMana = c.manaBase + c.manaPerLevel * static_cast<double>(snap.exileLevel) +
                intMana + l1.flatMana + p.flatMana;
            const double totalIncMana = l1.incManaPct + p.incManaPct;
            snap.poeMaxMana = std::max(0.0, baseMana * std::max(0.0, 1.0 + totalIncMana / 100.0));
        }

        const double totalFlatEs = l1.flatEnergyShield + p.flatEnergyShield;
        const double totalIncEs = l1.incEnergyShieldPct + p.incEnergyShieldPct + intEsInc;
        snap.poeMaxEnergyShield = std::max(0.0, totalFlatEs * std::max(0.0, 1.0 + totalIncEs / 100.0));
        snap.esRechargeRateMult = std::max(0.1, 1.0 + p.incEsRechargeRatePct / 100.0);
        snap.esRechargeDelayMult = 1.0 / std::max(0.1, 1.0 + p.fasterEsRechargeStartPct / 100.0);

        // 방어·회피 + Iron Reflexes / Unwavering Stance
        // PoE1 Iron Reflexes 규칙: 전환된 회피는 방어도 증가·회피 증가를 모두 받되 하이브리드(% Armour and Evasion)는 1회만 합산, 민첩 회피 보너스는 미적용.
        const double totalArmourIncPct = p.incArmourPct + p.incArmourAndEvasionPct;
        const double rawArmour = std::max(0.0, (l1.armour + p.flatArmour) * std::max(0.0, 1.0 + totalArmourIncPct / 100.0));
        if (p.ironReflexes) {
            const double convIncPct = p.incArmourPct + p.incEvasionPct + p.incArmourAndEvasionPct;
            const double convertedEvasion = std::max(0.0, (l1.evasion + p.flatEvasion) * std::max(0.0, 1.0 + convIncPct / 100.0));
            snap.armour = rawArmour + convertedEvasion;
            snap.evasion = 0.0;
        } else {
            const double totalEvasionIncPct = p.incEvasionPct + p.incArmourAndEvasionPct + dexEvasionInc;
            const double rawEvasion = std::max(0.0, (l1.evasion + p.flatEvasion) * std::max(0.0, 1.0 + totalEvasionIncPct / 100.0));
            snap.armour = rawArmour;
            snap.evasion = p.cannotEvade ? 0.0 : rawEvasion;
        }

        // 동적 원소별 최대 저항 상한 (기본 75%, PoE 하드캡 90%, 검수 M1)
        snap.maxFireResist = std::clamp(c.resistCap + p.maxFireResistBonus, 0.0, 90.0);
        snap.maxColdResist = std::clamp(c.resistCap + p.maxColdResistBonus, 0.0, 90.0);
        snap.maxLightningResist = std::clamp(c.resistCap + p.maxLightningResistBonus, 0.0, 90.0);
        snap.maxChaosResist = std::clamp(c.resistCap + p.maxChaosResistBonus, 0.0, 90.0);

        const std::uint8_t clampedEnd = std::min(l2.enduranceCharges, snap.maxEnduranceCharges);
        const double endDR = clampedEnd * kEndurancePhysAndElemDR;
        const double actPenalty = (snap.exileLevel >= config.act10Level) ? -60.0
                                : (snap.exileLevel >= config.act5Level)  ? -30.0
                                                                         : 0.0;
        snap.fireResist = std::min(snap.maxFireResist, l1.fireResist + p.fireResist + actPenalty);
        snap.coldResist = std::min(snap.maxColdResist, l1.coldResist + p.coldResist + actPenalty);
        snap.lightningResist = std::min(snap.maxLightningResist, l1.lightningResist + p.lightningResist + actPenalty);
        snap.chaosResist = snap.chaosImmune ? 100.0 : std::min(snap.maxChaosResist, l1.chaosResist + p.chaosResist + actPenalty);

        snap.physDamageReduction = std::clamp(l1.physDamageReduction + p.physDamageReduction + endDR, 0.0, 90.0);
        snap.elementalDamageReduction = std::clamp(l1.elementalDamageReduction + endDR, 0.0, 90.0);
        snap.attackBlockChance = std::clamp(l1.attackBlockChance + p.attackBlockChance, 0.0, 75.0);
        snap.spellBlockChance = std::clamp(l1.spellBlockChance + p.spellBlockChance, 0.0, 75.0);

        const double rawSupp = std::max(0.0, l1.spellSuppressionChance + p.spellSuppressionChance);
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

        // 발동 젬이 어떤 공격 칸에 연결되어 있는지 먼저 수집(결정 131: 공격 1칸당 발동 젬 최대 1개)
        std::array<double, kSkillSlotCount> attackExtraManaMult{};
        std::array<bool, kSkillSlotCount> attackHasTrigger{};
        std::array<bool, kSkillSlotCount> triggerLinkAccepted{};
        for (std::size_t i = 0; i < kSkillSlotCount; ++i) {
            const auto& slot = l1.slots[i];
            if (!slot.occupied) continue;
            const auto* def = pack.FindGem(slot.gem.gemId);
            if (!def || def->kind != GemKind::Trigger) continue;
            if (slot.linkedAttackSlot >= 0 && static_cast<std::size_t>(slot.linkedAttackSlot) < kSkillSlotCount) {
                const auto targetIdx = static_cast<std::size_t>(slot.linkedAttackSlot);
                const auto& atkSlot = l1.slots[targetIdx];
                if (!attackHasTrigger[targetIdx] &&
                    atkSlot.occupied &&
                    pack.CanTriggerLinkAttack(slot.gem.gemId, atkSlot.gem.gemId)) {
                    attackHasTrigger[targetIdx] = true;
                    triggerLinkAccepted[i] = true;
                    if (const auto* trigLv = def->GetLevel(slot.gem.level)) {
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
                        l1,
                        l2,
                        snap,
                        strMeleePhysInc,
                        spellManaMult,
                        spellMoreDmg,
                        true,
                        trigCooldown,
                        trigAddedTypes.empty() ? nullptr : &trigAddedTypes);
                    snap.triggeredSpellStats[spellInst.instanceId] = std::move(computed);
                }
            } else {
                snap.slotStats[i] = ComputeSingleSkill(
                    slot.gem,
                    pack,
                    l1,
                    l2,
                    snap,
                    strMeleePhysInc,
                    attackExtraManaMult[i],
                    0.0,
                    false,
                    0.0);
            }
        }

        return snap;
    }
}
