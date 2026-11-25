#include "core/SkillSystem.h"
#include "core/GemForms.h"

#include <algorithm>
#include <cmath>

namespace SkyrimPoE::Core
{
    bool SkillResourceCost::CanPayMagicka(double currentMagicka) const noexcept
    {
        return magicka <= 0.0 || (std::isfinite(currentMagicka) && currentMagicka + 1e-6 >= magicka);
    }

    bool SkillResourceCost::CanPayHealth(double currentHealth) const noexcept
    {
        return health <= 0.0 || (std::isfinite(currentHealth) && currentHealth - health >= 1.0);
    }

    SkillResourceCost ComputeSkillResourceCost(
        const ComputedSkillStats& stats, const StatSnapshot& snapshot, double baseSkyrimMaxMagicka,
        double skyrimMaxHealth, double magickaCostMultiplier) noexcept
    {
        // Legacy Blood Magic stores its life cost in the original mana field.
        const double poeLife = snapshot.bloodMagic && !snapshot.poe2Passives && stats.poeLifeCost == 0.0
            ? stats.poeManaCost : stats.poeLifeCost;
        SkillResourceCost costs;
        costs.health = ComputeBloodMagicLifeCost(skyrimMaxHealth, snapshot.poeMaxLife, poeLife);
        if (!snapshot.bloodMagic) {
            costs.magicka = ComputeMagickaScaling(baseSkyrimMaxMagicka, snapshot.poeMaxMana,
                stats.poeManaCost, snapshot.expectedPoeMana).skyrimSkillCost;
            if (std::isfinite(magickaCostMultiplier) && magickaCostMultiplier > 0.0) {
                costs.magicka *= magickaCostMultiplier;
            }
        }
        return costs;
    }

    void SkillSystem::TickCooldowns(double deltaSec, bool gamePaused)
    {
        if (gamePaused || !std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return;
        }
        std::scoped_lock lk(cooldownMutex_);
        for (auto it = cooldowns_.begin(); it != cooldowns_.end();) {
            it->second -= deltaSec;
            if (it->second <= 0.0) {
                it = cooldowns_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void SkillSystem::ResetOnRevert()
    {
        {
            std::scoped_lock lk(cooldownMutex_);
            cooldowns_.clear();
            cursors_.clear();
            triggerDepth_ = 0;
        }
        std::scoped_lock lk(guardMutex_);
        guard_ = {};
        guardAbsorbRatio_ = 0.75;
        guardCooldownOnEndSec_ = 4.0;
        guardReflectPerAbsorbedSkyrim_ = 1.0;
    }

    std::optional<TriggeredSpellCast> SkillSystem::TryTriggerOnCrit(
        std::uint8_t attackSlotIndex,
        const StatSnapshot& snapshot,
        const std::array<SkillSlot, kSkillSlotCount>& slots,
        double currentSkyrimMagicka,
        double baseSkyrimMaxMagicka,
        const DataPack& pack,
        const Config& config,
        double currentSkyrimHealth,
        double skyrimMaxHealth)
    {
        // 결정 103: 발동 깊이 1 제한(발동으로 나간 주문은 추가 발동을 일으키지 않는다)
        if (triggerDepth_ > 0 || attackSlotIndex >= kSkillSlotCount) {
            return std::nullopt;
        }

        // 결정 131: 해당 공격 칸에 연결된 최초 1개의 발동 젬을 찾는다.
        std::size_t triggerSlotIdx = kSkillSlotCount;
        for (std::size_t i = 0; i < kSkillSlotCount; ++i) {
            const auto& slot = slots[i];
            if (!slot.occupied || slot.linkedAttackSlot != static_cast<std::int8_t>(attackSlotIndex)) {
                continue;
            }
            if (!GemForms::AllowsExecution(snapshot.gemGeneration, slot.gem.gemId)) continue;
            const auto* def = pack.FindGem(slot.gem.gemId, snapshot.gemGeneration);
            if (def && def->kind == GemKind::Trigger) {
                triggerSlotIdx = i;
                break;
            }
        }
        if (triggerSlotIdx >= kSkillSlotCount) {
            return std::nullopt;
        }

        const auto& trigSlot = slots[triggerSlotIdx];
        const std::size_t maxSockets = std::min<std::size_t>(trigSlot.gem.socketedGems.size(), trigSlot.gem.socketCount);
        if (maxSockets == 0) {
            return std::nullopt;
        }

        std::scoped_lock lk(cooldownMutex_);
        std::size_t& cursor = cursors_[trigSlot.gem.instanceId];
        cursor %= maxSockets;

        // 순환 순서대로 쿨다운이 준비된 첫 주문을 찾는다(결정 101, 102).
        for (std::size_t step = 0; step < maxSockets; ++step) {
            const std::size_t idx = (cursor + step) % maxSockets;
            const auto& spellInst = trigSlot.gem.socketedGems[idx];
            if (!GemForms::AllowsExecution(snapshot.gemGeneration, spellInst.gemId)) continue;
            const auto statsIt = snapshot.triggeredSpellStats.find(spellInst.instanceId);
            if (statsIt == snapshot.triggeredSpellStats.end()) {
                continue;
            }
            const auto& stats = statsIt->second;
            // 검수 M2: EnforceGemRequirements=true일 때 요구치 미달 발동 주문 젬은 발동 대상에서 제외
            if (config.enforceGemRequirements && !stats.requirementMet) {
                continue;
            }

            // 개별 쿨다운 중이면 다음 후보 주문 확인
            if (RemainingCooldownLocked(spellInst.instanceId) > 0.0) {
                continue;
            }

            // 비용이 부족하면 순환 차례와 쿨다운을 보존한다(결정 126, 127).
            const auto costs = ComputeSkillResourceCost(stats, snapshot, baseSkyrimMaxMagicka, skyrimMaxHealth);
            if (!costs.CanPayHealth(currentSkyrimHealth) || !costs.CanPayMagicka(currentSkyrimMagicka)) {
                return std::nullopt;
            }

            // 통과: 개별 쿨다운 즉시 시작(max(0.15, 주문 자체 쿨다운)), 순환 차례 전진
            const double appliedCd = std::max(kMinTriggerCooldownSec, stats.cooldownSec);
            cooldowns_[spellInst.instanceId] = appliedCd;
            cursor = (idx + 1) % maxSockets;

            const double poeAvgDmg = stats.baseHitDamage.TotalAverage();
            const double skyrimDmg = ConvertOffenseHit(poeAvgDmg, snapshot.exileLevel, pack, config);

            TriggeredSpellCast cast;
            cast.spellInstanceId = spellInst.instanceId;
            cast.gemId = stats.gemId;
            cast.exileLevel = snapshot.exileLevel;
            cast.poeBaseHitDamage = stats.baseHitDamage;
            cast.targetAilmentHitDamage = stats.targetAilmentHitDamage;
            cast.effectiveCritChancePct = stats.EffectiveCritChancePct();
            cast.critMultiplierPct = stats.critMultiplierPct;
            cast.skyrimMagickaCost = costs.magicka;
            cast.skyrimHealthCost = costs.health;
            cast.usesHealthCost = snapshot.bloodMagic || costs.health > 0.0;
            cast.skyrimHitDamage = skyrimDmg;
            cast.areaMult = stats.areaMult;
            cast.cooldownAppliedSec = appliedCd;
            cast.isHandCast = false;
            cast.projectileCount = stats.projectileCount;
            cast.chainCount = stats.chainCount;
            cast.forkEnabled = stats.forkEnabled;
            cast.pierceCount = stats.pierceCount;
            cast.alwaysPierce = stats.alwaysPierce;
            cast.spellRepeatCount = 0;  // 발동 주문은 Spell Echo 반복 불가
            cast.igniteChancePct = stats.igniteChancePct;
            cast.arcRemainingChainMorePct = stats.arcRemainingChainMorePct;
            cast.starforgeShock = snapshot.starforgeShock;
            cast.projectilePierceChancePct = stats.projectilePierceChancePct;
            cast.additionalSpellProjectilesChancePct = stats.additionalSpellProjectilesChancePct;
            cast.additionalProjectileChancePct = stats.additionalProjectileChancePct;
            cast.additionalChainChancePct = stats.additionalChainChancePct;
            cast.additionalForkProjectileChancePct = stats.additionalForkProjectileChancePct;
            cast.projectileSpeedMult = stats.projectileSpeedMult;
            cast.projectileAilmentChanceIncPct = stats.projectileAilmentChanceIncPct;
            return cast;
        }

        return std::nullopt;
    }

    std::optional<TriggeredSpellCast> SkillSystem::TryBuildHandCastSpell(
        std::uint8_t slotIndex,
        const StatSnapshot& snapshot,
        const std::array<SkillSlot, kSkillSlotCount>& slots,
        double currentSkyrimMagicka,
        double baseSkyrimMaxMagicka,
        const DataPack& pack,
        const Config& config,
        double currentSkyrimHealth,
        double skyrimMaxHealth,
        double magickaCostMultiplier)
    {
        if (slotIndex >= kSkillSlotCount) {
            return std::nullopt;
        }
        const auto& slot = slots[slotIndex];
        if (!slot.occupied) {
            return std::nullopt;
        }
        const auto& stats = snapshot.slotStats[slotIndex];
        if (!GemForms::AllowsExecution(snapshot.gemGeneration, slot.gem.gemId) ||
            !GemForms::AllowsExecution(snapshot.gemGeneration, stats.gemId) ||
            (snapshot.gemGeneration != 0 && stats.gemId != slot.gem.gemId) ||
            stats.kind != GemKind::Spell || stats.gemId.empty()) {
            return std::nullopt;
        }
        if (config.enforceGemRequirements && !stats.requirementMet) {
            return std::nullopt;
        }
        std::scoped_lock lk(cooldownMutex_);
        if (RemainingCooldownLocked(slot.gem.instanceId) > 0.0) {
            return std::nullopt;
        }

        const auto costs = ComputeSkillResourceCost(stats, snapshot, baseSkyrimMaxMagicka,
            skyrimMaxHealth, magickaCostMultiplier);
        if (!costs.CanPayHealth(currentSkyrimHealth) || !costs.CanPayMagicka(currentSkyrimMagicka)) {
            return std::nullopt;
        }

        if (stats.cooldownSec > 0.0) {
            cooldowns_[slot.gem.instanceId] = stats.cooldownSec;
        }

        const double poeAvgDmg = stats.baseHitDamage.TotalAverage();
        const double skyrimDmg = ConvertOffenseHit(poeAvgDmg, snapshot.exileLevel, pack, config);

        TriggeredSpellCast cast;
        cast.spellInstanceId = slot.gem.instanceId;
        cast.gemId = stats.gemId;
        cast.exileLevel = snapshot.exileLevel;
        cast.poeBaseHitDamage = stats.baseHitDamage;
        cast.targetAilmentHitDamage = stats.targetAilmentHitDamage;
        cast.effectiveCritChancePct = stats.EffectiveCritChancePct();
        cast.critMultiplierPct = stats.critMultiplierPct;
        cast.skyrimMagickaCost = costs.magicka;
        cast.skyrimHealthCost = costs.health;
        cast.usesHealthCost = snapshot.bloodMagic || costs.health > 0.0;
        cast.skyrimHitDamage = skyrimDmg;
        cast.areaMult = stats.areaMult;
        cast.cooldownAppliedSec = stats.cooldownSec;
        cast.isHandCast = true;
        cast.projectileCount = stats.projectileCount;
        cast.chainCount = stats.chainCount;
        cast.forkEnabled = stats.forkEnabled;
        cast.pierceCount = stats.pierceCount;
        cast.alwaysPierce = stats.alwaysPierce;
        cast.spellRepeatCount = stats.spellRepeatCount;
        cast.igniteChancePct = stats.igniteChancePct;
        cast.arcRemainingChainMorePct = stats.arcRemainingChainMorePct;
        cast.starforgeShock = snapshot.starforgeShock;
        cast.projectilePierceChancePct = stats.projectilePierceChancePct;
        cast.additionalSpellProjectilesChancePct = stats.additionalSpellProjectilesChancePct;
        cast.additionalProjectileChancePct = stats.additionalProjectileChancePct;
        cast.additionalChainChancePct = stats.additionalChainChancePct;
        cast.additionalForkProjectileChancePct = stats.additionalForkProjectileChancePct;
        cast.projectileSpeedMult = stats.projectileSpeedMult;
        cast.projectileAilmentChanceIncPct = stats.projectileAilmentChanceIncPct;
        return cast;
    }

    GuardActivationResult SkillSystem::TryActivateMoltenShell(
        std::uint8_t slotIndex,
        const StatSnapshot& snapshot,
        const std::array<SkillSlot, kSkillSlotCount>& slots,
        double currentSkyrimMagicka,
        double baseSkyrimMaxMagicka,
        double baseSkyrimMaxHealth,
        const DataPack& pack,
        const Config& config,
        double currentSkyrimHealth,
        double skyrimMaxHealth)
    {
        GuardActivationResult res{};
        if (slotIndex >= kSkillSlotCount || !slots[slotIndex].occupied) {
            res.failReason = "슬롯이 비어 있습니다.";
            return res;
        }
        const auto& stats = snapshot.slotStats[slotIndex];
        if (!GemForms::AllowsExecution(snapshot.gemGeneration, slots[slotIndex].gem.gemId)) {
            res.failReason = GemForms::UseReasonText(
                GemForms::ClassifyUse(snapshot.gemGeneration, slots[slotIndex].gem.gemId).reason);
            return res;
        }
        if (stats.kind != GemKind::Guard) {
            res.failReason = "가드 스킬이 아닙니다.";
            return res;
        }
        if (config.enforceGemRequirements && !stats.requirementMet) {
            res.failReason = "젬 요구치를 충족하지 못했습니다.";
            return res;
        }

        // 검수 M4: 단일 스코프 락으로 상태 검사·활성화 사이 TOCTOU 경쟁 차단
        std::scoped_lock lk(guardMutex_);
        if (guard_.active) {
            res.failReason = "용암 방패가 이미 활성화되어 있습니다.";
            return res;
        }
        if (guard_.cooldownRemainingSec > 0.0) {
            res.failReason = "용암 방패가 재사용 대기 중입니다.";
            return res;
        }

        const auto costs = ComputeSkillResourceCost(stats, snapshot, baseSkyrimMaxMagicka, skyrimMaxHealth);
        if (!costs.CanPayHealth(currentSkyrimHealth)) {
            res.failReason = "생명력이 부족합니다.";
            return res;
        }
        if (!costs.CanPayMagicka(currentSkyrimMagicka)) {
            res.failReason = "매지카가 부족합니다.";
            return res;
        }

        const double capSkyrim = std::max(1.0,
            ComputeSkyrimMaxEnergyShield(baseSkyrimMaxHealth, stats.moltenShellAbsorbCapPoe,
                snapshot.expectedPoeLife));
        // 검수 M3: 반사 화염 피해는 방어 체력 배율(baseSkyrimMaxHealth)에 종속되지 않도록 ConvertOffenseHit 선형 환산 계수로 계산
        const double reflectPct = std::max(0.0, stats.moltenShellReflectPct);
        const double fullReflectOffenseSkyrim = ConvertOffenseHit(
            stats.moltenShellAbsorbCapPoe * (reflectPct / 100.0),
            snapshot.exileLevel,
            pack,
            config);

        guard_.active = true;
        guard_.maxShieldSkyrim = capSkyrim;
        guard_.currentShieldSkyrim = capSkyrim;
        guard_.remainingDurationSec = std::max(0.5, stats.moltenShellDurationSec);
        guard_.cooldownRemainingSec = 0.0;
        guard_.bonusFlatArmour = stats.moltenShellFlatArmour;
        guard_.totalAbsorbedSkyrim = 0.0;
        guardAbsorbRatio_ = std::clamp(stats.moltenShellAbsorbPct / 100.0, 0.0, 1.0);
        guardCooldownOnEndSec_ = std::max(4.0, stats.cooldownSec);
        guardReflectPerAbsorbedSkyrim_ = (capSkyrim > 1e-6) ? (fullReflectOffenseSkyrim / capSkyrim) : 0.0;

        res.ok = true;
        res.skyrimMagickaCost = costs.magicka;
        res.skyrimHealthCost = costs.health;
        res.usesHealthCost = snapshot.bloodMagic || costs.health > 0.0;
        res.bonusFlatArmour = stats.moltenShellFlatArmour;
        res.maxShieldSkyrim = capSkyrim;
        return res;
    }

    GuardAbsorbResult SkillSystem::AbsorbWithGuardShield(
        double rawPostDefenseDamage,
        double difficultyToPcMult,
        bool isDoT)
    {
        GuardAbsorbResult out{};
        out.remainingRawDamage = rawPostDefenseDamage;
        if (isDoT || !std::isfinite(rawPostDefenseDamage) || rawPostDefenseDamage <= 0.0) {
            return out;
        }
        const double diff = (std::isfinite(difficultyToPcMult) && difficultyToPcMult > 1e-6) ? difficultyToPcMult : 1.0;

        std::scoped_lock lk(guardMutex_);
        if (!guard_.active || guard_.currentShieldSkyrim <= 0.0) {
            return out;
        }

        const double effective = rawPostDefenseDamage * diff;
        const double wanted = effective * guardAbsorbRatio_;
        const double absorbed = std::min(wanted, guard_.currentShieldSkyrim);
        guard_.currentShieldSkyrim = std::max(0.0, guard_.currentShieldSkyrim - absorbed);
        guard_.totalAbsorbedSkyrim += absorbed;
        out.absorbedEffective = absorbed;
        out.remainingRawDamage = std::max(0.0, (effective - absorbed) / diff);

        if (guard_.currentShieldSkyrim <= 1e-6) {
            guard_.active = false;
            guard_.currentShieldSkyrim = 0.0;
            guard_.remainingDurationSec = 0.0;
            guard_.bonusFlatArmour = 0.0;
            guard_.cooldownRemainingSec = guardCooldownOnEndSec_;
            out.brokeNow = true;
            out.reflectedFireSkyrim = guard_.totalAbsorbedSkyrim * guardReflectPerAbsorbedSkyrim_;
        }
        return out;
    }

    GuardTickResult SkillSystem::TickGuardShield(double deltaSec, bool gamePaused)
    {
        GuardTickResult out{};
        if (gamePaused || !std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return out;
        }

        std::scoped_lock lk(guardMutex_);
        if (guard_.active) {
            guard_.remainingDurationSec -= deltaSec;
            if (guard_.remainingDurationSec <= 0.0) {
                guard_.active = false;
                guard_.currentShieldSkyrim = 0.0;
                guard_.remainingDurationSec = 0.0;
                guard_.bonusFlatArmour = 0.0;
                guard_.cooldownRemainingSec = guardCooldownOnEndSec_;
                out.stateChanged = true;
                out.expiredNow = true;
                out.reflectedFireSkyrim = guard_.totalAbsorbedSkyrim * guardReflectPerAbsorbedSkyrim_;
            }
        } else if (guard_.cooldownRemainingSec > 0.0) {
            guard_.cooldownRemainingSec = std::max(0.0, guard_.cooldownRemainingSec - deltaSec);
        }
        return out;
    }

    GuardShieldSnapshot SkillSystem::GuardSnapshot() const
    {
        std::scoped_lock lk(guardMutex_);
        return guard_;
    }

    double SkillSystem::RollTriggeredSpellSkyrimDamage(
        const TriggeredSpellCast& cast,
        const DataPack& pack,
        const Config& config,
        double damageRoll01,
        double critRoll01,
        bool& outIsCrit) noexcept
    {
        const double t = std::clamp(std::isfinite(damageRoll01) ? damageRoll01 : 0.5, 0.0, 1.0);
        const double cRoll = std::clamp(std::isfinite(critRoll01) ? critRoll01 : 1.0, 0.0, 1.0);

        double poeHit = 0.0;
        for (const auto& r : cast.poeBaseHitDamage.byType) {
            const double lo = std::max(0.0, std::min(r.min, r.max));
            const double hi = std::max(lo, std::max(r.min, r.max));
            poeHit += lo + (hi - lo) * t;
        }

        outIsCrit = (cast.effectiveCritChancePct > 0.0) && (cRoll * 100.0 < cast.effectiveCritChancePct);
        if (outIsCrit) {
            poeHit *= std::max(1.0, cast.critMultiplierPct / 100.0);
        }

        return ConvertOffenseHit(poeHit, cast.exileLevel, pack, config);
    }

    double SkillSystem::RemainingCooldownLocked(std::uint64_t spellInstanceId) const noexcept
    {
        const auto it = cooldowns_.find(spellInstanceId);
        return (it != cooldowns_.end() && it->second > 0.0) ? it->second : 0.0;
    }

    double SkillSystem::RemainingCooldown(std::uint64_t spellInstanceId) const
    {
        std::scoped_lock lk(cooldownMutex_);
        return RemainingCooldownLocked(spellInstanceId);
    }

    std::size_t SkillSystem::RoundRobinCursor(std::uint64_t triggerGemInstanceId) const
    {
        std::scoped_lock lk(cooldownMutex_);
        const auto it = cursors_.find(triggerGemInstanceId);
        return (it != cursors_.end()) ? it->second : 0;
    }
}
