#pragma once

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/EnergyShield.h"
#include "core/ItemData.h"
#include "core/PassiveTreeData.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <string>
#include <string_view>

namespace SkyrimPoE::Runtime
{
    // 결정 23, 61, 86, 120, 137(마)(바), 160:
    // - 단일 Prisma UI 뷰 상시 유지(HUD 표시 전용 + K 스킬 / L 패시브 / O 장비·제작 3탭 패널)
    // - 패널 닫힘 시 뷰를 Hide하지 않고 Unfocus만 수행해 HUD 유지, HUD 값 변경 시에만 JS 푸시
    // - Focus 실패 시 패널 미오픈 및 획득한 자원만 해제, 최초 패널 열기 시 지연 키 차단 훅 설치
    // - 다른 메뉴·로드·사망·창 전환(gameActive=false) 시 패널 자동 닫기 및 g_hiddenKeys 초기화
    class UiBridge
    {
    public:
        static bool Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            Core::EnergyShieldPool* esPool,
            const Core::DataPack* dataPack,
            const Core::PassiveTreeData* treeData,
            const Core::ItemData* itemData,
            const Core::Config* config,
            Core::SkillSystem* skillSystem = nullptr,
            Core::CharmSystem* charmSystem = nullptr);

        // 주 갱신 스레드 프레임 콜백에서 호출: HUD 변경 푸시, 창 전환/사망/로드 감시
        static void Tick(RE::Main* main);

        // 주 갱신 스레드에서 패널 강제 닫기
        static void ClosePanelOnMainThread();

        [[nodiscard]] static bool IsPanelOpen() noexcept;
        [[nodiscard]] static bool IsDomReady() noexcept;
        [[nodiscard]] static bool IsHudShown() noexcept;
        [[nodiscard]] static bool IsSearchFocused() noexcept;
        [[nodiscard]] static std::string_view ActiveTab() noexcept;

        // 언어 팩(ko-KR/en-US) 및 DataPack 기준 젬 표시 이름 조회(C-25)
        [[nodiscard]] static std::string GetGemDisplayName(std::string_view gemId);

        // 자동 시험용(주 갱신 스레드 전용): 실제 사용자 경로와 동일한 패널 열기·JS 명령 처리·페이로드 생성 진입점
        static bool TryOpenPanelOnMainThread(std::string_view initialTab = "skills");
        static void ExecuteCommandForTestOnMainThread(std::string_view rawJson);
        [[nodiscard]] static std::string BuildPanelJsonForTestOnMainThread();
    };
}
