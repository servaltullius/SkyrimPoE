#include "Core/Crit.h"

#include <algorithm>
#include <cmath>

namespace poe
{
    float CombineCritChance(float ownChance, float skyrimPercent)
    {
        const float own = std::isfinite(ownChance) ? ownChance : 0.0f;
        const float skyrim = std::isfinite(skyrimPercent) ? skyrimPercent / 100.0f : 0.0f;
        return std::clamp(own + skyrim, 0.0f, 1.0f);
    }
}
