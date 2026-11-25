#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace SkyrimPoE::Core
{
    // Keep the existing cast limits for support gems and passive modifiers together.
    inline constexpr std::uint8_t kMaxCastProjectiles = 16;
    inline constexpr std::uint8_t kMaxCastChains = 16;

    template <class Roll>
    [[nodiscard]] bool RollProjectileChance(double chancePct, Roll&& roll)
    {
        return chancePct > 0.0 && (chancePct >= 100.0 || roll() * 100.0 < chancePct);
    }

    template <class Roll>
    [[nodiscard]] std::uint8_t RollCastProjectileCount(std::uint8_t base, double surpassingChancePct,
        std::span<const double> additionalSpellProjectileChances, Roll&& roll)
    {
        int count = std::clamp<int>(base, 1, kMaxCastProjectiles);
        if (std::isfinite(surpassingChancePct) && surpassingChancePct > 0.0 && count < kMaxCastProjectiles) {
            const double chance = std::min(surpassingChancePct, (kMaxCastProjectiles - count) * 100.0);
            const int guaranteed = static_cast<int>(chance / 100.0);
            count += guaranteed;
            if (RollProjectileChance(chance - guaranteed * 100.0, roll)) {
                ++count;
            }
        }
        for (std::size_t amount = 1; amount < additionalSpellProjectileChances.size() && count < kMaxCastProjectiles; ++amount) {
            if (RollProjectileChance(additionalSpellProjectileChances[amount], roll)) {
                count += static_cast<int>(std::min<std::size_t>(amount, kMaxCastProjectiles - count));
            }
        }
        return static_cast<std::uint8_t>(count);
    }

    template <class Roll>
    [[nodiscard]] std::uint8_t RollCastChainCount(std::uint8_t base, double extraChancePct, Roll&& roll)
    {
        if (base >= kMaxCastChains) {
            return kMaxCastChains;
        }
        return static_cast<std::uint8_t>(base + (RollProjectileChance(extraChancePct, roll) ? 1 : 0));
    }

    struct ProjectilePierceRoll
    {
        bool pierced = false;
        std::uint8_t remainingPierces = 0;
    };

    template <class Roll>
    [[nodiscard]] ProjectilePierceRoll RollProjectilePierce(std::uint8_t remainingPierces, bool alwaysPierces,
        double chancePct, Roll&& roll)
    {
        if (alwaysPierces) return { true, remainingPierces };
        if (remainingPierces > 0) return { true, static_cast<std::uint8_t>(remainingPierces - 1) };
        return { RollProjectileChance(chancePct, roll), 0 };
    }

    template <class Roll>
    [[nodiscard]] std::uint8_t RollForkedProjectileCount(double extraChancePct, Roll&& roll)
    {
        return static_cast<std::uint8_t>(RollProjectileChance(extraChancePct, roll) ? 3 : 2);
    }
}
