#include "core/DamageConversion.h"
#include "core/RulesPack.h"

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

        double RoundRulePercent(double percent) noexcept
        {
            return std::floor(percent + ActiveRuleValues().armour.roundingOffset);
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

    bool CanInflictPoEAilments(VanillaHitSource source) noexcept
    {
        return source == VanillaHitSource::Attack;  // 결정 209 ⑨
    }

    double OffenseReferenceLife(std::uint32_t exileLevel, const DataPack& pack, const Config& config) noexcept
    {
        const double lifeL = pack.MonsterLife(exileLevel);
        const double life1 = pack.MonsterLife(1);
        return config.offenseK * std::pow(lifeL / life1, 1.0 - config.offenseRollback);
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
        const double lifeL = pack.MonsterLife(lv);
        const double out = OffenseReferenceLife(lv, pack, config) * (poeHitDamage / lifeL);
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

    double ArmourReductionPercent(double armour, double poeHit) noexcept
    {
        const auto& rule = ActiveRuleValues().armour;
        if (!std::isfinite(armour) || !std::isfinite(poeHit) || poeHit < 0.0 ||
            (armour == 0.0 && poeHit == 0.0)) {
            return rule.zeroOverZeroPercent;
        }
        const double magnitude = std::abs(armour);
        const double sign = armour < 0.0 ? rule.negativeArmourSign : 1.0;
        return RoundRulePercent(sign * (magnitude / (magnitude + rule.damageCoefficient * poeHit)) * 100.0);
    }

    double ArmourPassRatio(double armour, double poeHit, double extraReductionPercent, double reductionCap) noexcept
    {
        const double cap = std::clamp(reductionCap, 0.0, ActiveRuleValues().armour.physicalReductionCapPercent);
        // PoB CalcDefence.lua:405, 440, 2408 floors the combined reduction at zero.
        return 1.0 - std::clamp(ArmourReductionPercent(armour, poeHit) + extraReductionPercent, 0.0, cap) / 100.0;
    }

    void ArmourBreakState::ApplyFullyBroken(double nowSeconds, bool targetIsPlayer) noexcept
    {
        const auto& rule = ActiveRuleValues().armour;
        expiresAt_ = nowSeconds + (targetIsPlayer ? rule.brokenPlayerDurationSeconds : rule.brokenEnemyDurationSeconds);
        targetIsPlayer_ = targetIsPlayer;
    }

    bool ArmourBreakState::IsFullyBroken(double nowSeconds) const noexcept
    {
        return expiresAt_ > nowSeconds;
    }

    double ArmourBreakState::DamageTakenMultiplier(double nowSeconds, DamageType type, bool isDamageOverTime) const noexcept
    {
        if (targetIsPlayer_ || type != DamageType::Physical || isDamageOverTime || !IsFullyBroken(nowSeconds)) {
            return 1.0;
        }
        return 1.0 + ActiveRuleValues().armour.brokenEnemyHitDamageTakenPercent / 100.0;
    }

    bool EnemyArmourBreaks::SetGenerationLocked(std::uint64_t generation)
    {
        if (generation < generation_) return false;
        if (generation != generation_) {
            targets_.clear();
            generation_ = generation;
            nowSeconds_ = 0.0;
        }
        return true;
    }

    void EnemyArmourBreaks::ApplyFullyBroken(std::uint32_t actorHandle, std::uint64_t generation)
    {
        if (actorHandle == 0) return;
        std::scoped_lock lock(mutex_);
        if (!SetGenerationLocked(generation)) return;
        targets_[actorHandle].ApplyFullyBroken(nowSeconds_, false);
    }

    void EnemyArmourBreaks::Advance(double elapsedSeconds, std::uint64_t generation)
    {
        std::scoped_lock lock(mutex_);
        if (!SetGenerationLocked(generation)) return;
        if (std::isfinite(elapsedSeconds) && elapsedSeconds > 0.0) {
            nowSeconds_ += elapsedSeconds;
        }
        std::erase_if(targets_, [this](const auto& target) { return !target.second.IsFullyBroken(nowSeconds_); });
    }

    double EnemyArmourBreaks::DamageTakenMultiplier(std::uint32_t actorHandle, std::uint64_t generation,
        DamageType type, bool isDamageOverTime) const
    {
        std::scoped_lock lock(mutex_);
        if (generation != generation_) return 1.0;
        const auto target = targets_.find(actorHandle);
        return target == targets_.end() ? 1.0 : target->second.DamageTakenMultiplier(nowSeconds_, type, isDamageOverTime);
    }

    EnemyArmourBreaks& ActiveEnemyArmourBreaks() noexcept
    {
        static EnemyArmourBreaks breaks;
        return breaks;
    }

    double EnemyArmourBreaks::ApplyToDamage(std::uint32_t actorHandle, std::uint64_t generation,
        double damage, double physicalFraction, bool isDamageOverTime) const
    {
        if (isDamageOverTime || !std::isfinite(physicalFraction) || physicalFraction <= 0.0) {
            return damage;
        }
        const double multiplier = DamageTakenMultiplier(actorHandle, generation, DamageType::Physical, false);
        return damage + damage * std::min(physicalFraction, 1.0) * (multiplier - 1.0);
    }

    double PhysicalDamageFraction(const DamagePacket& packet) noexcept
    {
        const double total = packet.TotalAverage();
        const double physical = packet.byType[static_cast<std::size_t>(DamageType::Physical)].Average();
        if (!std::isfinite(total) || total <= 0.0 || !std::isfinite(physical)) {
            return 0.0;
        }
        return std::clamp(physical / total, 0.0, 1.0);
    }

    bool PlayerHitMeetsAilmentThreshold(double postDeflectionDamage, double maxHealth) noexcept
    {
        return postDeflectionDamage >= std::max(50.0, maxHealth) * 0.05;
    }

    bool PlayerHitMeetsAilmentThreshold(double damage, double maxHealth, const StatSnapshot& snapshot,
        DamageType type, double skyrimMaximumEnergyShield) noexcept
    {
        if (type == DamageType::Physical || type == DamageType::Chaos) {
            return PlayerHitMeetsAilmentThreshold(damage, maxHealth);
        }
        const auto& p = snapshot.passiveAilments;
        const bool freeze = type == DamageType::Cold;
        const double moreThreshold = p.elementalAilmentThresholdIncPct + (freeze ? p.freezeThresholdIncPct : 0.0);
        const double esAsThreshold = p.esAsAilmentThresholdPct + (freeze ? p.esAsFreezeThresholdPct : 0.0);
        const double effectiveLife = std::max(50.0, maxHealth);
        const double extra = skyrimMaximumEnergyShield * esAsThreshold / 100.0;
        const double threshold = (effectiveLife + extra) * std::max(0.0, 1.0 + moreThreshold / 100.0);
        return damage >= threshold * 0.05;
    }

    double EvasionHitChance(double evasion, double accuracy) noexcept
    {
        const auto& rule = ActiveRuleValues().hit;
        if (!std::isfinite(accuracy) || accuracy < 0.0) {
            return rule.negativeAccuracyPercent / 100.0;
        }
        const double ev = (std::isfinite(evasion) && evasion > 0.0) ? evasion : 0.0;
        if (ev == 0.0) {
            return rule.maxPercent / 100.0;
        }
        const double raw = (1.0 - rule.monsterEvasionMultiplier * ev /
            (ev + rule.monsterAccuracyMultiplier * accuracy)) * 100.0;
        return std::clamp(RoundRulePercent(raw), rule.minPercent, rule.maxPercent) / 100.0;
    }

    double DeflectionChance(double rating, double accuracy) noexcept
    {
        const auto& rule = ActiveRuleValues().deflect;
        if (!std::isfinite(rating) || rating < rule.minimumRating || !std::isfinite(accuracy)) {
            return rule.minPercent / 100.0;
        }
        const double acc = std::max(0.0, accuracy);
        const double miss = acc / (acc + rating * rule.ratingCoefficient) *
            rule.chanceScale - rule.chanceOffsetPercent;
        return std::clamp(rule.percentScale - RoundRulePercent(miss), rule.minPercent, rule.capPercent) / 100.0;
    }

    double ApplyRollBias(double chance, bool lucky, bool unlucky) noexcept
    {
        if (lucky == unlucky) return chance;
        return lucky ? 1.0 - (1.0 - chance) * (1.0 - chance) : chance * chance;
    }

    double DeflectionReduction(const StatSnapshot& snapshot, bool critical) noexcept
    {
        const double extraCrit = critical ? snapshot.passiveAilments.deflectReductionCritBonusPct : 0.0;
        return std::clamp((ActiveRuleValues().deflect.baseReductionPercent + snapshot.deflectReductionBonusPct + extraCrit) / 100.0,
            0.0, 1.0);
    }

    void ConvertGlobalHitDamage(DamagePacket& damage, const PassiveStatTotals& passives, bool isFireSpell) noexcept
    {
        const double toFire = std::max(0.0, passives.convertToFirePct);
        const double fireToChaos = isFireSpell ? std::max(0.0, passives.convertFireSpellToChaosPct) : 0.0;
        if (toFire == 0.0 && fireToChaos == 0.0) {
            return;
        }

        const auto fireIndex = static_cast<std::size_t>(DamageType::Fire);
        const auto chaosIndex = static_cast<std::size_t>(DamageType::Chaos);
        const auto original = damage.byType;
        damage.byType = {};
        for (std::size_t from = 0; from < original.size(); ++from) {
            const double toChaos = from == fireIndex ? fireToChaos : 0.0;
            const double total = toFire + toChaos;
            const double scale = std::max(100.0, total);
            const auto add = [&](std::size_t to, double share) {
                damage.byType[to].min += original[from].min * share;
                damage.byType[to].max += original[from].max * share;
            };
            // The global DamageConvertToFire modifier also participates for original fire.
            add(fireIndex, toFire / scale);
            add(chaosIndex, toChaos / scale);
            add(from, std::max(0.0, 1.0 - total / scale));
        }
    }

    void RestrictToFire(DamagePacket& damage) noexcept
    {
        const auto fireIndex = static_cast<std::size_t>(DamageType::Fire);
        for (std::size_t type = 0; type < damage.byType.size(); ++type) {
            if (type != fireIndex) damage.byType[type] = {};
        }
        for (std::size_t element = 1; element < damage.randomElementExtra.size(); ++element) {
            damage.randomElementExtra[element] = {};
            damage.targetAilmentExtra[element] = {};
        }
    }

    void ScaleDamagePacket(DamagePacket& damage, const DamageTypeValues& multipliers) noexcept
    {
        const auto scale = [](DamageRange& range, double factor) {
            range.min *= factor;
            range.max *= factor;
        };
        for (std::size_t i = 0; i < damage.byType.size(); ++i) {
            scale(damage.byType[i], multipliers[i]);
        }
        const auto fireIndex = static_cast<std::size_t>(DamageType::Fire);
        for (std::size_t i = 0; i < damage.randomElementExtra.size(); ++i) {
            scale(damage.randomElementExtra[i], multipliers[fireIndex + i]);
            scale(damage.targetAilmentExtra[i], multipliers[fireIndex + i]);
        }
    }

    void ConvertPassiveHitDamage(DamagePacket& damage, const PassiveStatTotals& passives, bool isFireSpell,
        const ExtraDamageStats& extraDamage, const DamageTypeValues& increasedPct, const DamageTypeValues& moreMultipliers,
        bool isAttack, TargetAilmentHitDamage* targetHit) noexcept
    {
        ConvertGlobalHitDamage(damage, passives, isFireSpell);
        // Gain uses the converted base ranges simultaneously. An extra result cannot itself feed another gain.
        const auto converted = damage.byType;
        DamageRange total;
        for (std::size_t from = 0; from < converted.size(); ++from) {
            const auto& source = converted[from];
            total.min += source.min;
            total.max += source.max;
            for (std::size_t to = 0; to < converted.size(); ++to) {
                const double skillGain = isAttack ? extraDamage.attackGain[to] : extraDamage.spellGain[to];
                const double gain = std::max(0.0, extraDamage.gain[from][to] + skillGain) / 100.0;
                damage.byType[to].min += source.min * gain;
                damage.byType[to].max += source.max * gain;
            }
        }
        const double randomGain = std::max(0.0, extraDamage.randomElementGainPct) / 100.0;
        const auto addAgainstAilment = [&](std::size_t destination, DamageType source, double percent) {
            const auto& base = converted[static_cast<std::size_t>(source)];
            const double factor = percent / 100.0;
            damage.targetAilmentExtra[destination] = { base.min * factor, base.max * factor };
        };
        addAgainstAilment(0, DamageType::Cold, extraDamage.coldAsFireAgainstFrozenPct);
        addAgainstAilment(1, DamageType::Physical, extraDamage.physicalAsColdAgainstShockedPct);
        addAgainstAilment(2, DamageType::Physical, extraDamage.physicalAsLightningAgainstChilledPct);
        for (auto& extra : damage.randomElementExtra) {
            extra = { total.min * randomGain, total.max * randomGain };
        }
        if (passives.dealOnlyFire) {
            RestrictToFire(damage);
        }
        ScaleDamagePacket(damage, moreMultipliers);
        if (targetHit) {
            *targetHit = {
                .againstElementalAilmentsPct = passives.hitDamageIncAgainstElementalAilmentsPct,
                .againstAnyAilmentsPct = passives.hitDamageIncAgainstAnyAilmentsPct,
                .againstIgnitedPct = passives.hitDamageIncAgainstIgnitedPct,
                .againstBurningPct = passives.hitDamageIncAgainstBurningPct,
                .againstFrozenPct = passives.hitDamageIncAgainstFrozenPct,
            };
            if (targetHit->HasModifiers()) {
                targetHit->basis = damage;
                targetHit->increasedPct = increasedPct;
            }
        }
        DamageTypeValues increaseMultipliers{};
        for (std::size_t type = 0; type < increasedPct.size(); ++type) {
            increaseMultipliers[type] = std::max(0.0, 1.0 + increasedPct[type] / 100.0);
        }
        ScaleDamagePacket(damage, increaseMultipliers);
    }

    bool HasRandomElementExtra(const DamagePacket& damage) noexcept
    {
        return std::ranges::any_of(damage.randomElementExtra, [](const auto& r) { return r.max > 0.0; });
    }

    DamagePacket ResolveRandomElementExtra(const DamagePacket& damage, std::size_t element) noexcept
    {
        DamagePacket result = damage;
        if (element < damage.randomElementExtra.size()) {
            auto& range = result.byType[static_cast<std::size_t>(DamageType::Fire) + element];
            range.min += damage.randomElementExtra[element].min;
            range.max += damage.randomElementExtra[element].max;
        }
        result.randomElementExtra = {};
        return result;
    }

    RegenerationSplit SplitLifeRegeneration(double recovery, double missingLife, bool excessToEnergyShield) noexcept
    {
        const double amount = std::max(0.0, recovery);
        const double restored = std::min(amount, std::max(0.0, missingLife));
        return { restored, excessToEnergyShield ? amount - restored : 0.0 };
    }

    PlayerLifeConditions GetPlayerLifeConditions(double currentLife, double permanentMaxLife,
        double currentMaxLife, const StatSnapshot& snapshot) noexcept
    {
        const double lowLifeMax = snapshot.poe2Passives ? currentMaxLife : permanentMaxLife;
        const double lowLifeFraction = snapshot.poe2Passives ? ActiveRuleValues().lowLifeFraction : 0.50;
        return {
            !snapshot.chaosInoculation && lowLifeMax > 0.0 && currentLife <= lowLifeMax * lowLifeFraction,
            snapshot.poe2Passives && currentMaxLife > 0.0 && currentLife >= currentMaxLife
        };
    }

    RegenerationSplit ConvertLifeRegeneration(double recoveryPoe, double currentLife, double currentMaxLife,
        double skyrimMaximumEnergyShield, const StatSnapshot& snapshot) noexcept
    {
        if (currentMaxLife <= 0.0 || snapshot.poeMaxLife <= 0.0) return {};
        const double lifePerPoint = currentMaxLife / snapshot.poeMaxLife;
        const auto split = SplitLifeRegeneration(recoveryPoe, (currentMaxLife - currentLife) / lifePerPoint,
            snapshot.excessLifeRegenToEs);
        const double esPerPoint = snapshot.poeMaxEnergyShield > 0.0
            ? skyrimMaximumEnergyShield / snapshot.poeMaxEnergyShield : 0.0;
        return { split.life * lifePerPoint, split.energyShield * esPerPoint };
    }

    double BlockedHitDamageFraction(const StatSnapshot& snapshot) noexcept
    {
        return std::clamp(snapshot.passiveAilments.blockedHitDamageTakenPct / 100.0, 0.0, 1.0);
    }

    double PlayerDamageBypassingEnergyShield(double damage, const StatSnapshot& snapshot) noexcept
    {
        return damage * std::clamp(snapshot.passiveAilments.energyShieldBypassPct / 100.0, 0.0, 1.0);
    }

    double BlockChanceLimit(double addedMaxPercent) noexcept
    {
        const auto& rule = ActiveRuleValues().block;
        return std::clamp(rule.baseMaxPercent + addedMaxPercent, 0.0, rule.capPercent);
    }

    double SpellHitBlockChance(const StatSnapshot& snapshot, bool isProjectile) noexcept
    {
        return isProjectile ? std::max(snapshot.attackBlockChance, snapshot.spellBlockChance) : snapshot.spellBlockChance;
    }

    AilmentKind EnergyShieldBypassAilment(AilmentKind ailment, bool shavronneChaosEs) noexcept
    {
        // Shavronne prevents poison's bypass. Chaos still consumes double ES.
        return shavronneChaosEs && ailment == AilmentKind::Poison ? AilmentKind::Count : ailment;
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
        const double pArmour = ArmourPassRatio(row.bestArmour * mult, h);
        const double pEvasion = EvasionHitChance(row.bestEvasion * mult, acc);
        return (pArmour + pEvasion + 1.0) / 3.0;
    }

    double ExpectedElementalPass(
        std::uint32_t exileLevel,
        const DataPack& pack,
        const Config& config) noexcept
    {
        const auto lv = ClampLevel(exileLevel);
        const auto& row = pack.ExpectedDefense(lv);
        const auto& resistance = ActiveRuleValues().resistance;
        const double res = std::min(resistance.baseMaxPercent, std::max(0.0, config.expectedResistAffixes) * row.bestResistTier);
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
        std::uint32_t startingStrength,
        const Config& config) noexcept
    {
        return ActiveRuleValues().life.Total(ClampLevel(exileLevel), startingStrength, 0.0,
            std::max(0.0, config.expectedLifeIncrease) * 100.0);
    }

    double ExpectedMana(
        std::uint32_t exileLevel,
        std::uint32_t startingIntelligence,
        const Config& config) noexcept
    {
        return ActiveRuleValues().mana.Total(ClampLevel(exileLevel), startingIntelligence, 0.0,
            std::max(0.0, config.expectedManaIncrease) * 100.0);
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
        const double poePerSkyrim = c.monsterDamageMult * pack.MonsterDamage(lv) / dref;
        const double h = skyrimPreArmorD * poePerSkyrim;

        const double pass = ArmourPassRatio(snapshot.armour, h, snapshot.physDamageReduction,
            snapshot.passiveAilments.maximumPhysicalReductionPct);

        const double meL = SafePositive(ExpectedPhysicalPass(lv, pack, config), 1.0);
        const double me1 = SafePositive(ExpectedPhysicalPass(1, pack, config), 1.0);
        const double denom = std::pow(meL, config.defenseRollback) * std::pow(me1, 1.0 - config.defenseRollback);
        // Astramentis modifies post-armour damage in PoE units. Invert the same
        // hit scale before applying the expected-defence normalization to both terms.
        const double flatSkyrim = poePerSkyrim > 0.0 ? snapshot.flatPhysTakenFromAttacksDelta / poePerSkyrim : 0.0;
        const double out = (skyrimPreArmorD * pass + flatSkyrim) / SafePositive(denom, 1.0);
        return (std::isfinite(out) && out > 0.0) ? out : 0.0;
    }

    double ConvertDefenseResistDamage(
        double skyrimPreResistD,
        DamageType damageType,
        std::uint32_t exileLevel,
        const StatSnapshot& snapshot,
        const DataPack& pack,
        const Config& config,
        bool isHit) noexcept
    {
        if (!std::isfinite(skyrimPreResistD) || skyrimPreResistD <= 0.0) {
            return 0.0;
        }
        const auto lv = ClampLevel(exileLevel);
        const auto& resistance = ActiveRuleValues().resistance;
        double resist = 0.0;
        double maxResist;
        double meL = 1.0;
        double me1 = 1.0;
        switch (damageType) {
            case DamageType::Fire:
                resist = snapshot.fireResist;
                maxResist = snapshot.maxFireResist;
                meL = ExpectedElementalPass(lv, pack, config);
                me1 = ExpectedElementalPass(1, pack, config);
                break;
            case DamageType::Cold:
                resist = snapshot.coldResist;
                maxResist = snapshot.maxColdResist;
                meL = ExpectedElementalPass(lv, pack, config);
                me1 = ExpectedElementalPass(1, pack, config);
                break;
            case DamageType::Lightning:
                resist = snapshot.lightningResist;
                maxResist = snapshot.maxLightningResist;
                meL = ExpectedElementalPass(lv, pack, config);
                me1 = ExpectedElementalPass(1, pack, config);
                break;
            case DamageType::Chaos:
                if (snapshot.chaosImmune) {
                    return 0.0;
                }
                resist = snapshot.chaosResist;
                maxResist = snapshot.maxChaosResist;
                meL = ExpectedChaosPass(lv, pack, config);
                me1 = ExpectedChaosPass(1, pack, config);
                break;
            default:
                return skyrimPreResistD;
        }
        const double cap = std::clamp(std::isnan(maxResist) ? resistance.baseMaxPercent : maxResist,
            0.0, resistance.capPercent);
        double pass = std::max(0.0, 1.0 - std::min(cap, resist) / 100.0);
        const double armourFraction = snapshot.armourAppliesPct[static_cast<std::size_t>(damageType)] / 100.0;
        if (isHit && armourFraction > 0.0) {
            const double hitPoe = skyrimPreResistD * pass * pack.Constants().monsterDamageMult *
                pack.MonsterDamage(lv) / SafePositive(config.defenseDref, 10.7);
            pass *= ArmourPassRatio(snapshot.armour * armourFraction, hitPoe, 0.0);
        }
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
        double expectedPoeLife,
        bool chaosInoculation) noexcept
    {
        const double base = SafePositive(baseSkyrimMaxHealth, 100.0);
        if (chaosInoculation) {
            // 3단계 검수 C6: 스카이림 낙하·환경·물리 즉사 사고를 피하기 위한 CI 최소 안전 체력 하한
            return std::max(1.0, std::min(25.0, base * 0.15));
        }
        const double expLife = SafePositive(expectedPoeLife, ActiveRuleValues().life.At(1));
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
        double expectedPoeLife) noexcept
    {
        if (!std::isfinite(poeMaxEnergyShield) || poeMaxEnergyShield <= 0.0) {
            return 0.0;
        }
        const double base = SafePositive(baseSkyrimMaxHealth, 100.0);
        const double expLife = SafePositive(expectedPoeLife, ActiveRuleValues().life.At(1));
        return std::max(0.0, base * (poeMaxEnergyShield / expLife));
    }

    MagickaScalingResult ComputeMagickaScaling(
        double baseSkyrimMaxMagicka,
        double poeMaxMana,
        double poeSkillCost,
        double expectedPoeMana) noexcept
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
        const double expMana = SafePositive(expectedPoeMana, ActiveRuleValues().mana.At(1));
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
        double currentEnergyShield,
        AilmentKind ailment) noexcept
    {
        EnergyShieldAbsorbResult res;
        const double es = (std::isfinite(currentEnergyShield) && currentEnergyShield > 0.0) ? currentEnergyShield : 0.0;
        res.newEnergyShield = es;

        if (!std::isfinite(rawPostDefenseDamage) || rawPostDefenseDamage <= 0.0) {
            res.remainingRawDamage = 0.0;
            return res;
        }
        const auto& rule = ActiveRuleValues().energyShield;
        if (es <= 0.0 || (ailment == AilmentKind::Bleed && rule.bleedBypasses) ||
            (ailment == AilmentKind::Poison && rule.poisonBypasses)) {
            res.remainingRawDamage = rawPostDefenseDamage;
            return res;
        }

        const double diff = SafePositive(difficultyToPcMult, 1.0);
        const double effectiveDmg = rawPostDefenseDamage * diff;
        const double consumption = isChaos ? rule.chaosConsumptionMultiplier : rule.normalConsumptionMultiplier;
        const double shieldSpent = std::min(es, effectiveDmg * consumption);
        const double absorbed = shieldSpent / consumption;
        res.newEnergyShield = es - shieldSpent;
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
