#include "core/CharmSystem.h"
#include "core/ContentFeatures.h"

#include <algorithm>
#include <cmath>

namespace SkyrimPoE::Core
{
    void CharmSystem::SetSlots(const std::array<CharmSlotState, kMaxCharmSlots>& slots, std::uint8_t unlockedSlots)
    {
        std::scoped_lock lk(mutex_);
        slots_ = slots;
        for (auto& s : slots_) {
            if (!std::isfinite(s.charges)) {
                s.charges = 0.0;
            }
            s.charges = std::clamp(s.charges, 0.0, kMaxCharges);
        }
        unlockedSlots_ = std::min<std::uint8_t>(unlockedSlots, static_cast<std::uint8_t>(kMaxCharmSlots));
    }

    void CharmSystem::SetSlot(std::uint8_t slotIndex, const CharmSlotState& state)
    {
        if (slotIndex >= kMaxCharmSlots) {
            return;
        }
        std::scoped_lock lk(mutex_);
        slots_[slotIndex] = state;
        if (!std::isfinite(slots_[slotIndex].charges)) {
            slots_[slotIndex].charges = 0.0;
        }
        slots_[slotIndex].charges = std::clamp(slots_[slotIndex].charges, 0.0, kMaxCharges);
    }

    void CharmSystem::SetUnlockedSlots(std::uint8_t unlockedSlots)
    {
        std::scoped_lock lk(mutex_);
        unlockedSlots_ = std::min<std::uint8_t>(unlockedSlots, static_cast<std::uint8_t>(kMaxCharmSlots));
    }

    void CharmSystem::ResetOnRevert()
    {
        std::scoped_lock lk(mutex_);
        slots_ = {};
        unlockedSlots_ = 0;
        activeTimers_ = {};
    }

    std::array<CharmSlotState, kMaxCharmSlots> CharmSystem::Slots() const
    {
        std::scoped_lock lk(mutex_);
        return slots_;
    }

    std::uint8_t CharmSystem::UnlockedSlots() const
    {
        std::scoped_lock lk(mutex_);
        return unlockedSlots_;
    }

    std::array<double, kCharmKindCount> CharmSystem::ActiveTimers() const
    {
        std::scoped_lock lk(mutex_);
        return activeTimers_;
    }

    bool CharmSystem::TryConsumeAndActivateLocked(CharmKind targetKind) noexcept
    {
        // 결정 239·248: 세대 1 패키지는 P5까지 참을 발동하지 않는다(세 발동 경로가 모두 여기를 지난다).
        if (!ActiveContentFeatures().charms) {
            triggerGateRejections_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const auto kIdx = static_cast<std::size_t>(targetKind);
        if (kIdx >= kCharmKindCount) {
            return false;
        }
        // 이미 활성 중이면 중복 소모 방지
        if (activeTimers_[kIdx] > 0.0) {
            return false;
        }
        const std::size_t limit = std::min<std::size_t>(unlockedSlots_, kMaxCharmSlots);
        for (std::size_t i = 0; i < limit; ++i) {
            auto& slot = slots_[i];
            if (slot.occupied && slot.kind == targetKind && slot.charges + 1e-6 >= kChargesPerUse) {
                slot.charges = std::max(0.0, slot.charges - kChargesPerUse);
                activeTimers_[kIdx] = kDurationSec;
                return true;
            }
        }
        return false;
    }

    bool CharmSystem::TryTriggerOnIncomingDamage(DamageType type, bool enableCharms)
    {
        std::scoped_lock lk(mutex_);
        if (!enableCharms || unlockedSlots_ == 0) {
            return false;
        }
        triggerEntries_.fetch_add(1, std::memory_order_relaxed);
        switch (type) {
            case DamageType::Fire:      return TryConsumeAndActivateLocked(CharmKind::Ruby);
            case DamageType::Cold:      return TryConsumeAndActivateLocked(CharmKind::Sapphire);
            case DamageType::Lightning: return TryConsumeAndActivateLocked(CharmKind::Topaz);
            case DamageType::Chaos:     return TryConsumeAndActivateLocked(CharmKind::Amethyst);
            default:                    return false;
        }
    }

    bool CharmSystem::TryTriggerOnAilment(AilmentKind kind, bool enableCharms)
    {
        std::scoped_lock lk(mutex_);
        if (!enableCharms || unlockedSlots_ == 0) {
            return false;
        }
        triggerEntries_.fetch_add(1, std::memory_order_relaxed);
        if (kind == AilmentKind::Freeze || kind == AilmentKind::Chill || kind == AilmentKind::Bleed) {
            return TryConsumeAndActivateLocked(CharmKind::Purifying);
        }
        return false;
    }

    bool CharmSystem::TryTriggerOnLowLife(double projectedHealthRatio, bool enableCharms)
    {
        std::scoped_lock lk(mutex_);
        if (!enableCharms || unlockedSlots_ == 0) {
            return false;
        }
        triggerEntries_.fetch_add(1, std::memory_order_relaxed);
        if (!std::isfinite(projectedHealthRatio) || projectedHealthRatio > kLowLifeThresholdRatio) {
            return false;
        }
        return TryConsumeAndActivateLocked(CharmKind::Bastion);
    }

    void CharmSystem::OnKillRecharge(MonsterRarity rarity, bool isBoss, bool isDragon)
    {
        if (!ActiveContentFeatures().charms) {
            rechargeGateRejections_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::scoped_lock lk(mutex_);
        double gain = 2.0;
        if (rarity == MonsterRarity::Magic) gain = 4.0;
        else if (rarity == MonsterRarity::Rare) gain = 8.0;
        if (isBoss || isDragon) {
            gain = std::max(gain, 15.0);
        }

        const std::size_t limit = std::min<std::size_t>(unlockedSlots_, kMaxCharmSlots);
        for (std::size_t i = 0; i < limit; ++i) {
            if (slots_[i].occupied) {
                slots_[i].charges = std::min(kMaxCharges, slots_[i].charges + gain);
            }
        }
    }

    bool CharmSystem::Tick(double deltaSec, bool gamePaused)
    {
        std::scoped_lock lk(mutex_);
        if (gamePaused || !std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return false;
        }
        bool anyExpired = false;
        for (auto& t : activeTimers_) {
            if (t > 0.0) {
                t -= deltaSec;
                if (t <= 0.0) {
                    t = 0.0;
                    anyExpired = true;
                }
            }
        }
        return anyExpired;
    }

    CharmActiveBonuses CharmSystem::ComputeActiveBonuses() const
    {
        CharmActiveBonuses b{};
        // 이미 켜져 있던 타이머가 남아도 세대 1에서는 보너스·면역을 주지 않는다.
        if (!ActiveContentFeatures().charms) {
            return b;
        }
        std::scoped_lock lk(mutex_);
        auto isOn = [&](CharmKind k) {
            return activeTimers_[static_cast<std::size_t>(k)] > 0.0;
        };
        if (isOn(CharmKind::Ruby)) {
            b.fireResist += 25.0;
            b.maxFireResist += 2.0;
            b.activeMask |= static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(CharmKind::Ruby));
        }
        if (isOn(CharmKind::Sapphire)) {
            b.coldResist += 25.0;
            b.maxColdResist += 2.0;
            b.activeMask |= static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(CharmKind::Sapphire));
        }
        if (isOn(CharmKind::Topaz)) {
            b.lightningResist += 25.0;
            b.maxLightningResist += 2.0;
            b.activeMask |= static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(CharmKind::Topaz));
        }
        if (isOn(CharmKind::Amethyst)) {
            b.chaosResist += 25.0;
            b.activeMask |= static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(CharmKind::Amethyst));
        }
        if (isOn(CharmKind::Purifying)) {
            b.freezeImmune = true;
            b.bleedImmune = true;
            b.activeMask |= static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(CharmKind::Purifying));
        }
        if (isOn(CharmKind::Bastion)) {
            b.physDamageReduction += 15.0;
            b.flatArmour += 600.0;
            b.activeMask |= static_cast<std::uint8_t>(1u << static_cast<std::uint8_t>(CharmKind::Bastion));
        }
        return b;
    }

    CharmSystem::GateStats CharmSystem::GetGateStatsForTest() const noexcept
    {
        return {
            triggerEntries_.load(std::memory_order_relaxed),
            triggerGateRejections_.load(std::memory_order_relaxed),
            rechargeGateRejections_.load(std::memory_order_relaxed),
        };
    }
}
