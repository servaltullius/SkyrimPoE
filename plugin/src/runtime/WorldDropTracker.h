#pragma once

#include "core/ItemData.h"
#include "core/Types.h"

#include <cstdint>
#include <vector>

namespace SkyrimPoE::Runtime
{
    // 코세이브 'WDRP' v1 직렬화 단위(검수 C4: 세이브 왕복 시 바닥 드랍+빛기둥 추적 복원)
    struct SavedWorldDropEntry
    {
        RE::FormID dropRefFormId = 0;
        RE::FormID corpseRefFormId = 0;
        RE::FormID beamRefFormId = 0;
        std::uint8_t worldDropTier = 2;
    };

    // 결정 16, 159, 검수 C4:
    // 자체 바닥 물리 드랍 추적기(Death Drop Overhaul 미의존).
    // - 설정 등급 이상·동시 상한 내 전리품만 시체에서 월드로 배출(복제 금지) + 임시 빛기둥 + 효과음
    // - 'WDRP' v1 코세이브 저장 + 로드/셀 전환 시 미추적 고아 빛기둥 자동 스윕 2중 방어
    class WorldDropTracker
    {
    public:
        static void Initialize(const Core::ItemData* itemData, const Core::Config* config);

        // DeathListener 주 갱신 콜백에서 호출: 처치 드랍 판정 및 시체/바닥 배출
        static void OnKillAwarded(
            RE::ActorHandle corpseHandle,
            std::uint32_t effectiveMonsterLevel,
            bool isBoss,
            bool isDragon);

        // 명시적 바닥 드랍 + 빛기둥 생성 헬퍼(AutoTest E7 및 내부 공용)
        static bool SpawnPhysicalDropFromContainer(
            RE::TESObjectREFR* sourceContainer,
            RE::TESBoundObject* itemObj,
            std::uint8_t worldDropTier,
            bool isCurrencyDrop);

        // 주 갱신 스레드 틱: 줍기 감지·빛기둥 위치 동기화·시간 초과/셀 이탈 시체 회수·고아 스윕
        static void Tick(double dt, bool paused);

        // 로드 직후 및 셀 진입 시 미추적 고아 빛기둥 Static 마커를 즉시 정리(검수 C4)
        static std::size_t SweepOrphanBeamsNearPlayer();

        // 모든 활성 드랍을 즉시 시체로 회수/정리(AutoTest E7 검증용)
        static void ReturnAllToCorpsesNow();

        // 코세이브 'WDRP' v1 스냅샷/복원/초기화(순수 메모리)
        [[nodiscard]] static std::vector<SavedWorldDropEntry> TakeSnapshotForSave();
        static void RestoreFromSave(std::vector<SavedWorldDropEntry> entries);
        static void Clear();
        [[nodiscard]] static std::size_t ActiveCount();
    };
}
