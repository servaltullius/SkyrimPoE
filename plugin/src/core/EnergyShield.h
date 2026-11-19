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
        double rechargeDelay = 2.0;
        double rechargeRate = 0.33;
    };

    // 결정 67, 70, 85: 난이도 배율 일치 에너지 실드 풀 + 2초 지연 후 초당 33% 재충전.
    class EnergyShieldPool
    {
    public:
        void Configure(double rechargeDelay, double rechargeRate);
        void SetMax(double newMax, bool refillIfInitializedEmpty = false);
        void ResetFull(double newMax);
        void Clear();

        // 피격 시 ES 선차감(카오스는 ES 우회하지만 PoE 규칙대로 재충전 타이머는 리셋).
        [[nodiscard]] EnergyShieldAbsorbResult AbsorbDamage(
            double rawPostDefenseDamage,
            double difficultyToPcMult,
            bool isChaos);

        // 주 갱신 스레드에서 델타타임(게임 일시정지 시 정지)으로 재충전 진행.
        void Tick(double deltaSec, bool gamePaused);

        [[nodiscard]] EnergyShieldState Snapshot() const;

    private:
        mutable std::mutex mutex_;
        EnergyShieldState state_;
        bool initialized_ = false;
    };
}
