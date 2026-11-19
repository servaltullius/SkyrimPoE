#include "core/DamageConversion.h"

#include <algorithm>
#include <cmath>

namespace SkyrimPoE::Core
{
    namespace
    {
        std::uint32_t ClampLevel(std::uint32_t level) noexcept
        {
            return std::clamp<std::uint32_t>(level, 1, kMaxExileLevel);
        }

        double SafePositive(double v, double fallback) noexcept
        {
            return (std::isfinite(v) && v > 0.0) ? v : fallback;
        }
    }

    double ExpectedHitDamage(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        const auto lv = ClampLevel(exileLevel);
        const double n = SafePositive(config.offenseBaseHits, 3.0);
        return pack.MonsterLife(lv) / n;
    }

    double ConvertOffenseHit(
        double poeHitDamage,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        if (!std::isfinite(poeHitDamage) || poeHitDamage <= 0.0) {
            return 0.0;
        }
        const auto lv = ClampLevel(exileLevel);
        const double lifeL = SafePositive(pack.MonsterLife(lv), 22.0);
        const double life1 = SafePositive(pack.MonsterLife(1), 22.0);
        const double g = lifeL / life1;
        const double scale = std::pow(g, 1.0 - config.offenseRollback);
        const double out = config.offenseK * (poeHitDamage / lifeL) * scale;
        return (std::isfinite(out) && out > 0.0) ? out : 0.0;
    }

    double ConvertOffenseDps(
        double poeDps,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        if (!std::isfinite(poeDps) || poeDps <= 0.0) {
            return 0.0;
        }
        const double rate = SafePositive(config.baseHitRate, 1.6);
        const double perHit = ConvertOffenseHit(poeDps / rate, exileLevel, pack, config);
        return perHit * rate;
    }

    double ArmourPassRatio(double armour, double poeHit, double cap) noexcept
    {
        if (!std::isfinite(armour) || armour <= 0.0) {
            return 1.0;
        }
        if (!std::isfinite(poeHit) || poeHit <= 0.0) {
            return 1.0 - std::clamp(cap, 0.0, 0.90);
        }
        const double reduction = std::min(cap, armour / (armour + 5.0 * poeHit));
        return std::clamp(1.0 - reduction, 0.0, 1.0);
    }

    double EvasionHitChance(double evasion, double accuracy, double minHit) noexcept
    {
        if (!std::isfinite(accuracy) || accuracy <= 0.0) {
            return minHit;
        }
        const double ev = (std::isfinite(evasion) && evasion > 0.0) ? evasion : 0.0;
        const double raw = accuracy / (accuracy + std::pow(ev / 5.0, 0.9)) * 125.0;
        const double pct = std::floor(raw + 0.5);
        return std::clamp(pct, minHit * 100.0, 100.0) / 100.0;
    }

    double ExpectedPhysicalPass(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        const auto lv = ClampLevel(exileLevel);
        const auto& c = pack.Constants();
        const auto& row = pack.ExpectedDefense(lv);
        const double h = c.monsterDamageMult * pack.MonsterDamage(lv);
        const double acc = pack.MonsterAccuracy(lv);
        const double mult = 1.0 + std::max(0.0, config.expectedArmorIncrease);
        const double pArmour = ArmourPassRatio(row.bestArmour * mult, h, c.armourMitigationCap);
        const double pEvasion = EvasionHitChance(row.bestEvasion * mult, acc, c.minHitChance);
        return (pArmour + pEvasion + 1.0) / 3.0;
    }

    double ExpectedElementalPass(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        const auto lv = ClampLevel(exileLevel);
        const auto& c = pack.Constants();
        const auto& row = pack.ExpectedDefense(lv);
        const double res = std::min(c.resistCap, std::max(0.0, config.expectedResistAffixes) * row.bestResistTier);
        return std::max(0.01, 1.0 - res / 100.0);
    }

    double ExpectedChaosPass(
        std::uint32_t exileLevel,
        const DataPack& /*pack*/,
        const Config& config) noexcept
    {
        const auto lv = ClampLevel(exileLevel);
        const double res = (lv >= config.act10Level) ? -60.0
                         : (lv >= config.act5Level)  ? -30.0
                                                     : 0.0;
        return 1.0 - res / 100.0;
    }

    double ExpectedLife(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        const auto lv = ClampLevel(exileLevel);
        const auto& c = pack.Constants();
        return (c.lifeBase + c.lifePerLevel * static_cast<double>(lv)) * (1.0 + std::max(0.0, config.expectedLifeIncrease));
    }

    double ExpectedMana(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        const auto lv = ClampLevel(exileLevel);
        const auto& c = pack.Constants();
        return (c.manaBase + c.manaPerLevel * static_cast<double>(lv)) * (1.0 + std::max(0.0, config.expectedManaIncrease));
    }

    double ConvertDefensePhysicalHit(
        double skyrimPreArmorD,
        std::uint32_t exileLevel,
        const StatSnapshot& snapshot,
        const DataPack& pack,
        const Config& config) noexcept
    {
        if (!std::isfinite(skyrimPreArmorD) || skyrimPreArmorD <= 0.0) {
            return 0.0;
        }
        const auto lv = ClampLevel(exileLevel);
        const auto& c = pack.Constants();
        const double dref = SafePositive(config.defenseDref, 10.7);
        const double h = skyrimPreArmorD * (c.monsterDamageMult * pack.MonsterDamage(lv) / dref);

        const double armourRed = (snapshot.armour > 0.0) ? (snapshot.armour / (snapshot.armour + 5.0 * h)) : 0.0;
        const double totalRed = std::clamp(armourRed + snapshot.physDamageReduction / 100.0, 0.0, c.armourMitigationCap);
        const double pass = 1.0 - totalRed;

        const double meL = SafePositive(ExpectedPhysicalPass(lv, pack, config), 1.0);
        const double me1 = SafePositive(ExpectedPhysicalPass(1, pack, config), 1.0);
        const double denom = std::pow(meL, config.defenseRollback) * std::pow(me1, 1.0 - config.defenseRollback);
        const double out = skyrimPreArmorD * pass / SafePositive(denom, 1.0);
        return (std::isfinite(out) && out > 0.0) ? out : 0.0;
    }

    double ConvertDefenseResistDamage(
        double skyrimPreResistD,
        DamageType damageType,
        std::uint32_t exileLevel,
        const StatSnapshot& snapshot,
        const DataPack& pack,
        const Config& config) noexcept
    {
        if (!std::isfinite(skyrimPreResistD) || skyrimPreResistD <= 0.0) {
            return 0.0;
        }
        const auto lv = ClampLevel(exileLevel);
        double resist = 0.0;
        double cap = pack.Constants().resistCap;
        double meL = 1.0;
        double me1 = 1.0;
        switch (damageType) {
            case DamageType::Fire:
                resist = snapshot.fireResist;
                cap = std::clamp(snapshot.maxFireResist, 0.0, 90.0);
                meL = ExpectedElementalPass(lv, pack, config);
                me1 = ExpectedElementalPass(1, pack, config);
                break;
            case DamageType::Cold:
                resist = snapshot.coldResist;
                cap = std::clamp(snapshot.maxColdResist, 0.0, 90.0);
                meL = ExpectedElementalPass(lv, pack, config);
                me1 = ExpectedElementalPass(1, pack, config);
                break;
            case DamageType::Lightning:
                resist = snapshot.lightningResist;
                cap = std::clamp(snapshot.maxLightningResist, 0.0, 90.0);
                meL = ExpectedElementalPass(lv, pack, config);
                me1 = ExpectedElementalPass(1, pack, config);
                break;
            case DamageType::Chaos:
                if (snapshot.chaosImmune) {
                    return 0.0;
                }
                resist = snapshot.chaosResist;
                cap = std::clamp(snapshot.maxChaosResist, 0.0, 90.0);
                meL = ExpectedChaosPass(lv, pack, config);
                me1 = ExpectedChaosPass(1, pack, config);
                break;
            default:
                return skyrimPreResistD;
        }
        double pass = std::max(0.0, 1.0 - std::min(cap, resist) / 100.0);
        // PoE 3.30 인내 충전: 원소 피해 감소(저항 상한과 별도로 곱연산, 카오스 제외)
        if (damageType != DamageType::Chaos && snapshot.elementalDamageReduction > 0.0) {
            const double elemDr = std::clamp(snapshot.elementalDamageReduction, 0.0, 90.0);
            pass *= (1.0 - elemDr / 100.0);
        }
        const double denom = std::pow(SafePositive(meL, 1.0), config.defenseRollback) *
                             std::pow(SafePositive(me1, 1.0), 1.0 - config.defenseRollback);
        const double out = skyrimPreResistD * pass / SafePositive(denom, 1.0);
        return (std::isfinite(out) && out > 0.0) ? out : 0.0;
    }

    double ComputeSkyrimMaxHealth(
        double baseSkyrimMaxHealth,
        double poeMaxLife,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config,
        bool chaosInoculation) noexcept
    {
        const double base = SafePositive(baseSkyrimMaxHealth, 100.0);
        if (chaosInoculation) {
            // 3단계 검수 C6: 스카이림 낙하·환경·물리 즉사 사고를 피하기 위한 CI 최소 안전 체력 하한
            return std::max(1.0, std::min(25.0, base * 0.15));
        }
        const double expLife = SafePositive(ExpectedLife(exileLevel, pack, config), 50.0);
        const double life = SafePositive(poeMaxLife, expLife);
        return std::max(1.0, base * (life / expLife));
    }

    double ComputeBloodMagicLifeCost(
        double skyrimMaxHealth,
        double poeMaxLife,
        double poeSkillCost) noexcept
    {
        if (!std::isfinite(poeSkillCost) || poeSkillCost <= 0.0) {
            return 0.0;
        }
        const double maxHp = SafePositive(skyrimMaxHealth, 100.0);
        const double life = SafePositive(poeMaxLife, 1.0);
        const double out = poeSkillCost * (maxHp / life);
        return (std::isfinite(out) && out > 0.0) ? out : 0.0;
    }

    double ComputeSkyrimMaxEnergyShield(
        double baseSkyrimMaxHealth,
        double poeMaxEnergyShield,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        if (!std::isfinite(poeMaxEnergyShield) || poeMaxEnergyShield <= 0.0) {
            return 0.0;
        }
        const double base = SafePositive(baseSkyrimMaxHealth, 100.0);
        const double expLife = SafePositive(ExpectedLife(exileLevel, pack, config), 50.0);
        return std::max(0.0, base * (poeMaxEnergyShield / expLife));
    }

    MagickaScalingResult ComputeMagickaScaling(
        double baseSkyrimMaxMagicka,
        double poeMaxMana,
        double poeSkillCost,
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        MagickaScalingResult res;
        const double base = SafePositive(baseSkyrimMaxMagicka, 100.0);
        // 결정 144: PoE 마나가 0 이하인 빌드(Blood Magic 등)는 최대 매지카를 원본으로 유지하고 0÷0을 방지한다.
        if (!std::isfinite(poeMaxMana) || poeMaxMana <= 0.0) {
            res.skyrimMaxMagicka = base;
            res.skyrimSkillCost = 0.0;
            res.isZeroManaBuild = true;
            return res;
        }
        const double expMana = SafePositive(ExpectedMana(exileLevel, pack, config), 40.0);
        res.skyrimMaxMagicka = std::max(1.0, base * (poeMaxMana / expMana));
        const double cost = (std::isfinite(poeSkillCost) && poeSkillCost > 0.0) ? poeSkillCost : 0.0;
        res.skyrimSkillCost = cost * (res.skyrimMaxMagicka / poeMaxMana);
        res.isZeroManaBuild = false;
        return res;
    }

    EnergyShieldAbsorbResult AbsorbWithEnergyShield(
        double rawPostDefenseDamage,
        double difficultyToPcMult,
        bool isChaos,
        double currentEnergyShield) noexcept
    {
        EnergyShieldAbsorbResult res;
        const double es = (std::isfinite(currentEnergyShield) && currentEnergyShield > 0.0) ? currentEnergyShield : 0.0;
        res.newEnergyShield = es;

        if (!std::isfinite(rawPostDefenseDamage) || rawPostDefenseDamage <= 0.0) {
            res.remainingRawDamage = 0.0;
            return res;
        }
        if (isChaos || es <= 0.0) {
            res.remainingRawDamage = rawPostDefenseDamage;
            return res;
        }

        const double diff = SafePositive(difficultyToPcMult, 1.0);
        const double effectiveDmg = rawPostDefenseDamage * diff;
        const double absorbed = std::min(es, effectiveDmg);
        res.newEnergyShield = es - absorbed;
        res.absorbedEffectiveDamage = absorbed;
        res.remainingRawDamage = std::max(0.0, (effectiveDmg - absorbed) / diff);
        return res;
    }

    MindOverMatterAbsorbResult AbsorbWithMindOverMatter(
        double rawDamageAfterEs,
        double difficultyToPcMult,
        double momRatio,
        double availableMagicka) noexcept
    {
        MindOverMatterAbsorbResult res;
        if (!std::isfinite(rawDamageAfterEs) || rawDamageAfterEs <= 0.0) {
            res.remainingRawDamage = 0.0;
            return res;
        }
        const double ratio = std::isfinite(momRatio) ? std::clamp(momRatio, 0.0, 1.0) : 0.0;
        const double avail = (std::isfinite(availableMagicka) && availableMagicka > 0.0) ? availableMagicka : 0.0;
        if (ratio <= 0.0 || avail <= 0.0) {
            res.remainingRawDamage = rawDamageAfterEs;
            return res;
        }
        const double diff = SafePositive(difficultyToPcMult, 1.0);
        const double effectiveDmg = rawDamageAfterEs * diff;
        const double absorbed = std::min(avail, effectiveDmg * ratio);
        res.absorbedMagicka = absorbed;
        res.remainingRawDamage = std::max(0.0, (effectiveDmg - absorbed) / diff);
        return res;
    }
}
