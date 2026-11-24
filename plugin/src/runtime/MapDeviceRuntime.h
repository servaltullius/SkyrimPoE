#pragma once

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/Config.h"
#include "core/EnergyShield.h"
#include "core/ItemData.h"
#include "core/ItemTypes.h"
#include "core/MapSystem.h"
#include "core/MapTravel.h"
#include "core/PassiveTreeData.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SkyrimPoE::Runtime
{
    struct PersistentState;

    enum class MapEntryDenial
    {
        None,
        ArenaMissingMilestones,
        ArenaAlreadyCleared,
        NoReenterableRun,
        NoPortals
    };

    struct MapActionResult
    {
        bool ok = false;
        std::string message;
        MapEntryDenial entryDenial = MapEntryDenial::None;
    };

    struct MapDeathEscapeRequestObservation
    {
        std::uint64_t count = 0;
        RE::FormID attackerId = 0;
        std::chrono::steady_clock::time_point acceptedAt{};
    };

    struct UninstallCleanupRemaining
    {
        std::size_t gems = 0;
        std::size_t jewels = 0;
        std::size_t charms = 0;

        [[nodiscard]] bool Empty() const noexcept { return gems == 0 && jewels == 0 && charms == 0; }
    };

    struct MapKillContext
    {
        std::uint64_t runId = 0;
        std::uint32_t rewardMonsterLevel = 0;
        std::uint8_t dropMapTier = 0;
        double bonusIiqPct = 0.0;
        double bonusIirPct = 0.0;
        bool isBoss = false;
        bool runFailed = false;
        // 결정 176 ④: 플레이어가 허가된 회차 셀 안에 없으면 보상을 주지 않는다.
        bool rewardBlocked = false;
        // rewardBlocked를 정한 입력. 보스 처치 보상 자격 진단 로그에 쓴다.
        RE::FormID playerCellId = 0;
        RE::FormID permittedCellId = 0;
        bool playerInActiveCell = false;
        // 결정 179 개정: 보스의 사망이 회차 실패 전에 시작돼 runFailed·rewardBlocked를 그 시점 값으로 정했는지.
        bool judgedAtDeathStart = false;
        std::optional<Core::Arena4BossDeathState> arenaBossDeath;
    };

    enum class MapKillReward
    {
        Eligible,
        NoPlayerContribution,
        Ineligible
    };

    struct MapTravelStatus
    {
        // 이동 요청이 도착한 뒤 보스 처치를 기다리는 동안에도 귀환 종류를 유지한다.
        std::optional<Core::MapTravelKind> kind;
        bool inPackCell = false;
        bool unauthorized = false;
        bool canRetryReturn = false;

        bool operator==(const MapTravelStatus&) const = default;
    };

    struct MapCellResetStatus
    {
        bool pending = false;
        std::size_t dispatched = 0;   // VM이 접수한 횟수
        std::size_t completed = 0;    // 호출 완료 콜백이 온 횟수
    };

    struct Arena4BossPhaseStatus
    {
        std::uint8_t requested = 0;
        std::uint8_t completed = 0;
        bool invulnerable = false;
    };

    struct PlayerMapItemEntry
    {
        Core::InventoryItemKey key{};
        RE::FormID baseFormId = 0;
        std::uint8_t tier = 1;
        std::int32_t count = 1;
        bool tracked = false;
        Core::ItemRarity rarity = Core::ItemRarity::Normal;
        std::string displayName;
        std::vector<std::string> modLines;
        Core::MapModSummary summary{};
    };

    // 6단계(결정 166): 런타임 오버레이 지도 장치 · 4차 전직 3페이즈 보스 · 치명타 사망 가로채기 · 안전 제거 정리
    class MapDeviceRuntime
    {
    public:
        static void Initialize(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            const Core::ItemData* itemData,
            const Core::PassiveTreeData* treeData,
            const Core::Config* config,
            Core::EnergyShieldPool* esPool,
            Core::CharmSystem* charmSystem,
            const Core::MapTravelSettings& travelSettings);
        static void InstallReturnDoorSink();
        static void BindDataPack(const Core::DataPack* dataPack);

        // 주 갱신 스레드 프레임 틱: 셀 소속 원자 갱신, 비동기 셀 로드 완료 후 스폰, 3페이즈 무적 타이머, 탈출 쿨다운
        static void Tick(double deltaSec, bool gamePaused);

        // 주 갱신 스레드 전용: 지도·4차 투기장 입장/재입장/귀환/포기
        static MapActionResult OpenNormalMap(
            RE::PlayerCharacter* player,
            const Core::InventoryItemKey& mapKey,
            RE::FormID baseFormId);
        static MapActionResult OpenArena4(RE::PlayerCharacter* player);
        static MapActionResult ReenterActiveMap(RE::PlayerCharacter* player);
        static MapActionResult ReturnPortal(RE::PlayerCharacter* player);
        static MapActionResult AbandonActiveMap(RE::PlayerCharacter* player);
        static MapActionResult SafeEscape(RE::PlayerCharacter* player);
        [[nodiscard]] static MapTravelStatus GetTravelStatus(RE::PlayerCharacter* player);
        [[nodiscard]] static Core::MapTravelSettings GetTravelSettings() noexcept;
        [[nodiscard]] static double GetTravelArrivalRadius() noexcept;
        [[nodiscard]] static double GetFallMargin() noexcept;
        // 입구로 옮긴 낙하 복귀 횟수(자동 시험 H10)
        [[nodiscard]] static std::size_t GetFallRecoveryCount();
        // 출구 구역(결정 177 ③ 재보완)이 이동을 건 누적 횟수와, 켜져 있던 출구 문을 다시 끈 누적 수(자동 시험 H27)
        [[nodiscard]] static std::size_t GetExitZoneTravelCount();
        // 결정 182: 허가된 회차의 출구 구역 귀환을 거절해야 하면 그 안내 문구(포털 0, 보스 생존). 주 갱신 스레드 전용(자동 시험 H29도 씀).
        [[nodiscard]] static std::optional<std::string> ExitZoneReturnRefusal();
        [[nodiscard]] static std::size_t GetExitDoorReenabledCount();
        // 셀이 다시 붙었을 때 남은 스폰으로 찾아 끄기를 요청한 누적 액터 수(결정 178 ⑦ 보완, 자동 시험 H23·H25)
        [[nodiscard]] static std::size_t GetLeftoverSweepCleanedCount() noexcept;
        // 남은 스폰 청소가 Disable을 요청했고 아직 꺼진 것을 확인하지 못한 액터인지(자동 시험 H23·H25가 기다림)
        [[nodiscard]] static bool IsLeftoverSweepPending(RE::FormID actorId);
        [[nodiscard]] static MapCellResetStatus GetCellResetStatus(RE::FormID cellId);

        // 주 갱신 스레드 전용: 안전 제거 일괄 정리 절차
        static MapActionResult RunSafeUninstallCleanup(RE::PlayerCharacter* player);
        [[nodiscard]] static std::optional<UninstallCleanupRemaining> GetUninstallCleanupRemaining(const PersistentState& state);

        // 주 갱신 스레드 전용(DeathListener 호출): 보스 처치로 회차를 진행하고, 자격에 따라 보상을 반영한다.
        static void OnActorKilled(RE::FormID actorId, std::uint64_t runId, MapKillReward reward);

        // 스레드 안전(CombatHooks / MonsterRuntime 호출): 4차 전직 3페이즈 보스 체력 하한 동기 클램프 + 전환 예약
        // rawSkyrimDamage는 난이도 배율 적용 전 피해량이며, diffMult는 실제 체력 차감 배율(fDiffMultHPByPC)이다.
        [[nodiscard]] static double FilterArena4BossIncomingDamage(
            RE::Actor* boss,
            double rawSkyrimDamage,
            double diffMult,
            std::optional<std::uint64_t> eventGeneration = std::nullopt);

        // CombatHooks 워커 스레드 안전: 지도/결투장 내부 치명타 가로채기 및 회차별 귀환 예약
        // true 반환 시 호출부는 들어오는 피해를 즉시 0.0f로 무효화해야 한다.
        [[nodiscard]] static bool TryInterceptLethalHitFromHook(
            double curHp,
            double effectiveDamage,
            std::uint64_t generation,
            RE::FormID attackerId);
        [[nodiscard]] static bool WouldInterceptLethalHit(
            bool enabled, double curHp, double effectiveDamage, std::uint64_t generation);
        [[nodiscard]] static MapDeathEscapeRequestObservation GetLastDeathEscapeRequest();

        // 락프리 원자 상태 조회
        [[nodiscard]] static bool IsPlayerInActiveMapCell() noexcept;
        [[nodiscard]] static bool IsActiveMapRun() noexcept;
        [[nodiscard]] static bool IsActiveMapCell(RE::FormID cellFormId) noexcept;
        [[nodiscard]] static std::uint8_t GetActiveMapTier() noexcept;
        [[nodiscard]] static bool IsActiveMapOrArenaBoss(RE::FormID actorFormId) noexcept;
        [[nodiscard]] static bool IsActiveArena4Boss(RE::FormID actorFormId) noexcept;
        // 활성 회차가 스폰해 등록한 액터(보스·무리·결투장 증원)인지. 결정 181의 첫 타격 적대 판정에 쓴다.
        [[nodiscard]] static bool IsActiveRunMapActor(RE::FormID actorFormId);
        [[nodiscard]] static bool IsDeathEscapeInFlight() noexcept;
        [[nodiscard]] static std::uint64_t GetDeathEscapeRequestCount() noexcept;
        [[nodiscard]] static bool IsUninstallCleanupArmed() noexcept;
        [[nodiscard]] static bool IsUninstallCleanupActive() noexcept;
        static void DisarmUninstallCleanup() noexcept;

        // 뮤텍스 보호 스냅샷 조회
        [[nodiscard]] static bool IsArena4BossInvulnerable(RE::FormID actorId);
        [[nodiscard]] static Arena4BossPhaseStatus GetArena4BossPhaseStatusForAutoTest(RE::FormID actorId);
        [[nodiscard]] static Core::ActiveMapState GetActiveMapStateCopy();
        [[nodiscard]] static std::uint64_t GetActiveRunId();
        [[nodiscard]] static bool IsRunActive(std::uint64_t runId);
        // 활성이고 실패하지 않은 회차(첫 공물 같은 보상 판정용)
        [[nodiscard]] static bool IsRunRewardable(std::uint64_t runId);

        // 전용 잎 잠금으로 액터 배율과 지도 어픽스를 읽는다.
        [[nodiscard]] static Core::MapActorScaling GetMapActorScaling(
            RE::FormID actorId, Core::MapModSummary* mods = nullptr);
        // 처치 보상은 회차 잠금과 액터 등록부 잠금으로 함께 확인한다.
        [[nodiscard]] static MapKillContext GetMapKillContext(RE::FormID actorId, RE::FormID playerCellId);
        // 결정 179 개정: 회차가 실패하기 전에 시작된 보스 사망(치명타)을 기록하고, 리셋으로 취소되면 지운다.
        static void NoteMapBossDeathStart(RE::FormID actorId, RE::FormID playerCellId);
        static void ClearMapBossDeathStart(RE::FormID actorId);
        // 사망 확정과 회차 처리 대기를 표시한다. 같은 회차 보스의 중복 확정은 건너뛴다.
        [[nodiscard]] static std::optional<MapKillContext> ConfirmMapKill(
            RE::FormID actorId, RE::FormID playerCellId);

        // 주 갱신 스레드 전용: 인벤토리 보유 지도 목록·4차 공물 수량·엔진 이동 요청
        [[nodiscard]] static std::vector<PlayerMapItemEntry> ListPlayerMaps(RE::PlayerCharacter* player);
        [[nodiscard]] static std::int32_t CountPlayerTributes(RE::PlayerCharacter* player);
        static bool MovePlayerToSpace(
            RE::PlayerCharacter* player,
            RE::TESObjectCELL* targetCell,
            RE::TESWorldSpace* targetWorldspace,
            const RE::NiPoint3& pos,
            const RE::NiPoint3& angle);

        // Layer2Input에 플레이어의 지도 셀 소속과 불이익을 동기화
        static void SyncMapLayer2ToStatEngine();

        // SKSE 코세이브 회차 스냅샷/복원/초기화(순수 메모리) 및 PostLoad 재바인딩
        [[nodiscard]] static Core::ActiveMapState TakeSnapshotForSave();
        static void RestoreFromSave(Core::ActiveMapState saved, RE::FormID legacyCell, bool atlasDamaged, bool runDamaged);
        static void OnPostLoadRebind();
        static void ResetOnRevert();
    };
}
