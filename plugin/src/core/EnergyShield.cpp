#include "core/EnergyShield.h"
#include "core/RulesPack.h"

#include <algorithm>
#include <cmath>

namespace SkyrimPoE::Core
{
    void EnergyShieldPool::Configure(double rechargeDelayMultiplier, double rechargeRateMultiplier,
        bool rechargeEnabled)
    {
        const auto& rule = ActiveRuleValues().energyShield;
        std::scoped_lock lock(mutex_);
        state_.rechargeDelay = rule.rechargeDelaySeconds *
            ((std::isfinite(rechargeDelayMultiplier) && rechargeDelayMultiplier >= 0.0) ? rechargeDelayMultiplier : 1.0);
        state_.rechargeRate = rechargeEnabled
            ? (rule.rechargePercentPerMinute / (60.0 * 100.0)) *
                ((std::isfinite(rechargeRateMultiplier) && rechargeRateMultiplier >= 0.0) ? rechargeRateMultiplier : 1.0)
            : 0.0;
    }

    void EnergyShieldPool::SetMax(double newMax, bool /*refillIfInitializedEmpty*/)
    {
        const double clamped = (std::isfinite(newMax) && newMax > 0.0) ? newMax : 0.0;
        std::scoped_lock lock(mutex_);
        state_.max = clamped;
        if (!initialized_) {
            // 최초 초기화 시에만 만충전하고, 이후 0 -> >0 장비 착탈 시에는 정상 재충전 규칙을 따른다.
            state_.current = clamped;
            state_.timeSinceLastHit = state_.rechargeDelay;
            initialized_ = true;
        } else {
            state_.current = std::clamp(state_.current, 0.0, clamped);
        }
    }

    void EnergyShieldPool::ResetFull(double newMax)
    {
        const double clamped = (std::isfinite(newMax) && newMax > 0.0) ? newMax : 0.0;
        std::scoped_lock lock(mutex_);
        state_.max = clamped;
        state_.current = clamped;
        state_.timeSinceLastHit = state_.rechargeDelay;
        initialized_ = true;
    }

    void EnergyShieldPool::Clear()
    {
        std::scoped_lock lock(mutex_);
        state_.current = 0.0;
        state_.max = 0.0;
        state_.timeSinceLastHit = 0.0;
        initialized_ = false;
    }

    void EnergyShieldPool::Recover(double amount)
    {
        if (!std::isfinite(amount) || amount <= 0.0) {
            return;
        }
        std::scoped_lock lock(mutex_);
        state_.current = std::min(state_.max, state_.current + amount);
    }

    EnergyShieldAbsorbResult EnergyShieldPool::AbsorbDamage(
        double rawPostDefenseDamage,
        double difficultyToPcMult,
        bool isChaos,
        AilmentKind ailment)
    {
        std::scoped_lock lock(mutex_);
        const auto res = AbsorbWithEnergyShield(
            rawPostDefenseDamage,
            difficultyToPcMult,
            isChaos,
            state_.current,
            ailment);
        if (res.newEnergyShield < state_.current) {
            state_.timeSinceLastHit = 0.0;
        }
        state_.current = std::clamp(res.newEnergyShield, 0.0, state_.max);
        return res;
    }

    void EnergyShieldPool::Tick(double deltaSec, bool gamePaused)
    {
        if (gamePaused || !std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return;
        }
        std::scoped_lock lock(mutex_);
        if (state_.max <= 0.0) {
            return;
        }
        state_.timeSinceLastHit += deltaSec;
        if (state_.timeSinceLastHit > state_.rechargeDelay && state_.current < state_.max) {
            const double rechargeSec = std::min(deltaSec, state_.timeSinceLastHit - state_.rechargeDelay);
            const double gain = state_.max * state_.rechargeRate * rechargeSec;
            state_.current = std::min(state_.max, state_.current + gain);
        }
    }

    EnergyShieldState EnergyShieldPool::Snapshot() const
    {
        std::scoped_lock lock(mutex_);
        return state_;
    }
}
