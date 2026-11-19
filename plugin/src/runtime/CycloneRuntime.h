#pragma once

#include "core/CommandQueue.h"
#include "core/CycloneState.h"
#include "core/DataPack.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"
#include "core/Types.h"

namespace SkyrimPoE::Runtime
{
    struct CycloneTelemetry
    {
        std::uint64_t areaHitTicks = 0;
        std::uint64_t dealDamageHits = 0;
        std::uint64_t critHits = 0;
        std::uint64_t cocTriggers = 0;
        std::uint64_t cocExecutions = 0;
        std::uint64_t cocTargetsHit = 0;
        std::uint64_t staggersSwallowed = 0;
        std::uint64_t staggersAborted = 0;
    };

    // 결정 48~51, 54, 56, 63, 97~103, 121, 122, 126~132, 145, 150:
    // 사이클론 입력 가로채기(NotifyAnimationGraph), 프렌지 양보, OAR Variable09,
    // NPC 노드 회전, 범위 근접 타격(DealDamage), 치명타 시 시전(CoC) 발동 연결.
    class CycloneRuntime
    {
    public:
        static bool Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            Core::SkillSystem* skillSystem,
            const Core::DataPack* dataPack,
            const Core::Config* config);

        // 주 갱신 스레드 프레임 콜백에서 호출
        static void Tick(double deltaSec, bool gamePaused);

        // 메뉴/로드/외부 사유로 즉시 종료(주 갱신 스레드)
        static void StopNowOnMainThread(RE::PlayerCharacter* player);

        [[nodiscard]] static bool IsActive() noexcept;
        [[nodiscard]] static bool IsYieldingToFrenzy() noexcept;
        [[nodiscard]] static CycloneTelemetry GetTelemetry() noexcept;

        // 자동 시험용(주 갱신 스레드 전용): 실제 NotifyAnimationGraph 훅 경로를 통해 채널링 시작·유지·해제
        static void ForceRefreshYieldCheckOnMainThread(RE::PlayerCharacter* player);
        static bool TryStartForAutoTest(RE::PlayerCharacter* player);
        static void SetAutoTestHeld(bool held);
    };
}
