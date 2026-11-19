#include "core/SkillSystem.h"

#include <algorithm>
#include <cmath>

namespace SkyrimPoE::Core
{
    void SkillSystem::TickCooldowns(double deltaSec, bool gamePaused)
    {
        if (gamePaused || !std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return;
        }
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
        cooldowns_.clear();
        cursors_.clear();
        triggerDepth_ = 0;
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
            const auto* def = pack.FindGem(slot.gem.gemId);
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

        std::size_t& cursor = cursors_[trigSlot.gem.instanceId];
        cursor %= maxSockets;

        // 순환 순서대로 쿨다운이 준비된 첫 주문을 찾는다(결정 101, 102).
        for (std::size_t step = 0; step < maxSockets; ++step) {
            const std::size_t idx = (cursor + step) % maxSockets;
            const auto& spellInst = trigSlot.gem.socketedGems[idx];
            const auto statsIt = snapshot.triggeredSpellStats.find(spellInst.instanceId);
            if (statsIt == snapshot.triggeredSpellStats.end()) {
                continue;
            }
            const auto& stats = statsIt->second;

            // 개별 쿨다운 중이면 다음 후보 주문 확인
            if (RemainingCooldown(spellInst.instanceId) > 0.0) {
                continue;
            }

            // 비용 확인: Blood Magic이면 생명력 비용 + 1 HP 자살 방지 가드, 아니면 매지카 비용(결정 125, 144)
            double magickaCost = 0.0;
            double healthCost = 0.0;
            const bool usesHealth = snapshot.bloodMagic;
            if (usesHealth) {
                healthCost = ComputeBloodMagicLifeCost(skyrimMaxHealth, snapshot.poeMaxLife, stats.poeManaCost);
                if ((currentSkyrimHealth - healthCost) < (1.0 - 1e-6)) {
                    return std::nullopt;
                }
            } else {
                const auto manaScale = ComputeMagickaScaling(
                    baseSkyrimMaxMagicka,
                    snapshot.poeMaxMana,
                    stats.poeManaCost,
                    snapshot.exileLevel,
                    pack,
                    config);
                magickaCost = manaScale.skyrimSkillCost;
                // 결정 126, 127: 매지카가 부족하면 발동 폐기, 순환 차례 유지, 쿨다운 미소모!
                if (currentSkyrimMagicka + 1e-6 < magickaCost) {
                    return std::nullopt;
                }
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
            cast.effectiveCritChancePct = stats.EffectiveCritChancePct();
            cast.critMultiplierPct = stats.critMultiplierPct;
            cast.skyrimMagickaCost = magickaCost;
            cast.skyrimHealthCost = healthCost;
            cast.usesHealthCost = usesHealth;
            cast.skyrimHitDamage = skyrimDmg;
            cast.areaMult = stats.areaMult;
            cast.cooldownAppliedSec = appliedCd;
            return cast;
        }

        return std::nullopt;
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

    double SkillSystem::RemainingCooldown(std::uint64_t spellInstanceId) const
    {
        const auto it = cooldowns_.find(spellInstanceId);
        return (it != cooldowns_.end() && it->second > 0.0) ? it->second : 0.0;
    }

    std::size_t SkillSystem::RoundRobinCursor(std::uint64_t triggerGemInstanceId) const
    {
        const auto it = cursors_.find(triggerGemInstanceId);
        return (it != cursors_.end()) ? it->second : 0;
    }
}
