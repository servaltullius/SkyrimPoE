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

            double manaMult = 1.0 + extraManaMultPct / 100.0;
            double morePhysMult = 1.0;
            double moreAreaDmgMult = 1.0;
            double moreSpellDmgMult = 1.0 + extraMoreSpellDmgPct / 100.0;
            double moreAllDmgMult = 1.0 + (l2.frenzyCharges * kFrenzySpeedAndMore) / 100.0;
            double moreAtkSpeedMult = 1.0;
            double incAreaPct = 0.0;
            double moreAreaMult = 1.0;
            double addBaseCritPct = 0.0;
            double incCritPct = l1.incCritChancePct + l2.powerCharges * kPowerCritInc;
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

            if (def->kind == GemKind::AttackChannel) {
                // 사이클론: 기본 무기 물리 피해 × baseMultiplier × 물리/범위/전체 증폭
                const double scale = lv->baseMultiplier * morePhysMult * areaDmgFactor * moreAllDmgMult;
                out.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Physical)] = DamageRange{
                    l1.baseWeaponPhysicalMin * scale,
                    l1.baseWeaponPhysicalMax * scale
                };
                // 결정 50, 98: 무기 레코드 속도 × (1 + attackSpeedMultiplier/100 = 3.0) × PoE 공속 배율
                const double skillSpeedBase = 1.0 + lv->attackSpeedMultiplier / 100.0;
                const double incSpeed = 1.0 + (l1.incAttackSpeedPct + l2.frenzyCharges * kFrenzySpeedAndMore) / 100.0;
                out.attackSpeedMult = std::max(0.1, skillSpeedBase * incSpeed * moreAtkSpeedMult);
                out.baseCritChancePct = l1.baseWeaponCritChancePct + addBaseCritPct;
            } else if (def->kind == GemKind::Spell) {
                const double spellScale = moreSpellDmgMult * areaDmgFactor * moreAllDmgMult;
                auto applySpellType = [&](DamageType dt, const char* minKey, const char* maxKey, double typeMult) {
                    const double mn = lv->GetStat(minKey, 0.0);
                    const double mx = lv->GetStat(maxKey, 0.0);
                    if (mn > 0.0 || mx > 0.0) {
                        out.baseHitDamage.byType[static_cast<std::size_t>(dt)] = DamageRange{
                            mn * spellScale * typeMult,
                            mx * spellScale * typeMult
                        };
                    }
                };
                applySpellType(DamageType::Physical, "spell_minimum_base_physical_damage", "spell_maximum_base_physical_damage", morePhysMult);
                applySpellType(DamageType::Fire, "spell_minimum_base_fire_damage", "spell_maximum_base_fire_damage", 1.0);
                applySpellType(DamageType::Cold, "spell_minimum_base_cold_damage", "spell_maximum_base_cold_damage", 1.0);
                applySpellType(DamageType::Lightning, "spell_minimum_base_lightning_damage", "spell_maximum_base_lightning_damage", 1.0);
                applySpellType(DamageType::Chaos, "spell_minimum_base_chaos_damage", "spell_maximum_base_chaos_damage", 1.0);
                out.baseCritChancePct = lv->critChance + addBaseCritPct;
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
            out.critMultiplierPct = l1.critMultiplierPct;
            const double totalAreaFactor = std::max(0.05, (1.0 + incAreaPct / 100.0) * moreAreaMult);
            out.areaMult = std::sqrt(totalAreaFactor);
            out.poeManaCost = std::max(0.0, lv->manaCost * manaMult);
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

        const auto* cls = pack.FindClass(l1.className);
        snap.str = cls ? cls->baseStr : 20;
        snap.dex = cls ? cls->baseDex : 20;
        snap.intel = cls ? cls->baseInt : 20;

        const auto& c = pack.Constants();
        // PoE: 힘 2당 생명력 +1 (결정 84: 기대 생명력에는 넣지 않고 실제 빌드에는 반영), 지능 2당 마나 +1
        const double strLife = static_cast<double>(snap.str / 2);
        const double intMana = static_cast<double>(snap.intel / 2);

        const double baseLife = c.lifeBase + c.lifePerLevel * static_cast<double>(snap.exileLevel) + strLife + l1.flatLife;
        snap.poeMaxLife = std::max(1.0, baseLife * (1.0 + l1.incLifePct / 100.0));

        if (l1.bloodMagic) {
            snap.poeMaxMana = 0.0;
        } else {
            const double baseMana = c.manaBase + c.manaPerLevel * static_cast<double>(snap.exileLevel) + intMana + l1.flatMana;
            snap.poeMaxMana = std::max(0.0, baseMana * (1.0 + l1.incManaPct / 100.0));
        }

        const double intEsInc = static_cast<double>(snap.intel / 5);  // PoE: 지능 5당 에너지 실드 +1%
        snap.poeMaxEnergyShield = std::max(0.0, l1.flatEnergyShield * (1.0 + (l1.incEnergyShieldPct + intEsInc) / 100.0));

        snap.armour = std::max(0.0, l1.armour);
        snap.evasion = std::max(0.0, l1.evasion);

        const double endDR = l2.enduranceCharges * kEndurancePhysAndElemDR;
        const double actPenalty = (snap.exileLevel >= config.act10Level) ? -60.0
                                : (snap.exileLevel >= config.act5Level)  ? -30.0
                                                                         : 0.0;
        snap.fireResist = std::min(c.resistCap, l1.fireResist + actPenalty);
        snap.coldResist = std::min(c.resistCap, l1.coldResist + actPenalty);
        snap.lightningResist = std::min(c.resistCap, l1.lightningResist + actPenalty);
        snap.chaosResist = std::min(c.resistCap, l1.chaosResist + actPenalty);

        snap.physDamageReduction = std::clamp(l1.physDamageReduction + endDR, 0.0, 90.0);
        snap.elementalDamageReduction = std::clamp(l1.elementalDamageReduction + endDR, 0.0, 90.0);
        snap.attackBlockChance = std::clamp(l1.attackBlockChance, 0.0, 75.0);
        snap.spellBlockChance = std::clamp(l1.spellBlockChance, 0.0, 75.0);
        snap.spellSuppressionChance = std::clamp(l1.spellSuppressionChance, 0.0, 100.0);
        snap.spellSuppressionPrevent = std::clamp(l1.spellSuppressionPrevent, 0.0, 100.0);

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
                    attackExtraManaMult[i],
                    0.0,
                    false,
                    0.0);
            }
        }

        return snap;
    }
}
