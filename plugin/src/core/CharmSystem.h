#pragma once

#include "core/Types.h"

#include <array>
#include <cstdint>
#include <mutex>

namespace SkyrimPoE::Core
{
    struct CharmActiveBonuses
    {
        double fireResist = 0.0;
        double maxFireResist = 0.0;
        double coldResist = 0.0;
        double maxColdResist = 0.0;
        double lightningResist = 0.0;
        double maxLightningResist = 0.0;
        double chaosResist = 0.0;
        double physDamageReduction = 0.0;
        double flatArmour = 0.0;
        bool freezeImmune = false;
        bool bleedImmune = false;
        std::uint8_t activeMask = 0;  // bit k = CharmKind(k) 현재 활성
    };

    // 5단계(결정 162): 벨트 참(Charms, 1~3칸) 자동 발동·처치 충전·지속시간 관리 순수 엔진
    class CharmSystem
    {
    public:
        static constexpr double kMaxCharges = 40.0;
        static constexpr double kChargesPerUse = 20.0;
        static constexpr double kDurationSec = 4.0;
        static constexpr double kLowLifeThresholdRatio = 0.40;

        void SetSlots(const std::array<CharmSlotState, kMaxCharmSlots>& slots, std::uint8_t unlockedSlots);
        void SetSlot(std::uint8_t slotIndex, const CharmSlotState& state);
        void SetUnlockedSlots(std::uint8_t unlockedSlots);
        void ResetOnRevert();

        [[nodiscard]] std::array<CharmSlotState, kMaxCharmSlots> Slots() const;
        [[nodiscard]] std::uint8_t UnlockedSlots() const;
        [[nodiscard]] std::array<double, kCharmKindCount> ActiveTimers() const;

        // 원소·카오스 피격 시 해당 참 자동 발동(해당 타격 저항 계산 직전 호출)
        bool TryTriggerOnIncomingDamage(DamageType type, bool enableCharms);

        // 빙결·냉각·출혈 적용 시 정화의 참(Purifying) 자동 발동
        bool TryTriggerOnAilment(AilmentKind kind, bool enableCharms);

        // 피격 후 예상 체력 비율이 40% 이하일 때 수호의 참(Bastion) 자동 발동
        bool TryTriggerOnLowLife(double projectedHealthRatio, bool enableCharms);

        // 처치 보상 충족 적 처치 시 활성 칸의 모든 참 충전(+2/+4/+8/+15)
        void OnKillRecharge(MonsterRarity rarity, bool isBoss, bool isDragon);

        // 주 갱신 스레드 틱: 만료된 버프가 있으면 true 반환(StatEngine 재계산 신호)
        bool Tick(double deltaSec, bool gamePaused);

        [[nodiscard]] CharmActiveBonuses ComputeActiveBonuses() const;

    private:
        bool TryConsumeAndActivateLocked(CharmKind targetKind) noexcept;

        mutable std::mutex mutex_;
        std::array<CharmSlotState, kMaxCharmSlots> slots_{};
        std::uint8_t unlockedSlots_ = 0;
        std::array<double, kCharmKindCount> activeTimers_{};
    };
}
