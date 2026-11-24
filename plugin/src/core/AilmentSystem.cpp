#include "core/AilmentSystem.h"
#include "core/RulesPack.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace SkyrimPoE::Core
{
    namespace
    {
        double Positive(double value) noexcept
        {
            return std::isfinite(value) && value > 0.0 ? value : 0.0;
        }

        constexpr std::size_t Index(PoiseKind kind) noexcept
        {
            return static_cast<std::size_t>(kind);
        }

        constexpr std::size_t Index(AilmentKind kind) noexcept
        {
            return static_cast<std::size_t>(kind);
        }

        bool RollsHit(double chancePct, double roll01) noexcept
        {
            const double rollPct = std::clamp(std::isfinite(roll01) ? roll01 : 0.5, 0.0, 1.0) * 100.0;
            return chancePct >= 100.0 || rollPct < chancePct;
        }

        bool Weaker(const AilmentInstance& left, const AilmentInstance& right) noexcept
        {
            if (left.magnitude != right.magnitude) return left.magnitude < right.magnitude;
            return left.remaining < right.remaining;
        }

        bool AddInstance(std::vector<AilmentInstance>& instances, AilmentInstance instance)
        {
            constexpr std::size_t kMaxInstances = 16;
            if (instances.size() < kMaxInstances) {
                instances.push_back(instance);
                return true;
            }
            auto weakest = std::ranges::min_element(instances, Weaker);
            if (Weaker(*weakest, instance)) {
                *weakest = instance;
                return true;
            }
            return false;
        }

        AilmentInstance Strongest(const std::vector<AilmentInstance>& instances) noexcept
        {
            return instances.empty() ? AilmentInstance{} : *std::ranges::max_element(instances, Weaker);
        }

        double DamageDuring(std::vector<AilmentInstance>& instances, double deltaSec,
            std::size_t maxActive, double movementMultiplier, bool moving)
        {
            const auto effectiveDps = [movementMultiplier, moving](const AilmentInstance& instance) {
                return instance.magnitude * ((moving || instance.aggravated) ? movementMultiplier : 1.0);
            };
            std::ranges::sort(instances, std::ranges::greater{}, effectiveDps);
            double damage = 0.0;
            for (double start = 0.0; start < deltaSec;) {
                double end = deltaSec;
                double dps = 0.0;
                std::size_t active = 0;
                for (const auto& instance : instances) {
                    if (instance.remaining <= start) continue;
                    if (active == maxActive) break;
                    ++active;
                    dps += effectiveDps(instance);
                    end = std::min(end, instance.remaining);
                }
                if (active == 0) break;
                damage += dps * (end - start);
                start = end;
            }
            return damage;
        }

        bool AddPoiseBuildup(PoiseBuildup& state, double damage, double threshold,
            double damageScale, double duration, double repeatPercent, double repeatDuration,
            const RuleValues::Poise& rule) noexcept
        {
            if (damage <= 0.0 || threshold <= 0.0 || state.activeRemaining > 0.0) {
                return false;
            }
            const double repeatMultiplier = state.repeatModifierRemaining > 0.0 ? 1.0 + repeatPercent / 100.0 : 1.0;
            const double effectiveThreshold = std::max(1.0, std::floor(threshold * repeatMultiplier));
            state.ratio += (damage / effectiveThreshold) * damageScale;
            if (state.ratio < rule.fullBuildupPercent / 100.0) {
                state.decayDelayRemaining = rule.enemyDecayDelayMilliseconds / 1000.0;
                return false;
            }
            state = PoiseBuildup{ 0.0, 0.0, duration, repeatDuration };
            return true;
        }
    }

    double ThresholdAilmentChance(double damage, double threshold, double coefficient,
        double base, double increased, double more) noexcept
    {
        if (threshold <= 0.0) {
            return 0.0;
        }
        const double chance = (damage / threshold * coefficient + base) * (1.0 + increased / 100.0) * more;
        return std::clamp(chance, 0.0, 100.0);
    }

    double RawChillPercent(double coldDamage, double ailmentThreshold) noexcept
    {
        return ailmentThreshold > 0.0
            ? (coldDamage / ailmentThreshold) * ActiveRuleValues().chill.effectMultiplier : 0.0;
    }

    DamagePacket RollSpellDamagePacket(const DamagePacket& packet, double roll01, double multiplier) noexcept
    {
        const double roll = std::clamp(std::isfinite(roll01) ? roll01 : 0.5, 0.0, 1.0);
        DamagePacket rolled;
        for (std::size_t i = 0; i < packet.byType.size(); ++i) {
            const auto& range = packet.byType[i];
            const double low = std::max(0.0, std::min(range.min, range.max));
            const double high = std::max(low, std::max(range.min, range.max));
            const double damage = (low + (high - low) * roll) * multiplier;
            rolled.byType[i] = { damage, damage };
        }
        return rolled;
    }

    bool HasElementalHitDamage(const DamagePacket& packet) noexcept
    {
        for (const auto type : { DamageType::Fire, DamageType::Cold, DamageType::Lightning }) {
            if (packet.byType[static_cast<std::size_t>(type)].max > 0.0) {
                return true;
            }
        }
        return false;
    }

    void Flammability::AddHit(double fireDamage, double ailmentThreshold, double duration, double baseChancePct)
    {
        if (fireDamage > 0.0 && ailmentThreshold > 0.0 && duration > 0.0) {
            const auto& rule = ActiveRuleValues().ignite;
            const double magnitude = ThresholdAilmentChance(fireDamage, ailmentThreshold,
                rule.chanceMultiplier, Positive(baseChancePct));
            stacks_.push_back({ magnitude, duration });
        }
    }

    void Flammability::Tick(double deltaSec)
    {
        for (auto& stack : stacks_) {
            stack.remaining -= deltaSec;
        }
        std::erase_if(stacks_, [](const Stack& stack) { return stack.remaining <= 0.0; });
    }

    double Flammability::ChancePercent() const noexcept
    {
        const double cap = ActiveRuleValues().ignite.flammabilityCapPercent;
        double chance = 0.0;
        for (const auto& stack : stacks_) {
            chance += stack.chancePercent;
            if (chance >= cap) {
                return cap;
            }
        }
        return chance;
    }

    AilmentApplyResult AilmentSystem::ApplyHitAilments(std::uint32_t targetActorId, const HitAilmentInput& in,
        double elapsedSinceTick)
    {
        const auto it = states_.find(targetActorId);
        const double lastTick = it == states_.end() ? 0.0 : it->second.lastTickSec;
        return ApplyHitAilmentsAt(targetActorId, in, lastTick + Positive(elapsedSinceTick), false);
    }

    AilmentApplyResult AilmentSystem::ApplyHitAilmentsAt(std::uint32_t targetActorId, const HitAilmentInput& in,
        double now, bool isMoving)
    {
        AilmentApplyResult res{};
        if (targetActorId == 0) {
            return res;
        }
        const auto& rules = ActiveRuleValues();
        const double physHit = Positive(in.physHitPoe);
        const double fireHit = Positive(in.fireHitPoe);
        const double coldHit = Positive(in.coldHitPoe);
        const double lightningHit = Positive(in.lightningHitPoe);
        const double poisonBase = physHit + Positive(in.chaosHitPoe);
        const double ailmentThreshold = Positive(in.targetAilmentThresholdPoe);
        const bool unique = in.isBossOrDragon || in.isMapBoss;
        const double poiseThreshold = Positive(in.targetPoiseThresholdPoe) *
            rules.poise.ThresholdMultiplier(in.targetRarity, unique, in.isMapBoss);
        constexpr double kPercentPerMinuteToRatioPerSecond = 1.0 / (100.0 * 60.0);

        auto& st = states_[targetActorId];
        st.AdvanceTo(now, isMoving);
        const auto add = [&st](AilmentKind kind, double magnitude, double duration, bool aggravated = false) {
            return AddInstance(st.instances[Index(kind)], { magnitude, duration, aggravated });
        };
        if (!in.targetElementalImmune && ailmentThreshold > 0.0) {
            if (fireHit > 0.0) {
                st.flammability.AddHit(fireHit, ailmentThreshold, rules.ignite.flammabilityEnemyDurationSeconds,
                    in.igniteChancePct);
                if (RollsHit(st.flammability.ChancePercent(), in.igniteRoll01)) {
                    const double dps = fireHit * (rules.ignite.damagePercentPerMinute * kPercentPerMinuteToRatioPerSecond);
                    res.appliedIgnite = add(AilmentKind::Ignite, dps, rules.ignite.durationSeconds);
                }
            }

            const double rawChillPercent = RawChillPercent(coldHit, ailmentThreshold);
            const double chillPercent = std::min(rawChillPercent, rules.chill.maxPercent);
            if (coldHit > 0.0 && chillPercent >= rules.chill.minPercent) {
                const double chillRatio = chillPercent / 100.0 * rules.chill.SlowEffectMultiplier(in.targetRarity, unique);
                if (chillRatio > 0.0) {
                    res.appliedChill = add(AilmentKind::Chill, chillRatio, rules.chill.enemyDurationSeconds);
                }
            }

            const double shockBase = lightningHit + (in.starforgeShock ? physHit : 0.0);
            const double shockChance = ThresholdAilmentChance(shockBase, ailmentThreshold,
                rules.shock.chanceMultiplier, Positive(in.shockChancePct));
            if (shockBase > 0.0 && RollsHit(shockChance, in.shockRoll01)) {
                res.appliedShock = add(AilmentKind::Shock, rules.shock.effectPercent / 100.0, rules.shock.enemyDurationSeconds);
            }
        }

        if (!in.targetElementalImmune) {
            if (!in.targetFreezeImmune) {
                const double effectiveColdHit = coldHit * Positive(in.coldDamageAfterResistanceMultiplier);
                res.appliedFreeze = AddPoiseBuildup(st.poise[Index(PoiseKind::Freeze)], effectiveColdHit,
                    poiseThreshold, rules.freeze.damageScale, rules.freeze.enemyDurationSeconds, 0.0, 0.0, rules.poise);
            }
            res.appliedElectrocute = AddPoiseBuildup(st.poise[Index(PoiseKind::Electrocute)], Positive(in.electrocuteHitPoe),
                poiseThreshold, rules.electrocute.damageScale, rules.electrocute.enemyDurationMilliseconds / 1000.0,
                rules.electrocute.repeatThresholdModifierPercent, rules.electrocute.repeatModifierDurationSeconds, rules.poise);
        }
        res.appliedHeavyStun = AddPoiseBuildup(st.poise[Index(PoiseKind::HeavyStun)], Positive(in.heavyStunHitPoe),
            poiseThreshold, rules.heavyStun.damageScale, rules.heavyStun.enemyDurationMilliseconds / 1000.0,
            rules.heavyStun.repeatThresholdModifierPercent, rules.heavyStun.repeatModifierDurationSeconds, rules.poise);
        res.appliedPin = AddPoiseBuildup(st.poise[Index(PoiseKind::Pin)], Positive(in.pinHitPoe),
            poiseThreshold, rules.pin.damageScale, rules.pin.enemyDurationSeconds,
            rules.pin.repeatThresholdModifierPercent, rules.pin.repeatModifierDurationSeconds, rules.poise);

        if (physHit > 0.0 && !in.targetBleedImmune && RollsHit(Positive(in.bleedChancePct), in.bleedRoll01)) {
            const double dps = physHit * (rules.bleed.damagePercentPerMinute * kPercentPerMinuteToRatioPerSecond);
            res.appliedBleed = add(AilmentKind::Bleed, dps, rules.bleed.durationSeconds, in.aggravatesBleed);
        }

        if (poisonBase > 0.0 && RollsHit(Positive(in.poisonChancePct), in.poisonRoll01)) {
            const double dps = poisonBase * (rules.poison.damagePercentPerMinute * kPercentPerMinuteToRatioPerSecond);
            res.appliedPoison = add(AilmentKind::Poison, dps, rules.poison.durationSeconds);
        }

        st.RefreshActive();
        if (st.IsEmpty()) {
            states_.erase(targetActorId);
        }
        return res;
    }

    void AilmentSystem::ProliferateIgniteAndShock(
        std::uint32_t targetActorId,
        double igniteDps,
        double igniteRemaining,
        double shockRatio,
        double shockRemaining,
        bool targetElementalImmune,
        double now,
        bool isMoving)
    {
        if (targetActorId == 0 || targetElementalImmune) {
            return;
        }
        const double safeIgDps = Positive(igniteDps);
        const double safeIgRem = Positive(igniteRemaining);
        const double safeShRatio = std::min(Positive(shockRatio), ActiveRuleValues().shock.effectPercent / 100.0);
        const double safeShRem = Positive(shockRemaining);
        if (!(safeIgDps > 0.0 && safeIgRem > 0.0) && !(safeShRatio > 0.0 && safeShRem > 0.0)) {
            return;
        }

        auto& st = states_[targetActorId];
        st.AdvanceTo(now, isMoving);
        if (safeIgDps > 0.0 && safeIgRem > 0.0) {
            AddInstance(st.instances[Index(AilmentKind::Ignite)], { safeIgDps, safeIgRem });
        }
        if (safeShRatio > 0.0 && safeShRem > 0.0) {
            AddInstance(st.instances[Index(AilmentKind::Shock)], { safeShRatio, safeShRem });
        }
        st.RefreshActive();
    }

    void AilmentSystem::TrackedActor::AdvanceTo(double now, bool isMoving)
    {
        if (!std::isfinite(now) || now <= advancedThroughSec) {
            return;
        }
        const double deltaSec = now - advancedThroughSec;
        advancedThroughSec = now;
        const auto& rules = ActiveRuleValues();

        // Integrate to the hit/tick boundary before inserting new effects. A winner
        // that expires inside this interval can hand over for the rest of the interval.
        pendingDamage.firePoeDamage += DamageDuring(instances[Index(AilmentKind::Ignite)], deltaSec, 1, 1.0, false);
        pendingDamage.physPoeDamage += DamageDuring(instances[Index(AilmentKind::Bleed)], deltaSec, 1,
            rules.bleed.movingOrAggravatedMultiplier, isMoving);
        pendingDamage.chaosPoeDamage += DamageDuring(instances[Index(AilmentKind::Poison)], deltaSec,
            rules.poison.baseMaxStacks, 1.0, false);
        for (auto& group : instances) {
            for (auto& instance : group) {
                instance.remaining -= deltaSec;
            }
            std::erase_if(group, [](const AilmentInstance& instance) { return instance.remaining <= 0.0; });
        }
        for (auto& buildup : poise) {
            const double decaySeconds = std::max(0.0, deltaSec - buildup.decayDelayRemaining);
            buildup.decayDelayRemaining = std::max(0.0, buildup.decayDelayRemaining - deltaSec);
            buildup.ratio = std::max(0.0, buildup.ratio - decaySeconds * rules.poise.enemyDecayPercentPerSecond / 100.0);
            buildup.activeRemaining = std::max(0.0, buildup.activeRemaining - deltaSec);
            buildup.repeatModifierRemaining = std::max(0.0, buildup.repeatModifierRemaining - deltaSec);
        }
        flammability.Tick(deltaSec);
    }

    void AilmentSystem::TrackedActor::RefreshActive()
    {
        const auto ignite = Strongest(instances[Index(AilmentKind::Ignite)]);
        igniteDps = ignite.magnitude;
        igniteRemaining = ignite.remaining;

        const auto chill = Strongest(instances[Index(AilmentKind::Chill)]);
        chillRatio = chill.magnitude;
        chillRemaining = chill.remaining;

        const auto shock = Strongest(instances[Index(AilmentKind::Shock)]);
        shockRatio = shock.magnitude;
        shockRemaining = shock.remaining;

        const auto bleed = Strongest(instances[Index(AilmentKind::Bleed)]);
        bleedDps = bleed.magnitude;
        bleedRemaining = bleed.remaining;
        bleedAggravated = bleed.aggravated;
        activePoisonCount = std::min(instances[Index(AilmentKind::Poison)].size(),
            static_cast<std::size_t>(ActiveRuleValues().poison.baseMaxStacks));
    }

    bool AilmentSystem::TrackedActor::IsEmpty() const noexcept
    {
        return pendingDamage.TotalPoe() <= 0.0 && flammability.IsEmpty() &&
            std::ranges::all_of(poise, &PoiseBuildup::IsEmpty) &&
            std::ranges::all_of(instances, [](const auto& group) { return group.empty(); });
    }

    void AilmentSystem::AdvanceActorTo(std::uint32_t actorId, double now, bool isMoving)
    {
        const auto it = states_.find(actorId);
        if (it == states_.end()) return;
        it->second.AdvanceTo(now, isMoving);
        it->second.RefreshActive();
        if (it->second.IsEmpty()) {
            states_.erase(it);
        }
    }

    AilmentTickDamage AilmentSystem::TickActor(std::uint32_t actorId, double deltaSec, bool isMoving)
    {
        const auto it = states_.find(actorId);
        if (it == states_.end() || Positive(deltaSec) <= 0.0) return {};
        return TickActorAt(actorId, it->second.lastTickSec + deltaSec, isMoving);
    }

    AilmentTickDamage AilmentSystem::TickActorAt(std::uint32_t actorId, double now, bool isMoving)
    {
        const auto it = states_.find(actorId);
        if (it == states_.end()) return {};
        auto& st = it->second;
        st.AdvanceTo(now, isMoving);
        st.lastTickSec = now;
        st.RefreshActive();
        const auto damage = std::exchange(st.pendingDamage, {});
        if (st.IsEmpty()) {
            states_.erase(it);
        }
        return damage;
    }

    void AilmentSystem::ClearAilment(std::uint32_t actorId, AilmentKind kind)
    {
        const auto it = states_.find(actorId);
        if (it == states_.end()) return;
        auto& st = it->second;
        if (kind == AilmentKind::Freeze) {
            st.poise[Index(PoiseKind::Freeze)] = {};
        } else if (Index(kind) < st.instances.size()) {
            st.instances[Index(kind)].clear();
        }
        st.RefreshActive();
        if (st.IsEmpty()) {
            states_.erase(it);
        }
    }

    void AilmentSystem::RemoveActor(std::uint32_t actorId)
    {
        states_.erase(actorId);
    }

    void AilmentSystem::ClearAll()
    {
        states_.clear();
    }

    double AilmentSystem::ShockDamageMultiplier(std::uint32_t actorId) const noexcept
    {
        const auto it = states_.find(actorId);
        if (it == states_.end() || it->second.shockRemaining <= 0.0) {
            return 1.0;
        }
        return 1.0 + it->second.shockRatio;
    }

    double AilmentSystem::ChillSlowRatio(std::uint32_t actorId) const noexcept
    {
        const auto it = states_.find(actorId);
        if (it == states_.end() || it->second.chillRemaining <= 0.0) {
            return 0.0;
        }
        return it->second.chillRatio;
    }

    bool AilmentSystem::IsChilledOrFrozen(std::uint32_t actorId) const noexcept
    {
        return IsFrozen(actorId) || ChillSlowRatio(actorId) > 0.0;
    }

    double AilmentSystem::FlammabilityChancePercent(std::uint32_t actorId) const noexcept
    {
        const auto* state = GetState(actorId);
        return state ? state->flammability.ChancePercent() : 0.0;
    }

    bool AilmentSystem::IsFrozen(std::uint32_t actorId) const noexcept
    {
        const auto it = states_.find(actorId);
        return it != states_.end() && it->second.poise[Index(PoiseKind::Freeze)].activeRemaining > 0.0;
    }

    const ActorAilmentState* AilmentSystem::GetState(std::uint32_t actorId) const noexcept
    {
        const auto it = states_.find(actorId);
        return it == states_.end() ? nullptr : &it->second;
    }

    std::vector<std::uint32_t> AilmentSystem::ActiveActorIds() const
    {
        std::vector<std::uint32_t> out;
        out.reserve(states_.size());
        for (const auto& [id, _] : states_) {
            out.push_back(id);
        }
        return out;
    }
}
