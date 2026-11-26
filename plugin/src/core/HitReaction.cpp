#include "core/HitReaction.h"

#include <cmath>

namespace SkyrimPoE::Core
{
    bool ShouldFlinch(double hitSkyrim, double maxHealth, double nowSec,
        std::optional<double> lastFlinchSec, const FlinchRule& rule) noexcept
    {
        if (!std::isfinite(hitSkyrim) || !std::isfinite(maxHealth) || !std::isfinite(nowSec) ||
            hitSkyrim <= 0.0 || maxHealth <= 0.0 || !std::isfinite(rule.minLifeRatio) || rule.minLifeRatio < 0.0 ||
            !std::isfinite(rule.cooldownSec) || rule.cooldownSec < 0.0 || (lastFlinchSec && !std::isfinite(*lastFlinchSec))) {
            return false;
        }
        if (hitSkyrim / maxHealth < rule.minLifeRatio) {
            return false;
        }
        if (lastFlinchSec && std::isfinite(*lastFlinchSec) && nowSec >= *lastFlinchSec &&
            nowSec - *lastFlinchSec < rule.cooldownSec) {
            return false;
        }
        return true;
    }
}
