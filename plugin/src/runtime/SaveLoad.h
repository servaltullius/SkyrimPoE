#pragma once

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/ItemData.h"
#include "core/ItemTypes.h"
#include "core/MapTypes.h"
#include "core/PassiveTreeData.h"
#include "core/SaveRecordFormat.h"
#include "core/StatEngine.h"
#include "core/Types.h"
#include "runtime/MigrationRuntime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace SkyrimPoE::Runtime
{
    // SKSE 코세이브('SPOE')와 세이브 전환 생명주기 관리(결정 59, 105~108, 117~119, 138, 150 ④, 161~164).
    // 코세이브 콜백(OnSave/OnLoad/OnRevert)은 순수 메모리 작업만 수행하고, 엔진 상태 반영은 CommandQueue로 넘긴다.
    class SaveLoad
    {
    public:
        static constexpr std::uint32_t kCosaveId = 'SPOE';
        static constexpr std::uint32_t kAtlasVersion = Core::SaveRecord::kAtlasVersion;
        static constexpr std::uint32_t kMapsVersion = Core::SaveRecord::kMapsVersion;

        struct LoadedMapRecordVersions
        {
            std::uint32_t atlas = 0;
            std::uint32_t maps = 0;
        };

        struct LoadDiagnostics
        {
            std::optional<Core::SaveDecision> decision;
            std::optional<Core::GenerationRecord> generation;
            bool receivedLoad = false;
            bool collectionComplete = false;
        };

        struct SaveDiagnostics
        {
            std::uint64_t sequence = 0;
            bool wroteGeneration = false;
            std::size_t failedRecords = 0;
            bool complete = false;
            std::vector<std::uint32_t> recordTypes;
            std::optional<Core::SaveGeneration> generation;
            std::uint32_t threadId = 0;
        };

        static bool Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            const Core::DataPack* dataPack,
            const Core::PassiveTreeData* treeData,
            const Core::Config* config,
            const Core::ItemData* itemData = nullptr,
            Core::CharmSystem* charmSystem = nullptr);

        static void SetContentCheck(std::optional<Core::SaveGeneration> generation, std::string reason);
        static void SetCleanupTestTarget(bool enabled);
        [[nodiscard]] static std::optional<Core::SaveGeneration> GetCleanupTestTarget();
        static void ExpectCleanupTestLoad(std::string_view stem);
        [[nodiscard]] static bool MatchCleanupTestLoad(std::string_view name) noexcept;
        [[nodiscard]] static std::optional<MigrationCandidateSnapshot> GetMigrationCandidate(RE::PlayerCharacter* player);
        [[nodiscard]] static std::optional<MigrationDiagnostics> GetLastMigrationDiagnostics();
        static void ReleaseForTest();
        static void TickMigration();
        static void OnGameLoadStarted();
        static void OnGameLoadFinished(bool succeeded);

        // kNewGame / 성공한 kPostLoadGame 시 판정 및 주 갱신 스레드의 초기화를 예약한다.
        static void SchedulePostLoadOrNewGameInit(bool isNewGame);

        [[nodiscard]] static PersistentState GetStateCopy();
        [[nodiscard]] static std::optional<Core::SaveGeneration> GetContentGeneration();
        [[nodiscard]] static LoadDiagnostics GetLastLoadDiagnostics();
        [[nodiscard]] static SaveDiagnostics GetLastSaveDiagnostics();
        [[nodiscard]] static std::size_t GetPreservedRecordCount();
        // 마지막 로드에서 실제 읽기에 성공한 ATLS·MAPS 판(없으면 0).
        [[nodiscard]] static LoadedMapRecordVersions GetLoadedMapRecordVersions();
        static void SetState(const PersistentState& state);
        static void UpdateState(const std::function<void(PersistentState&)>& mutator);
        // 결정 137(ㅁ): 젬·장비·주얼 거래를 코세이브 저장(OnSave)과 직렬화해 절반만 반영된 상태가 저장되지 않도록 보장한다.
        static void RunSerializedTransaction(const std::function<void()>& txFn);
        static void AddRevertCallback(std::function<void()> cb);
        static void SyncStateToStatEngine();
        static void PersistCharmSlotsFromSystem();

        // 재검수 R-9: 원복해야 할 NPC kTemporary SpeedMult(냉각·빙결·가속)를 그 액터를 찾지 못해 지금 되돌릴 수 없을 때 맡긴다.
        // 맡긴 항목은 세이브(SPDM)에 함께 저장되고, 주 갱신에서 주기적으로 다시 시도한다.
        // 실행 중 만든 FF 액터는 찾지 못하면 이미 지워진 것이고 번호가 다른 액터에게 다시 쓰일 수 있으므로 맡기지 않는다.
        static void DeferSpeedRevert(RE::FormID actorId, float appliedDelta);
        // TESResetEvent(부활·셀 리셋)로 엔진이 임시 수정치를 지운 액터의 맡긴 원복을 버린다(주 갱신 스레드).
        static void ForgetSpeedRevert(RE::FormID actorId);
        // 주 갱신 스레드 전용: 맡긴 원복을 다시 시도한다(PostLoadInit과 주기 틱에서 호출).
        static void RetryPendingSpeedReverts(double deltaSec, bool gamePaused);
        // 6단계 안전 제거 정리용: 미원복 SPDM 대기열 NPC SpeedMult 즉시 원복
        static std::size_t RevertAllPendingSpeedModsNow();

    private:
        static void ScheduleLoadInit(bool isNewGame, std::optional<std::uint64_t> expectedWorld);
        static void ScheduleEngineInit(bool isNewGame, bool afterCleanup, std::uint64_t queueGeneration);
    };
}
