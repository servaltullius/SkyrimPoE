#pragma once

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/KillTracker.h"
#include "core/StatEngine.h"
#include "core/Types.h"

namespace SkyrimPoE::Runtime
{
    // 결정 108, 113~116, 134~140, 146~149, 161~164:
    // TESDeathEvent(사망 시작 시 소환/되살림 고정, 사망 확정 시 보상·참 충전·전령 폭발·헤드헌터 훔치기) 및 TESResetEvent(새 삶) 리스너.
    class DeathListener
    {
    public:
        static bool Install(
            Core::KillTracker* killTracker,
            Core::CommandQueue* commandQueue,
            const Core::Config* config,
            Core::StatEngine* statEngine = nullptr,
            Core::CharmSystem* charmSystem = nullptr);

        // 끝난 지도 회차의 스폰은 첫 적대·기여 기록을 지운다. 회차가 닫힌 뒤 늦게 온 사망 확정이
        // 회차 밖(오픈월드) 처치로 보상받지 않게 한다(2026-11-23 Codex 검토).
        static void ForgetKillRecords(std::span<const RE::FormID> actorIds);

        // 결정 259(B0): 처치 시 참 충전 세대 1 게이트 거절 누적 카운터.
        struct GateStats
        {
            std::uint64_t charmRechargeGateRejections = 0;
        };
        [[nodiscard]] static GateStats GetGateStatsForTest() noexcept;
    };
}
