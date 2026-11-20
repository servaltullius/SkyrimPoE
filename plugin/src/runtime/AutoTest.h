#pragma once

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/EnergyShield.h"
#include "core/ItemData.h"
#include "core/KillTracker.h"
#include "core/PassiveTreeData.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"
#include "core/Types.h"

namespace SkyrimPoE::Runtime
{
    // 마커 파일(<SKSE 로그 폴더>/SkyrimPoE.autotest)이 있을 때만 활성화되는
    // 2~5단계 시나리오 A·B·C·D·E·F 자동 통합 시험 하네스(총 45건).
    // 마커 파일이 없으면 Install에서 비활성화되어 일반 플레이에 영향과 비용이 0이다.
    class AutoTest
    {
    public:
        static void Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            Core::EnergyShieldPool* esPool,
            Core::KillTracker* killTracker,
            const Core::DataPack* dataPack,
            const Core::PassiveTreeData* treeData,
            const Core::ItemData* itemData,
            Core::Config* config,
            Core::SkillSystem* skillSystem = nullptr,
            Core::CharmSystem* charmSystem = nullptr);

        [[nodiscard]] static bool IsEnabled() noexcept;

        // 주 갱신 스레드 프레임 콜백에서 호출(비활성 시 즉시 반환)
        static void Tick(RE::Main* main, double deltaSec);

        // SKSE 메시징 콜백에서 호출(비활성 시 즉시 반환, 원자/뮤텍스 플래그만 갱신)
        static void OnPreLoadGame();
        static void OnPostLoadGame(bool succeeded);
        static void OnSaveGame(const SKSE::MessagingInterface::Message& msg);
    };
}
