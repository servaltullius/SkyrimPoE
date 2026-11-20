#include "core/MonsterModSystem.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace SkyrimPoE::Core
{
    namespace
    {
        std::uint16_t RollDistinctMods(std::size_t count, std::mt19937_64& rng)
        {
            std::uint16_t mask = 0;
            std::uniform_int_distribution<std::size_t> dist(0, kMonsterModCount - 1);
            std::size_t picked = 0;
            int guard = 0;
            while (picked < count && guard < 64) {
                ++guard;
                const auto idx = dist(rng);
                const auto bit = static_cast<std::uint16_t>(1u << idx);
                if ((mask & bit) == 0) {
                    mask |= bit;
                    ++picked;
                }
            }
            return mask;
        }
    }

    MonsterProfile MonsterModSystem::RollPromotion(
        bool isBoss,
        bool isDragon,
        double baseSkyrimMaxHealth,
        const Config& config,
        std::mt19937_64& rng)
    {
        MonsterProfile prof{};
        if (!config.enableMonsterPromotion) {
            return prof;
        }

        std::uniform_real_distribution<double> u01(0.0, 1.0);
        const double rareProb = std::clamp(
            config.rareMonsterChance * (isDragon ? 4.0 : (isBoss ? 2.5 : 1.0)), 0.0, 0.85);
        const double magicProb = std::clamp(
            config.magicMonsterChance * (isBoss ? 1.5 : 1.0), 0.0, 1.0 - rareProb);

        const double roll = u01(rng);
        if (roll < rareProb) {
            prof.rarity = MonsterRarity::Rare;
            std::uniform_int_distribution<std::size_t> cntDist(2, 3);
            prof.modMask = RollDistinctMods(cntDist(rng), rng);
        } else if (roll < rareProb + magicProb) {
            prof.rarity = MonsterRarity::Magic;
            prof.modMask = RollDistinctMods(1, rng);
        } else {
            return prof;
        }

        const double safeHp = std::max(10.0, std::isfinite(baseSkyrimMaxHealth) ? baseSkyrimMaxHealth : 100.0);
        if (prof.HasMod(MonsterModKind::EnergyShielded)) {
            const double ratio = std::max(0.0, config.monsterEsRatio) * (prof.rarity == MonsterRarity::Rare ? 1.35 : 1.0);
            prof.maxEsSkyrim = safeHp * ratio;
            prof.currentEsSkyrim = prof.maxEsSkyrim;
        }
        return prof;
    }

    double MonsterModSystem::IncomingDamageMultiplier(
        const MonsterProfile& profile,
        DamageType dominantType) noexcept
    {
        double mult = 1.0;
        if (profile.rarity == MonsterRarity::Magic) mult *= 0.75;
        else if (profile.rarity == MonsterRarity::Rare) mult *= 0.55;

        switch (dominantType) {
            case DamageType::Physical:
                if (profile.HasMod(MonsterModKind::Armoured)) mult *= 0.75;
                break;
            case DamageType::Fire:
                if (profile.HasMod(MonsterModKind::FireResistant)) mult *= 0.60;
                break;
            case DamageType::Cold:
                if (profile.HasMod(MonsterModKind::ColdResistant)) mult *= 0.60;
                break;
            case DamageType::Lightning:
                if (profile.HasMod(MonsterModKind::LightningResistant)) mult *= 0.60;
                break;
            case DamageType::Chaos:
                if (profile.HasMod(MonsterModKind::ChaosResistant)) mult *= 0.60;
                break;
            default:
                break;
        }
        return std::clamp(mult, 0.10, 2.0);
    }

    MonsterIncomingResult MonsterModSystem::ApplyPlayerHitToMonster(
        MonsterProfile& profile,
        double rawSkyrimDamage,
        DamageType dominantType,
        bool bypassesEs) noexcept
    {
        MonsterIncomingResult res{};
        if (!std::isfinite(rawSkyrimDamage) || rawSkyrimDamage <= 0.0) {
            return res;
        }

        const double scaled = rawSkyrimDamage * IncomingDamageMultiplier(profile, dominantType);
        res.hadAnyImpact = scaled > 0.0;

        const bool chaosBypass = bypassesEs || (dominantType == DamageType::Chaos);
        if (!chaosBypass && profile.currentEsSkyrim > 0.0) {
            const double absorbed = std::min(profile.currentEsSkyrim, scaled);
            profile.currentEsSkyrim -= absorbed;
            profile.esRechargeCooldown = kEsRechargeDelaySec;
            res.absorbedByEs = absorbed;
            res.remainingHealthDamage = std::max(0.0, scaled - absorbed);
        } else {
            if (profile.maxEsSkyrim > 0.0) {
                profile.esRechargeCooldown = kEsRechargeDelaySec;
            }
            res.remainingHealthDamage = scaled;
        }
        return res;
    }

    MonsterOutgoingModifier MonsterModSystem::ComputeOutgoingModifier(
        const MonsterProfile& profile) noexcept
    {
        MonsterOutgoingModifier m{};
        if (profile.rarity == MonsterRarity::Magic) m.damageMultiplier *= 1.20;
        else if (profile.rarity == MonsterRarity::Rare) m.damageMultiplier *= 1.35;

        if (profile.HasMod(MonsterModKind::Hasted)) {
            m.damageMultiplier *= 1.15;
            m.isHasted = true;
        }
        if (profile.HasMod(MonsterModKind::ExtraPhysical)) {
            m.damageMultiplier *= 1.25;
        }
        if (profile.HasMod(MonsterModKind::Deadly)) {
            m.damageMultiplier *= 1.20;
            m.isDeadly = true;
        }
        if (profile.HasMod(MonsterModKind::ExtraFire))      m.extraFireRatio = 0.25;
        if (profile.HasMod(MonsterModKind::ExtraCold))      m.extraColdRatio = 0.25;
        if (profile.HasMod(MonsterModKind::ExtraLightning)) m.extraLightningRatio = 0.25;
        return m;
    }

    void MonsterModSystem::TickMonsterEs(MonsterProfile& profile, double deltaSec) noexcept
    {
        if (profile.maxEsSkyrim <= 0.0 || !std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return;
        }
        double rechargeDt = deltaSec;
        if (profile.esRechargeCooldown > 0.0) {
            if (deltaSec <= profile.esRechargeCooldown) {
                profile.esRechargeCooldown -= deltaSec;
                return;
            }
            rechargeDt = deltaSec - profile.esRechargeCooldown;
            profile.esRechargeCooldown = 0.0;
        }
        if (profile.currentEsSkyrim < profile.maxEsSkyrim && rechargeDt > 0.0) {
            profile.currentEsSkyrim = std::min(
                profile.maxEsSkyrim,
                profile.currentEsSkyrim + profile.maxEsSkyrim * kEsRechargeRatePerSec * rechargeDt);
        }
    }

    std::string MonsterModSystem::BuildModSummaryKo(
        const MonsterProfile& profile,
        const ItemData& data)
    {
        if (profile.rarity == MonsterRarity::Normal) {
            return "";
        }
        std::ostringstream oss;
        oss << (profile.rarity == MonsterRarity::Rare ? "[희귀]" : "[매직]");
        bool first = true;
        for (std::size_t i = 0; i < kMonsterModCount; ++i) {
            const auto kind = static_cast<MonsterModKind>(i);
            if (!profile.HasMod(kind)) continue;
            oss << (first ? " " : " · ");
            first = false;
            if (const auto* meta = data.FindMonsterMod(kind)) {
                oss << meta->koName;
            } else {
                oss << "속성#" << i;
            }
        }
        return oss.str();
    }
}
