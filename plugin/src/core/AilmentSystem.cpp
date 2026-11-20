#include "core/AilmentSystem.h"

#include <algorithm>
#include <cmath>

namespace SkyrimPoE::Core
{
    namespace
    {
        double ComputePoEThresholdEffect(double hitDmg, double threshold, double cap, double minVal, bool guaranteedMin) noexcept
        {
            if (hitDmg <= 0.0 || threshold <= 0.0) {
                return 0.0;
            }
            const double raw = 0.50 * std::pow(hitDmg / threshold, 0.4);
            if (raw < minVal) {
                return guaranteedMin ? minVal : 0.0;
            }
            return std::min(raw, cap);
        }
    }

    AilmentApplyResult AilmentSystem::ApplyHitAilments(std::uint32_t targetActorId, const HitAilmentInput& in)
    {
        AilmentApplyResult res{};
        if (targetActorId == 0) {
            return res;
        }
        auto safePos = [](double v) noexcept {
            return (std::isfinite(v) && v > 0.0) ? v : 0.0;
        };
        const double physHit = safePos(in.physHitPoe);
        const double fireHit = safePos(in.fireHitPoe);
        const double coldHit = safePos(in.coldHitPoe);
        const double lightHit = safePos(in.lightningHitPoe);
        const double chaosHit = safePos(in.chaosHitPoe);
        const double threshold = std::max(1.0, safePos(in.targetAilmentThresholdPoe));
        const double rollPct = std::clamp(std::isfinite(in.roll01) ? in.roll01 : 0.5, 0.0, 1.0) * 100.0;
        auto rollsHit = [&](double chancePct) noexcept {
            return std::isfinite(chancePct) && (chancePct >= 100.0 || (chancePct > 0.0 && rollPct < chancePct));
        };

        auto& st = states_[targetActorId];

        if (!in.targetElementalImmune) {
            // 1) 점화(Ignite): 화염 치명타 또는 점화 확률 → 4초간 초당 화염 피해 90%(최고 1개)
            if (fireHit > 0.0 && (in.isCrit || rollsHit(in.igniteChancePct))) {
                const double dps = fireHit * 0.90;
                if (dps >= st.igniteDps || st.igniteRemaining <= 0.0) {
                    st.igniteDps = dps;
                    st.igniteRemaining = kIgniteDurationSec;
                    res.appliedIgnite = true;
                }
            }

            // 2) 냉각(Chill): 냉기 피해 비례 2초 감속(5~30%)
            if (coldHit > 0.0) {
                const double ratio = ComputePoEThresholdEffect(coldHit, threshold, 0.30, 0.05, in.isCrit);
                if (ratio >= 0.05) {
                    if (ratio >= st.chillRatio || st.chillRemaining <= 0.0) {
                        st.chillRatio = ratio;
                        st.chillRemaining = kChillDurationSec;
                        res.appliedChill = true;
                    }
                }
            }

            // 3) 빙결(Freeze): 냉기 치명타/확률 → 일반 0.3~1.5초, 보스·드래곤은 최대 냉각(30%)으로 안전 완화
            // 검수 m3: PoE1과 동일하게 치명타는 빙결 '확률' 100%만 부여하며 rawDur < 0.3s 극미량 피해는 무시
            if (coldHit > 0.0 && !in.targetFreezeImmune && (in.isCrit || rollsHit(in.freezeChancePct))) {
                const double rawDur = 6.0 * (coldHit / threshold);
                if (rawDur >= 0.3) {
                    if (in.isBossOrDragon) {
                        if (0.30 >= st.chillRatio || st.chillRemaining <= 0.0) {
                            st.chillRatio = std::max(st.chillRatio, 0.30);
                            st.chillRemaining = std::max(st.chillRemaining, kChillDurationSec);
                            res.appliedChill = true;
                        }
                    } else {
                        const double capped = std::min(rawDur, 1.5);
                        if (capped > st.freezeRemaining) {
                            st.freezeRemaining = capped;
                            res.appliedFreeze = true;
                        }
                    }
                }
            }

            // 4) 감전(Shock): 번개 피해(또는 Starforge 물리 피해) 치명타/확률 → 2초간 받는 피해 +5~50%
            const double shockBase = lightHit + (in.starforgeShock ? physHit : 0.0);
            if (shockBase > 0.0 && (in.isCrit || rollsHit(in.shockChancePct))) {
                const double ratio = ComputePoEThresholdEffect(shockBase, threshold, 0.50, 0.05, in.isCrit);
                if (ratio >= 0.05) {
                    if (ratio >= st.shockRatio || st.shockRemaining <= 0.0) {
                        st.shockRatio = ratio;
                        st.shockRemaining = kShockDurationSec;
                        res.appliedShock = true;
                    }
                }
            }
        }

        // 5) 출혈(Bleed): 물리 타격 + 출혈 확률 → 5초 DoT(이동 시 3배, PoB CalcSetup.lua:651, 최고 1개)
        if (physHit > 0.0 && !in.targetBleedImmune && rollsHit(in.bleedChancePct)) {
            const double dps = physHit * 0.70;
            if (dps >= st.bleedDps || st.bleedRemaining <= 0.0) {
                st.bleedDps = dps;
                st.bleedRemaining = kBleedDurationSec;
                res.appliedBleed = true;
            }
        }

        // 6) 중독(Poison): 물리+카오스 타격 + 중독 확률 → 2초 중첩 DoT(최대 20중첩)
        const double poisonBase = physHit + chaosHit;
        if (poisonBase > 0.0 && rollsHit(in.poisonChancePct)) {
            const double dps = poisonBase * 0.30;
            if (st.poisonStacks.size() < kMaxPoisonStacks) {
                st.poisonStacks.push_back(PoisonStack{ dps, kPoisonDurationSec });
                res.appliedPoison = true;
            } else {
                auto weakest = std::min_element(st.poisonStacks.begin(), st.poisonStacks.end(),
                    [](const PoisonStack& a, const PoisonStack& b) { return a.dps < b.dps; });
                if (weakest != st.poisonStacks.end() && dps > weakest->dps) {
                    *weakest = PoisonStack{ dps, kPoisonDurationSec };
                    res.appliedPoison = true;
                }
            }
        }

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
        bool targetElementalImmune)
    {
        if (targetActorId == 0 || targetElementalImmune) {
            return;
        }
        auto safePos = [](double v) noexcept {
            return (std::isfinite(v) && v > 0.0) ? v : 0.0;
        };
        const double safeIgDps = safePos(igniteDps);
        const double safeIgRem = safePos(igniteRemaining);
        const double safeShRatio = std::min(safePos(shockRatio), 0.50);
        const double safeShRem = safePos(shockRemaining);

        auto& st = states_[targetActorId];
        if (safeIgDps > 0.0 && safeIgRem > 0.0) {
            if (st.igniteRemaining <= 0.0 || safeIgDps > st.igniteDps) {
                st.igniteDps = safeIgDps;
                st.igniteRemaining = safeIgRem;
            } else if (safeIgDps == st.igniteDps) {
                st.igniteRemaining = std::max(st.igniteRemaining, safeIgRem);
            }
        }
        if (safeShRatio >= 0.05 && safeShRem > 0.0) {
            if (st.shockRemaining <= 0.0 || safeShRatio > st.shockRatio) {
                st.shockRatio = safeShRatio;
                st.shockRemaining = safeShRem;
            } else if (safeShRatio == st.shockRatio) {
                st.shockRemaining = std::max(st.shockRemaining, safeShRem);
            }
        }
        if (st.IsEmpty()) {
            states_.erase(targetActorId);
        }
    }

    AilmentTickDamage AilmentSystem::TickActor(std::uint32_t actorId, double deltaSec, bool isMoving)
    {
        AilmentTickDamage dmg{};
        if (!std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return dmg;
        }
        const auto it = states_.find(actorId);
        if (it == states_.end()) {
            return dmg;
        }
        auto& st = it->second;

        if (st.igniteRemaining > 0.0) {
            const double slice = std::min(deltaSec, st.igniteRemaining);
            dmg.firePoeDamage += st.igniteDps * slice;
            st.igniteRemaining = std::max(0.0, st.igniteRemaining - deltaSec);
            if (st.igniteRemaining <= 0.0) st.igniteDps = 0.0;
        }
        if (st.chillRemaining > 0.0) {
            st.chillRemaining = std::max(0.0, st.chillRemaining - deltaSec);
            if (st.chillRemaining <= 0.0) st.chillRatio = 0.0;
        }
        if (st.freezeRemaining > 0.0) {
            st.freezeRemaining = std::max(0.0, st.freezeRemaining - deltaSec);
        }
        if (st.shockRemaining > 0.0) {
            st.shockRemaining = std::max(0.0, st.shockRemaining - deltaSec);
            if (st.shockRemaining <= 0.0) st.shockRatio = 0.0;
        }
        if (st.bleedRemaining > 0.0) {
            const double slice = std::min(deltaSec, st.bleedRemaining);
            // C-10: PoB 2.68.1 Modules/CalcSetup.lua:651 이동 중 출혈 MORE 200%(즉 3배)
            const double moveMult = isMoving ? 3.0 : 1.0;
            dmg.physPoeDamage += st.bleedDps * moveMult * slice;
            st.bleedRemaining = std::max(0.0, st.bleedRemaining - deltaSec);
            if (st.bleedRemaining <= 0.0) st.bleedDps = 0.0;
        }
        for (auto pit = st.poisonStacks.begin(); pit != st.poisonStacks.end();) {
            const double slice = std::min(deltaSec, pit->remaining);
            dmg.chaosPoeDamage += pit->dps * slice;
            pit->remaining -= deltaSec;
            if (pit->remaining <= 0.0) {
                pit = st.poisonStacks.erase(pit);
            } else {
                ++pit;
            }
        }

        if (st.IsEmpty()) {
            states_.erase(it);
        }
        return dmg;
    }

    void AilmentSystem::ClearAilment(std::uint32_t actorId, AilmentKind kind)
    {
        const auto it = states_.find(actorId);
        if (it == states_.end()) return;
        auto& st = it->second;
        switch (kind) {
            case AilmentKind::Ignite: st.igniteDps = 0.0; st.igniteRemaining = 0.0; break;
            case AilmentKind::Chill:  st.chillRatio = 0.0; st.chillRemaining = 0.0; break;
            case AilmentKind::Freeze: st.freezeRemaining = 0.0; break;
            case AilmentKind::Shock:  st.shockRatio = 0.0; st.shockRemaining = 0.0; break;
            case AilmentKind::Bleed:  st.bleedDps = 0.0; st.bleedRemaining = 0.0; break;
            case AilmentKind::Poison: st.poisonStacks.clear(); break;
            default: break;
        }
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
        return 1.0 + std::clamp(it->second.shockRatio, 0.0, 0.50);
    }

    double AilmentSystem::ChillSlowRatio(std::uint32_t actorId) const noexcept
    {
        const auto it = states_.find(actorId);
        if (it == states_.end() || it->second.chillRemaining <= 0.0) {
            return 0.0;
        }
        return std::clamp(it->second.chillRatio, 0.0, 0.30);
    }

    bool AilmentSystem::IsFrozen(std::uint32_t actorId) const noexcept
    {
        const auto it = states_.find(actorId);
        return it != states_.end() && it->second.freezeRemaining > 0.0;
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
