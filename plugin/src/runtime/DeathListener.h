#pragma once

#include "core/CommandQueue.h"
#include "core/KillTracker.h"
#include "core/Types.h"

namespace SkyrimPoE::Runtime
{
    // 결정 108, 113~116, 134~140, 146~149:
    // TESDeathEvent(사망 시작 시 소환/되살림 고정, 사망 확정 시 보상 1회) 및 TESResetEvent(새 삶) 리스너.
    class DeathListener
    {
    public:
        static bool Install(
            Core::KillTracker* killTracker,
            Core::CommandQueue* commandQueue,
            const Core::Config* config);
    };
}
