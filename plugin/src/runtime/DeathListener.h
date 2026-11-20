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
    };
}
