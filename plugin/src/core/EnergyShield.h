#pragma once

#include "core/DamageConversion.h"

#include <mutex>

namespace SkyrimPoE::Core
{
    struct EnergyShieldState
    {
        double current = 0.0;
        double max = 0.0;
        double timeSinceLastHit = 0.0;
        double rechargeDelay = 0.0;
        double rechargeRate = 0.0;
    };

    // 난이도 배율을 반영하는 플레이어 ES 풀. 재충전은 ES가 줄어든 뒤 지연된다.
    class EnergyShieldPool
    {
    public:
        void Configure(double rechargeDelayMultiplier = 1.0, double rechargeRateMultiplier = 1.0,
            bool rechargeEnabled = true);
        void SetMax(double newMax, bool refillIfInitializedEmpty = false);
        void ResetFull(double newMax);
        void Clear();
        void Recover(double amount);

        // ES에 들어간 피해만 재충전 지연을 새로 시작한다.
        [[nodiscard]] EnergyShieldAbsorbResult AbsorbDamage(
            double rawPostDefenseDamage,
            double difficultyToPcMult,
            bool isChaos,
            AilmentKind ailment = AilmentKind::Count);

        // 주 갱신 스레드에서 델타타임(게임 일시정지 시 정지)으로 재충전 진행.
        void Tick(double deltaSec, bool gamePaused);

        [[nodiscard]] EnergyShieldState Snapshot() const;

    private:
        mutable std::mutex mutex_;
        EnergyShieldState state_;
        bool initialized_ = false;
    };

    [[nodiscard]] EnergyShieldPool& GetPlayerEnergyShield() noexcept;
}
