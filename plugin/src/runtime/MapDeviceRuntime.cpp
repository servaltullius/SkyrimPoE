#include "PCH.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/ModState.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/CycloneRuntime.h"
#include "runtime/DeathListener.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
#include "runtime/MainUpdateHook.h"
#include "runtime/MapPackRuntime.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/ProxySpellRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"
#include "runtime/UniqueIdUtil.h"
#include "runtime/WorldDropTracker.h"

#include "core/GemTransaction.h"
#include "core/ItemFormatter.h"
#include "core/MapSystem.h"
#include "core/PassiveTree.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <format>
#include <mutex>
#include <unordered_map>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        // houseCARL (2026-11-22): WhiterunStablesMapMarker의 빠른 이동 착지점(Skyrim.esm).
        constexpr RE::FormID kSafeReturnMarkerId = 0x0007287B;
        constexpr RE::FormID kXMarkerHeadingId = 0x00000034;
        constexpr RE::FormID kTamrielWorldspaceId = 0x0000003C;
        constexpr double kDeathEscapeReleaseCooldownSec = 1.5;
        constexpr double kFallRecoveryCooldownSec = 3.0;
        // 결정 177 ③ 재보완: 출구 구역에 든 뒤 귀환이 거절되면 이 간격으로 다시 시도하고, 거절 안내는 이 간격보다 자주 띄우지 않는다.
        constexpr double kExitZoneRetrySec = 0.5;
        // 거절된 뒤에는 이 간격으로 다시 시도한다(보스 처치 처리 대기는 10초 안에 풀리고, 풀리지 않는 거절이 로그를 채우지 않게).
        constexpr double kExitZoneRefusedRetrySec = 1.0;
        constexpr double kExitZoneMessageSec = 3.0;
        constexpr double kMapActorDeleteDelaySec = 3.0;
        constexpr double kMapActorDeleteRetrySec = 1.0;
        // 이만큼(일시정지·로딩 제외) 조회되지 않으면 삭제를 포기한다. 비활성 상태로 남고 셀 초기화에 맡긴다.
        constexpr double kMapActorDeleteGiveUpSec = 300.0;
        constexpr std::size_t kMaxPendingMapActorDeletes = 512;
        constexpr RE::FormID kDynamicFormMask = 0xFF000000u;

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::Config* g_config = nullptr;
        Core::EnergyShieldPool* g_esPool = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;

        std::mutex g_mutex;
        struct PendingActorDelete
        {
            RE::FormID actorId = 0;
            std::uint64_t runId = 0;
            // 스폰 때 넘긴 기본 객체(GetBaseObject, 그 던전의 스폰 원형 NPC). 레벨드 템플릿의 임시 ActorBase가 아니다.
            RE::FormID actorBaseId = 0;
            RE::FormID runCellId = 0;
            std::optional<double> untilLookupSec;
            double ageSec = 0.0;
            std::optional<double> firstDisabledAgeSec;
        };
        std::deque<PendingActorDelete> g_pendingActorDeletes;
        std::uint64_t g_pendingActorDeleteGeneration = 0;
        // 삭제 대기에서 빠진 뒤에도 같은 회차의 액터에 Disable을 중복 예약하지 않는다.
        std::vector<RE::FormID> g_runSpawnDisableRequests;
        Core::ActiveMapState g_activeMap{};
        std::optional<Core::PoEItemRecord> g_consumedMapRecord;
        Core::MapTravelSettings g_travelSettings;
        RE::TESObjectREFR* g_safeReturnMarker = nullptr;
        std::optional<Core::MapTravelTarget> g_safeReturnTarget;

        // 이동 요청은 메모리에만 둔다. 입장 비용은 도착 때 소비하므로 저장할 대기 항목이 없다.
        Core::MapTravel g_travel;
        struct TravelContext
        {
            Core::MapTravelTarget entryOrigin;
            Core::InventoryItemKey mapKey;
            RE::FormID item = 0;
            std::uint64_t runId = 0;
        };
        TravelContext g_travelContext;
        // 회차와 목표는 g_travelContext와 g_travel에 유지한다. 이 단계에서 도착 시계는 돌리지 않는다.
        struct PendingTravelDisable
        {
            std::uint64_t requestFrame;
            std::vector<RE::FormID> actorIds;
            double elapsedSec = 0.0;
            // 결정 179 개정: 사망이 시작된 보스의 확정·처치 반영이 남았으면 비활성화 요청 자체를 미룬다.
            // 비활성화된 보스는 사망 확정이 오지 않을 수 있다.
            bool awaitingBossDeath = false;
            double bossWaitSec = 0.0;
            // 보스 대기를 한 번 마쳤으면 이동 직전 재확인에서 다시 기다리지 않는다(큐가 멈춘 경우의 무한 대기 방지).
            bool bossHoldDone = false;
        };
        std::optional<PendingTravelDisable> g_pendingTravelDisable;
        RE::FormID g_permittedCell = 0;
        RE::FormID g_warnedUnauthorizedCell = 0;
        std::uint64_t g_retryReturnRunId = 0;
        struct CellResetRejection
        {
            RE::TESObjectCELL::CellState cellState;
            bool attached;
            RE::VMHandle handle;
            bool handleAvailable;
            bool handleIsCell;
            bool operator==(const CellResetRejection&) const = default;
        };
        // 결정 177 ①: Reset은 VM이 호출을 마쳤다는 콜백이 와야 대기에서 뺀다. 접수만으로는 새 회차를 열지 않는다.
        struct PendingCellReset
        {
            std::uint64_t seq = 0;       // 불러오기 뒤 늦게 온 옛 콜백이 새 대기를 지우지 않게 구분한다
            bool warned = false;         // 전송 실패 경고를 한 번만 남긴다
            bool inFlight = false;       // VM이 접수했고 콜백을 기다리는 중
            double waitedSec = 0.0;      // 대기에 들어온 뒤 지난 시간(일시정지·로딩 제외)
            double inFlightSec = 0.0;    // 접수 뒤 지난 시간(일시정지·로딩 제외)
            std::optional<CellResetRejection> lastRejection;
        };
        // 콜백이 이 시간 안에 오지 않으면 다시 보낸다.
        constexpr double kCellResetCallbackTimeoutSec = 10.0;
        // 초기화가 이 시간보다 오래 끝나지 않으면 새 회차 입장을 더 막지 않고 경고만 남긴다(영구 차단 방지).
        constexpr double kCellResetMaxEntryWaitSec = 20.0;
        std::unordered_map<RE::FormID, PendingCellReset> g_pendingCellResets;
        std::unordered_map<RE::FormID, std::size_t> g_sentCellResets;
        std::unordered_map<RE::FormID, std::size_t> g_completedCellResets;
        std::uint64_t g_nextCellResetSeq = 1;
        // 불러온 뒤 셀을 아직 못 읽어 결정 175 종료 판단을 미룬 경우
        bool g_endCheckAfterLoad = false;
        bool g_atlasDamagedAfterLoad = false;
        bool g_runDamagedAfterLoad = false;
        bool g_runDamageEscapeReady = false;
        // 입구로 옮긴 뒤에도 다음 판정에서 여전히 문턱 아래면 포털 귀환으로 바꾼다.
        std::uint32_t g_fallRecoveryStreak = 0;
        // 문턱 아래인데 복귀를 미룬 이유를 한 낙하에 한 번만 남긴다.
        bool g_fallSkipLogged = false;
        // 남은 스폰을 훑은 지도 팩 셀. 팩 밖으로 나가거나 불러오기 중이면 비워, 다시 들어오면 한 번 더 훑는다.
        RE::FormID g_leftoverSweptCell = 0;
        // 남은 스폰으로 찾아 끄기를 요청한 액터 수(자동 시험 H23·H25가 보고).
        std::atomic<std::size_t> g_leftoverSweepCleaned{ 0 };
        // 남은 스폰 청소가 Disable을 요청했고 아직 꺼진 것을 확인하지 못한 액터. 귀환은 이것도 꺼진 뒤에 떠난다(Codex 검토:
        // 청소 직후 귀환이 시작되면 예약된 Disable이 셀이 떨어진 뒤 실행될 수 있었다).
        std::vector<RE::FormID> g_sweepDisableRequested;
        double g_fallRecoveryCooldownSec = 0.0;
        std::size_t g_fallRecoveryCount = 0;
        // 결정 177 ③ 재보완(g_mutex): 출구 구역에 든 지도 팩 셀. 이동이 시작돼 셀을 떠날 때까지(또는 불러오기·셀 변경까지)
        // 귀환을 다시 시도한다. 0이면 없음.
        RE::FormID g_exitZoneLatchCell = 0;
        double g_exitZoneRetryCooldownSec = 0.0;
        double g_exitZoneMessageCooldownSec = 0.0;
        // 출구 구역이 이동을 건 횟수(자동 시험 H27)
        std::size_t g_exitZoneTravelCount = 0;
        // 출구 문이 켜져 있는지 확인한 지도 팩 셀(팩 밖으로 나가거나 불러오기 중이면 비워 다시 확인)과, 켜져 있어 다시 끈 수
        RE::FormID g_exitDoorsCheckedCell = 0;
        std::size_t g_exitDoorsReenabledCount = 0;
        std::atomic<std::uint64_t> g_deathEscapeRequestCount{ 0 };
        MapDeathEscapeRequestObservation g_lastDeathEscapeRequest;  // g_mutex
        // 셀이 막 바뀐 프레임(위치가 아직 옛 좌표일 수 있음)에는 낙하를 판정하지 않는다.
        RE::FormID g_lastObservedCell = 0;
        struct IncompatibleSavedRun
        {
            Core::ActiveMapState state;
            RE::FormID legacyCell = 0;
        };
        std::optional<IncompatibleSavedRun> g_incompatibleSavedRun;
        // 쓰기는 지도 상태 잠금 아래에서 이 잠금을 잡는다. 조회는 이 잠금만 잡고 값을 복사한다.
        std::mutex g_mapActorsMutex;
        Core::MapActorRegistry g_mapActors;
        Core::MapModSummary g_mapActorMods{};
        bool g_spawnedForCurrentRun = false;
        double g_bossMissingSec = 0.0;
        std::uint64_t g_nextRunId = 1;
        std::uint64_t g_pendingSpawnRunId = 0;
        double g_arenaPhaseInvulnSec = 0.0;
        std::uint8_t g_completedArenaPhase = Core::kArena4FirstPhase;
        // 결정 179 개정(2026-11-23): 회차가 실패하기 전에 보스의 사망이 시작됐을 때만 남긴다. 처치의 실패·보상 차단과
        // 결투장 페이즈 조건은 이 시점(치명타)의 값으로 판정한다.
        struct MapBossDeathStart
        {
            std::uint64_t runId;
            bool rewardBlocked;
            std::optional<Core::Arena4BossDeathState> arena;
        };
        std::optional<MapBossDeathStart> g_bossDeathStart;
        double g_deathEscapeExitElapsedSec = 0.0;

        // 락프리 원자 상태(CombatHooks 워커 스레드 안전 조회용)
        std::atomic_bool g_playerInActiveMapCell{ false };
        std::atomic_bool g_activeMapRun{ false };
        std::atomic<RE::FormID> g_activeCellFormId{ 0 };
        std::atomic<std::uint8_t> g_activeMapTier{ 0 };
        std::atomic<RE::FormID> g_activeBossFormId{ 0 };
        std::atomic_bool g_activeIsArena4{ false };
        std::atomic<std::uint64_t> g_deathEscapeRunId{ 0 };
        enum class CleanupStatus { Idle, InProgress, Armed };
        std::atomic<CleanupStatus> g_uninstallCleanupStatus{ CleanupStatus::Idle };
        std::atomic_bool g_uninstallCleanupIncomplete{ false };
        // g_mutex로 보호한다. 큐에 있는 보스 처치 처리보다 귀환 확정이 먼저 오지 않게 한다.
        std::uint64_t g_bossDeathPendingRunId = 0;
        std::uint64_t g_bossDeathConfirmedRunId = 0;
        std::optional<std::chrono::steady_clock::time_point> g_firstBossDeathReturnDeferral;
        struct PendingReturnConfirmation
        {
            std::uint64_t runId;
            Core::MapTravelKind kind;
        };
        std::optional<PendingReturnConfirmation> g_pendingReturnConfirmation;

        void ClearTravelLocked()
        {
            g_pendingTravelDisable.reset();
            g_travel.Clear();
            g_travelContext = {};
        }

        std::optional<Core::MapTravelKind> PendingTravelKindLocked()
        {
            if (const auto& travel = g_travel.Current()) {
                return travel->kind;
            }
            if (g_pendingReturnConfirmation) {
                return g_pendingReturnConfirmation->kind;
            }
            return std::nullopt;
        }

        bool IsArena4PhaseInvulnerableLocked()
        {
            return Core::MapSystem::IsArena4PhaseInvulnerable(
                g_activeMap.arenaPhase, g_completedArenaPhase, g_arenaPhaseInvulnSec);
        }

        void RefreshMapActorsLocked()
        {
            std::scoped_lock lk(g_mapActorsMutex);
            if (g_activeMap.active && g_dataPack) {
                g_mapActors.Rebuild(g_activeMap, *g_dataPack);
                g_mapActorMods = g_activeMap.summary;
            } else {
                g_mapActors.Clear();
                g_mapActorMods = {};
            }
        }

        bool IsMapRewardBlockedLocked(RE::FormID playerCellId)
        {
            return playerCellId == 0 || playerCellId != g_permittedCell ||
                !g_playerInActiveMapCell.load(std::memory_order_acquire);
        }

        // 결정 179 ⑦: 이 회차 보스의 사망이 시작됐거나 확정됐는데 처치 반영(큐)이 아직 끝나지 않았는지.
        bool BossKillProcessingPendingLocked(std::uint64_t runId)
        {
            return runId != 0 && ((g_bossDeathStart && g_bossDeathStart->runId == runId) || g_bossDeathPendingRunId == runId);
        }

        MapKillContext GetMapKillContextLocked(RE::FormID actorId, RE::FormID playerCellId)
        {
            std::scoped_lock actorLock(g_mapActorsMutex);
            if (!g_mapActors.Find(actorId)) {
                return {};
            }
            MapKillContext context;
            context.runId = g_activeMap.runId;
            context.rewardMonsterLevel = Core::MapSystem::RewardMonsterLevel(g_activeMap);
            context.dropMapTier = g_activeMap.isArena4 ? std::uint8_t{ 0 } : g_activeMap.tier;
            context.bonusIiqPct = g_activeMap.summary.bonusIiqPct;
            context.bonusIirPct = g_activeMap.summary.bonusIirPct;
            context.isBoss = actorId == g_activeMap.bossRefFormId;
            context.playerCellId = playerCellId;
            context.permittedCellId = g_permittedCell;
            context.playerInActiveCell = g_playerInActiveMapCell.load(std::memory_order_acquire);
            const auto* deathStart = context.isBoss && g_bossDeathStart && g_bossDeathStart->runId == context.runId
                ? &*g_bossDeathStart : nullptr;
            context.judgedAtDeathStart = deathStart != nullptr;
            const auto judged = Core::MapSystem::JudgeBossKill(deathStart != nullptr,
                deathStart && deathStart->rewardBlocked, g_activeMap.failed, IsMapRewardBlockedLocked(playerCellId));
            context.runFailed = judged.runFailed;
            context.rewardBlocked = judged.rewardBlocked;
            if (context.isBoss && g_activeMap.isArena4) {
                context.arenaBossDeath = deathStart && deathStart->arena
                    ? *deathStart->arena
                    : Core::Arena4BossDeathState{ g_completedArenaPhase, IsArena4PhaseInvulnerableLocked() };
            }
            return context;
        }

        void RefreshAtomicsLocked() noexcept
        {
            const bool active = g_activeMap.active;
            g_activeMapRun.store(active, std::memory_order_release);
            if (active) {
                const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(g_activeMap));
                g_activeCellFormId.store(forms ? forms->cellId : 0, std::memory_order_release);
                g_activeMapTier.store(g_activeMap.tier, std::memory_order_release);
                g_activeBossFormId.store(g_activeMap.bossRefFormId, std::memory_order_release);
                g_activeIsArena4.store(g_activeMap.isArena4, std::memory_order_release);
            } else {
                g_activeCellFormId.store(0u, std::memory_order_release);
                g_activeMapTier.store(0u, std::memory_order_release);
                g_activeBossFormId.store(0u, std::memory_order_release);
                g_activeIsArena4.store(false, std::memory_order_release);
                g_playerInActiveMapCell.store(false, std::memory_order_release);
            }
        }

        void SetActiveMapLocked(Core::ActiveMapState state, std::optional<Core::PoEItemRecord> consumedMap = std::nullopt)
        {
            if (g_activeMap.runId != state.runId) {
                if (g_pendingTravelDisable) {
                    if (Core::MapSystem::ReleaseDeathEscape(g_deathEscapeRunId, g_travelContext.runId)) {
                        g_deathEscapeExitElapsedSec = 0.0;
                    }
                    ClearTravelLocked();
                }
                g_runSpawnDisableRequests.clear();
                g_bossDeathPendingRunId = 0;
                g_bossDeathConfirmedRunId = 0;
                g_firstBossDeathReturnDeferral.reset();
                g_pendingReturnConfirmation.reset();
                g_completedArenaPhase = Core::kArena4FirstPhase;
                g_bossDeathStart.reset();
                g_bossMissingSec = 0.0;
            }
            g_activeMap = std::move(state);
            g_consumedMapRecord = std::move(consumedMap);
            RefreshMapActorsLocked();
        }

        // 검수 C-1: TESObjectREFR::MoveTo_Impl (SE 56227 / AE 56626) 직접 호출 래퍼
        bool InvokeMoveToImpl(
            RE::TESObjectREFR* self,
            const RE::ObjectRefHandle& targetHandle,
            RE::TESObjectCELL* targetCell,
            RE::TESWorldSpace* targetWorldspace,
            const RE::NiPoint3& pos,
            const RE::NiPoint3& angle)
        {
            if (!self || (!targetCell && !targetWorldspace)) {
                return false;
            }
            using MoveToImplFn = void (*)(
                RE::TESObjectREFR*,
                const RE::ObjectRefHandle&,
                RE::TESObjectCELL*,
                RE::TESWorldSpace*,
                const RE::NiPoint3&,
                const RE::NiPoint3&);
            static REL::Relocation<MoveToImplFn> func{ RELOCATION_ID(56227, 56626) };
            func(self, targetHandle, targetCell, targetWorldspace, pos, angle);
            // 엔진에 요청을 전달했는지만 나타낸다. 도착은 이후 틱에서 확인한다.
            return true;
        }

        Core::MapTravelTarget CaptureLocation(RE::TESObjectREFR& ref)
        {
            const auto* cell = ref.GetParentCell();
            const auto* world = ref.GetWorldspace();
            const auto& pos = ref.GetPosition();
            const auto& angle = ref.GetAngle();
            return { cell ? cell->GetFormID() : 0, world ? world->GetFormID() : 0,
                { pos.x, pos.y, pos.z }, { angle.x, angle.y, angle.z } };
        }

        std::optional<Core::MapTravelTarget> EntryTarget(const MapPackRuntime::DungeonForms& forms)
        {
            auto* marker = forms.entryMarker;
            if (!marker || marker->IsDeleted() || marker->GetParentCell() != forms.cell) {
                return std::nullopt;
            }
            auto target = CaptureLocation(*marker);
            target.marker = marker->GetFormID();
            return target;
        }

        void ResolveSafeReturnMarker()
        {
            auto* marker = RE::TESForm::LookupByID<RE::TESObjectREFR>(kSafeReturnMarkerId);
            const auto* base = marker ? marker->GetBaseObject() : nullptr;
            if (!marker || marker->GetFormType() != RE::FormType::Reference || marker->IsDeleted() ||
                !base || base->GetFormID() != kXMarkerHeadingId) {
                SKSE::log::error("MapDeviceRuntime: safe return marker 0x{:08X} is invalid.", kSafeReturnMarkerId);
                return;
            }
            auto destination = CaptureLocation(*marker);
            if (destination.worldspace == 0 && RE::TESForm::LookupByID<RE::TESWorldSpace>(kTamrielWorldspaceId)) {
                // 영구 야외 참조의 부모 셀을 아직 못 읽으면 월드스페이스가 비어 나온다(houseCARL로 Tamriel 확인).
                destination.cell = 0;
                destination.worldspace = kTamrielWorldspaceId;
            }
            if (destination.worldspace != kTamrielWorldspaceId) {
                SKSE::log::error("MapDeviceRuntime: safe return marker 0x{:08X} is outside Tamriel.", kSafeReturnMarkerId);
                return;
            }
            destination.marker = kSafeReturnMarkerId;
            g_safeReturnMarker = marker;
            g_safeReturnTarget = destination;
        }

        std::vector<RE::FormID> DisableRunSpawnsOnMainThread(std::uint64_t runId);

        bool IssueTravelMove(RE::PlayerCharacter* player, const Core::MapTravelTarget& target)
        {
            auto* cell = target.cell ? RE::TESForm::LookupByID<RE::TESObjectCELL>(target.cell) : nullptr;
            auto* world = target.worldspace ? RE::TESForm::LookupByID<RE::TESWorldSpace>(target.worldspace) : nullptr;
            const RE::NiPoint3 pos{ target.position[0], target.position[1], target.position[2] };
            const RE::NiPoint3 angle{ target.angle[0], target.angle[1], target.angle[2] };
            if (!player || (target.worldspace ? !world : !cell)) {
                return false;
            }
            RE::ObjectRefHandle handle;
            if (target.marker != 0) {
                auto* marker = target.marker == kSafeReturnMarkerId
                    ? g_safeReturnMarker : RE::TESForm::LookupByID<RE::TESObjectREFR>(target.marker);
                if (!marker || marker->IsDeleted()) {
                    return false;
                }
                handle = marker->GetHandle();
            } else {
                handle = player->CreateRefHandle();
            }
            return InvokeMoveToImpl(player, handle, cell, world, pos, angle);
        }

        Core::MapTravelTarget SavedReturn(const Core::ActiveMapState& state)
        {
            return { state.returnCellFormId, state.returnWorldspaceFormId, state.returnPos, state.returnAngle };
        }

        std::optional<Core::MapTravelTarget> ResolveReturnTarget(Core::MapTravelTarget saved)
        {
            auto* cell = saved.cell ? RE::TESForm::LookupByID<RE::TESObjectCELL>(saved.cell) : nullptr;
            auto* world = saved.worldspace ? RE::TESForm::LookupByID<RE::TESWorldSpace>(saved.worldspace) : nullptr;
            const auto finite = [](float value) { return std::isfinite(value); };
            if (!MapPackRuntime::OwnsCell(cell) && std::ranges::all_of(saved.position, finite) &&
                std::ranges::all_of(saved.angle, finite)) {
                if (world) {
                    // 실험 E: 야외는 조회 가능한 셀과 월드스페이스를 함께 넘긴다.
                    if (!cell || !cell->IsExteriorCell() || cell->GetRuntimeData().worldSpace != world) {
                        saved.cell = 0;
                    }
                    return saved;
                }
                if (!saved.worldspace && cell && cell->IsInteriorCell()) {
                    return saved;
                }
            }
            SKSE::log::warn("MapDeviceRuntime: return cell 0x{:08X} / ws 0x{:08X} unavailable.", saved.cell, saved.worldspace);
            return g_safeReturnTarget;
        }

        bool IsTravelLoading()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || ui->IsMenuOpen(RE::FaderMenu::MENU_NAME));
        }

        // 진단(2026-11-23 실행 19, H27 멈춤): 지도 팩 셀에서 플레이어가 출구 너머로 떨어지면 우리 낙하 복귀가 아닌 무언가가 화면을
        // 어둡게 하고(FaderMenu) 던전 안쪽으로 옮겼고, 그 페이더가 100초 넘게 닫히지 않았다. 엔진의 "마지막 안전 위치로 되돌리기"
        // (PlayerCharacter의 returnToLastKnownGoodPosition·lastKnownGoodPosition)인지 가르려고, 팩 셀 안에서 페이더 열림·닫힘과
        // 우리 이동이 아닌 순간이동을 그때의 엔진 값과 함께 남긴다. 동작은 바꾸지 않는다.
        struct FallSafetyProbe
        {
            bool faderOpen = false;
            bool returnFlag = false;
            std::optional<std::array<float, 3>> lastPos;
            RE::FormID lastCell = 0;
        };
        FallSafetyProbe g_fallSafetyProbe;

        void ObserveEngineFallSafety(RE::PlayerCharacter* player, const Core::MapTravelObservation& observation, bool travelPending)
        {
            auto& probe = g_fallSafetyProbe;
            auto* ui = RE::UI::GetSingleton();
            // observation.loading은 페이더도 로딩으로 본다. 페이더 열림을 보려고 로딩 화면일 때만 초기화한다.
            const bool loadingMenu = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
            if (!player || loadingMenu || !observation.inPackCell) {
                probe = {};
                return;
            }
            const bool faderOpen = ui && ui->IsMenuOpen(RE::FaderMenu::MENU_NAME);
            const bool mistOpen = ui && ui->IsMenuOpen(RE::MistMenu::MENU_NAME);
            const auto& runtime = player->GetPlayerRuntimeData();
            const bool returnFlag = runtime.playerFlags.returnToLastKnownGoodPosition;
            const auto& good = runtime.lastKnownGoodPosition;
            const auto& pos = observation.position;
            const auto describe = [&]() {
                return std::format("pos={:.0f},{:.0f},{:.0f}, lastKnownGood={:.0f},{:.0f},{:.0f}, returnFlag={}, fader={}, mist={}, travelPending={}, cell=0x{:08X}",
                    pos[0], pos[1], pos[2], good.x, good.y, good.z, returnFlag, faderOpen, mistOpen, travelPending, observation.cell);
            };
            if (probe.lastCell == observation.cell && probe.lastPos) {
                const auto& prev = *probe.lastPos;
                const float dx = pos[0] - prev[0];
                const float dy = pos[1] - prev[1];
                const float dz = pos[2] - prev[2];
                if (!travelPending && !faderOpen && dx * dx + dy * dy + dz * dz > 300.0f * 300.0f) {
                    SKSE::log::info("MapDeviceRuntime: player jumped in pack cell from {:.0f},{:.0f},{:.0f} without our travel: {}.",
                        prev[0], prev[1], prev[2], describe());
                }
            }
            if (faderOpen != probe.faderOpen || returnFlag != probe.returnFlag) {
                SKSE::log::info("MapDeviceRuntime: pack-cell fall safety changed (fader {}->{}, returnFlag {}->{}): {}.",
                    probe.faderOpen, faderOpen, probe.returnFlag, returnFlag, describe());
            }
            probe.faderOpen = faderOpen;
            probe.returnFlag = returnFlag;
            probe.lastPos = pos;
            probe.lastCell = observation.cell;
        }

        Core::MapTravelObservation ObservePlayer(RE::PlayerCharacter* player)
        {
            const bool loading = IsTravelLoading();
            if (!player) {
                return { .loading = loading };
            }
            const auto* cell = player->GetParentCell();
            const auto* world = player->GetWorldspace();
            const auto& pos = player->GetPosition();
            return { cell ? cell->GetFormID() : 0, world ? world->GetFormID() : 0,
                { pos.x, pos.y, pos.z }, player->Is3DLoaded(), loading, MapPackRuntime::OwnsCell(cell) };
        }

        class CellResetCallback final : public RE::BSScript::IStackCallbackFunctor
        {
        public:
            CellResetCallback(RE::FormID cellId, std::uint64_t seq) : cellId_(cellId), seq_(seq) {}

            // VM 스레드에서 불린다. 게임 상태는 건드리지 않고 대기 목록만 지도 잠금 아래에서 갱신한다.
            void operator()(RE::BSScript::Variable result) override
            {
                // Reset은 반환값이 없어 성공·실패를 구별할 수 없다. VM이 호출을 마쳤다는 것만 기록한다.
                if (result.GetType().GetRawType() != RE::BSScript::TypeInfo::RawType::kNone) {
                    SKSE::log::warn("MapDeviceRuntime: Cell.Reset callback returned an unexpected type for 0x{:08X}.", cellId_);
                }
                std::scoped_lock lk(g_mutex);
                const auto pending = g_pendingCellResets.find(cellId_);
                if (pending == g_pendingCellResets.end() || pending->second.seq != seq_ || !pending->second.inFlight) {
                    return;
                }
                g_pendingCellResets.erase(pending);
                ++g_completedCellResets[cellId_];
                SKSE::log::info("MapDeviceRuntime: Cell.Reset call returned for 0x{:08X} (no result value).", cellId_);
            }

            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            RE::FormID cellId_;
            std::uint64_t seq_;
        };

        // 셀이 붙어 있거나 붙고·떨어지는 중이 아닐 때만 Reset을 보낸다(실험 E: 셀이 떨어진 뒤 요청).
        [[nodiscard]] bool IsCellDetachedForReset(const RE::TESObjectCELL* cell) noexcept
        {
            using State = RE::TESObjectCELL::CellState;
            if (!cell) {
                return true;
            }
            const auto state = cell->cellState.get();
            return state == State::kUnloaded || state == State::kDataLoaded || state == State::kUnattached;
        }

        void QueueCellResetLocked(RE::FormID cellId)
        {
            if (cellId != 0 && !g_pendingCellResets.contains(cellId)) {
                g_pendingCellResets.emplace(cellId, PendingCellReset{ .seq = g_nextCellResetSeq++ });
            }
        }

        std::string_view DispatchCellReset(RE::TESObjectCELL* cell, std::uint64_t seq,
            std::optional<CellResetRejection>& rejection)
        {
            if (!cell) {
                return "cell lookup failed";
            }
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
            if (!policy) {
                return "VM or handle policy unavailable";
            }
            const auto handle = policy->GetHandleForObject(RE::FormType::Cell, cell);
            if (handle == policy->EmptyHandle()) {
                return "empty cell handle";
            }
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> callback{ new CellResetCallback(cell->GetFormID(), seq) };
            if (!vm->DispatchMethodCall2(handle, "Cell", "Reset", RE::MakeFunctionArguments(), callback)) {
                rejection = CellResetRejection{ cell->cellState.get(), cell->IsAttached(), handle,
                    policy->IsHandleObjectAvailable(handle), policy->HandleIsType(RE::FormType::Cell, handle) };
                return "VM rejected the request";
            }
            return {};
        }

        // 잠금 안: 상태만 보고 보낼 후보를 고른다(엔진 호출 없음).
        bool IsCellResetCandidateLocked(RE::FormID cellId, const Core::MapTravelObservation& observation)
        {
            const auto& travel = g_travel.Current();
            // 전 회차의 Reset이 밀렸어도 새 회차의 포털 귀환·재입장 사이에는 보내지 않는다.
            return !observation.loading && observation.cell != 0 && observation.cell != cellId &&
                g_activeCellFormId.load(std::memory_order_relaxed) != cellId && !(travel && travel->target.cell == cellId);
        }

        std::vector<std::pair<RE::FormID, std::uint64_t>> CollectCellResetCandidatesLocked(
            const Core::MapTravelObservation& observation)
        {
            std::vector<std::pair<RE::FormID, std::uint64_t>> candidates;
            for (const auto& [cellId, pending] : g_pendingCellResets) {
                if (!pending.inFlight && IsCellResetCandidateLocked(cellId, observation)) {
                    candidates.emplace_back(cellId, pending.seq);
                }
            }
            return candidates;
        }

        // 잠금 밖: 셀이 떨어져 있으면 Reset을 보내고, 결과를 잠금 안에서 대기 목록에 반영한다.
        // 접수되면 콜백을 기다리는 상태로 두고, 콜백이 와야 대기에서 뺀다.
        void SendCellResets(const std::vector<std::pair<RE::FormID, std::uint64_t>>& candidates)
        {
            for (const auto& [cellId, seq] : candidates) {
                auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(cellId);
                if (!IsCellDetachedForReset(cell)) {
                    continue;
                }
                std::optional<CellResetRejection> rejection;
                const auto failure = DispatchCellReset(cell, seq, rejection);
                std::scoped_lock lk(g_mutex);
                const auto pending = g_pendingCellResets.find(cellId);
                if (pending == g_pendingCellResets.end() || pending->second.seq != seq) {
                    continue;
                }
                if (!failure.empty()) {
                    if (!std::exchange(pending->second.warned, true)) {
                        SKSE::log::warn("MapDeviceRuntime: Cell.Reset dispatch failed for 0x{:08X} ({}); will retry.", cellId, failure);
                    }
                    if (rejection && pending->second.lastRejection != rejection) {
                        SKSE::log::info("MapDeviceRuntime: Cell.Reset VM rejected cell=0x{:08X}, cellState={}, IsAttached={}, handle=0x{:016X}, IsHandleObjectAvailable={}, HandleIsType(Cell)={}.",
                            cellId, static_cast<unsigned>(rejection->cellState), rejection->attached, rejection->handle,
                            rejection->handleAvailable, rejection->handleIsCell);
                    }
                    pending->second.lastRejection = rejection;
                    continue;
                }
                pending->second.lastRejection.reset();
                pending->second.inFlight = true;
                pending->second.inFlightSec = 0.0;
                ++g_sentCellResets[cellId];
                SKSE::log::info("MapDeviceRuntime: Cell.Reset dispatched for 0x{:08X}.", cellId);
            }
        }

        // 잠금 안: 대기 시간을 늘리고, 콜백이 오지 않는 접수는 다시 보낼 수 있게 되돌린다.
        void AdvanceCellResetTimersLocked(double deltaSec)
        {
            for (auto& [cellId, pending] : g_pendingCellResets) {
                pending.waitedSec += deltaSec;
                if (!pending.inFlight) {
                    continue;
                }
                pending.inFlightSec += deltaSec;
                if (pending.inFlightSec >= kCellResetCallbackTimeoutSec) {
                    pending.inFlight = false;
                    SKSE::log::warn("MapDeviceRuntime: Cell.Reset callback for 0x{:08X} did not arrive; sending again.", cellId);
                }
            }
        }

        using Core::MapTravelKind;

        RE::NiPointer<RE::Actor> LookupMapBoss(RE::FormID bossId, RE::FormID runCellId)
        {
            if (!bossId || !runCellId) {
                return {};
            }
            const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::FindDungeonByCell(runCellId));
            if (!forms || !forms->bossNpc) {
                return {};
            }
            RE::NiPointer<RE::Actor> boss{ RE::TESForm::LookupByID<RE::Actor>(bossId) };
            if (!boss || boss->IsDeleted()) {
                return {};
            }
            const auto* base = boss->GetBaseObject();
            const auto* cell = boss->GetParentCell();
            if (!Core::MapSystem::MatchesMapBoss(forms->bossNpc->GetFormID(), runCellId,
                    base ? base->GetFormID() : 0, cell ? cell->GetFormID() : 0)) {
                return {};
            }
            return boss;
        }

        bool IsBossDeadNow(RE::FormID bossId)
        {
            const auto boss = LookupMapBoss(bossId, g_activeCellFormId.load(std::memory_order_acquire));
            return boss && boss->IsDead();
        }

        void PromoteMapBoss(RE::Actor* boss, bool isArena4, std::uint8_t arenaPhase, const Core::MapModSummary& summary)
        {
            if (!boss || boss->IsDead() || boss->IsDisabled()) {
                return;
            }
            const auto modBit = [](Core::MonsterModKind kind) {
                return static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(kind));
            };
            std::uint16_t mask = modBit(Core::MonsterModKind::Armoured);
            if (isArena4 && arenaPhase == 2) {
                mask = modBit(Core::MonsterModKind::Hasted) | modBit(Core::MonsterModKind::ExtraFire);
            } else if (isArena4 && arenaPhase >= Core::kArena4FinalPhase) {
                mask = modBit(Core::MonsterModKind::Hasted) | modBit(Core::MonsterModKind::Deadly) |
                    modBit(Core::MonsterModKind::ExtraLightning);
            } else if (!isArena4) {
                if (summary.forceHasted) {
                    mask |= modBit(Core::MonsterModKind::Hasted);
                }
                if (summary.forceDeadly) {
                    mask |= modBit(Core::MonsterModKind::Deadly);
                }
            }
            MonsterRuntime::ForcePromoteForTest(boss, Core::MonsterRarity::Rare, mask, -1.0);
        }

        bool ShouldDeferBossDeathLocked(bool bossDeadNow)
        {
            const auto now = std::chrono::steady_clock::now();
            const double waitedSec = std::chrono::duration<double>(
                now - g_firstBossDeathReturnDeferral.value_or(now)).count();
            const bool deathConfirmed = g_bossDeathConfirmedRunId == g_activeMap.runId;
            const bool killPending = g_bossDeathPendingRunId == g_activeMap.runId;
            if (!Core::MapSystem::ShouldDeferReturnForBossDeath(g_activeMap.bossKilled,
                    bossDeadNow, deathConfirmed, killPending, waitedSec)) {
                return false;
            }
            if (!g_firstBossDeathReturnDeferral) {
                g_firstBossDeathReturnDeferral = now;
            }
            return true;
        }

        // 남은 스폰 청소가 Disable을 요청한 액터 가운데 아직 꺼지지 않은 것. 꺼졌거나 사라진 것은 목록에서 뺀다.
        std::vector<RE::FormID> TakeUndisabledSweepRequests()
        {
            std::vector<RE::FormID> requested;
            {
                std::scoped_lock lk(g_mutex);
                requested = g_sweepDisableRequested;
            }
            std::vector<RE::FormID> undisabled;
            for (const auto id : requested) {
                const RE::NiPointer<RE::Actor> actor{ RE::TESForm::LookupByID<RE::Actor>(id) };
                if (actor && !actor->IsDeleted() && !actor->IsDisabled()) {
                    undisabled.push_back(id);
                }
            }
            std::scoped_lock lk(g_mutex);
            std::erase_if(g_sweepDisableRequested, [&](RE::FormID id) {
                return std::ranges::find(requested, id) != requested.end() &&
                    std::ranges::find(undisabled, id) == undisabled.end();
            });
            return undisabled;
        }

        // waitForDisabled: 호출자가 방금 Disable을 요청한 액터(자동 취소된 회차의 정리 등). 비어 있지 않으면 끝낼 귀환과
        // 같이 그 액터들이 비활성화된 것을 확인한 다음 프레임에 이동한다.
        MapActionResult StartTravel(RE::PlayerCharacter* player, MapTravelKind kind,
            const Core::MapTravelTarget& target, const TravelContext& context, std::string message,
            std::vector<RE::FormID> waitForDisabled = {})
        {
            if (MapDeviceRuntime::IsUninstallCleanupArmed()) {
                // 정리 완료 뒤에는 이동을 갱신하지 않아 도착 확인도 시간 초과도 오지 않는다.
                // 늦게 도착한 입장 등으로 지도 팩 셀에 갇히지 않게 안전 탈출만 추적 없이 이동시킨다.
                if (kind == MapTravelKind::SafeEscape && IssueTravelMove(player, target)) {
                    return { true, std::move(message) };
                }
                return { false, "안전 제거 정리 완료 상태입니다. 세이브 후 종료하거나 다시 불러오세요." };
            }
            const bool entry = Core::IsMapEntry(kind);
            const bool retrySavedReturn = !entry && target.marker != kSafeReturnMarkerId;
            // 귀환은 목표가 고정 마커여도 같은 목표로 정한 횟수만큼 다시 이동한다. 입장은 한 번만 기다린다.
            const Core::MapTravelRequest request{
                kind, target, g_travelSettings.arrivalRadius, !entry,
                retrySavedReturn ? g_safeReturnTarget : std::nullopt,
                entry ? 1u : g_travelSettings.maxAttempts, g_travelSettings.timeoutSec
            };
            if (!entry) {
                const auto swept = TakeUndisabledSweepRequests();
                for (const auto id : swept) {
                    if (std::ranges::find(waitForDisabled, id) == waitForDisabled.end()) {
                        waitForDisabled.push_back(id);
                    }
                }
                if (!swept.empty()) {
                    SKSE::log::info("MapDeviceRuntime: return (kind={}) also waits for {} swept leftover actor(s) to be disabled.",
                        static_cast<int>(kind), swept.size());
                }
            }
            bool sync = false;
            std::uint64_t endingRunId = 0;
            bool awaitBossDeath = false;
            bool awaitCleanupDisable = false;
            const auto bossId = kind == MapTravelKind::Return
                ? g_activeBossFormId.load(std::memory_order_acquire) : 0u;
            const bool bossDeadNow = IsBossDeadNow(bossId);
            {
                std::scoped_lock lk(g_mutex);
                const bool currentRun = context.runId != 0 && g_activeMap.active && g_activeMap.runId == context.runId;
                if (PendingTravelKindLocked()) {
                    return { false, "이동 중입니다. 도착 후 다시 시도하세요." };
                }
                if (kind == MapTravelKind::Return && currentRun &&
                    ShouldDeferBossDeathLocked(bossDeadNow && bossId == g_activeMap.bossRefFormId)) {
                    return { false, "보스 처치를 처리하는 중입니다. 잠시 후 다시 귀환하세요." };
                }
                if (!g_travel.TryStart(request)) {
                    return { false, "이동 중입니다. 도착 후 다시 시도하세요." };
                }
                g_bossMissingSec = 0.0;
                g_travelContext = context;
                if (entry) {
                    // 입장 대기 중 지도 상태가 꺼져 있어 사망 가로채기가 새로 발동할 수 없다.
                    g_permittedCell = 0;
                    sync = g_playerInActiveMapCell.exchange(false, std::memory_order_acq_rel);
                } else if (currentRun) {
                    if (kind == MapTravelKind::Abandon) {
                        // 이동 실패 뒤에도 종료 요청을 유지한다. 회차 스폰은 이미 비활성화됐을 수 있다.
                        g_activeMap.endRequested = true;
                    }
                    const bool ending = Core::MapSystem::ShouldEndOnReturn(g_activeMap);
                    // 결정 179 ⑦: 사망이 시작된 보스의 처치 반영 전에는 회차를 끝내는 귀환과 사망 귀환이 떠나지 않는다.
                    // 처치가 반영되면 결정 175대로 이 귀환이 회차를 끝내고, 비활성화된 보스는 사망 확정이 오지 않을 수 있다.
                    awaitBossDeath = (ending || kind == MapTravelKind::DeathEscape) &&
                        Core::MapSystem::ShouldHoldEndingDisableForBossDeath(
                            BossKillProcessingPendingLocked(context.runId), g_activeMap.bossKilled, 0.0);
                    if (ending || awaitBossDeath) {
                        g_pendingTravelDisable = PendingTravelDisable{ MainUpdateHook::TickCount(), waitForDisabled };
                        g_pendingTravelDisable->awaitingBossDeath = awaitBossDeath;
                        // 일반 귀환이 이미 보스 처리를 기다리며 거절됐던 시간은 이 대기의 상한에 넣는다(10초를 두 번 기다리지 않게).
                        if (awaitBossDeath && g_firstBossDeathReturnDeferral) {
                            g_pendingTravelDisable->bossWaitSec = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - *g_firstBossDeathReturnDeferral).count();
                        }
                    }
                    if (ending) {
                        endingRunId = context.runId;
                        // 어떤 귀환이 회차를 끝내는지(사망 귀환으로 실패한 결투장 등) 실행 기록에서 가리기 위한 진단.
                        SKSE::log::info("MapDeviceRuntime: ending return accepted for run {} (kind={}, failed={}, bossKilled={}, endRequested={}, portals={}).",
                            context.runId, static_cast<int>(kind), g_activeMap.failed, g_activeMap.bossKilled,
                            g_activeMap.endRequested, g_activeMap.portalsRemaining);
                    }
                }
                if (!entry && !waitForDisabled.empty() && !g_pendingTravelDisable) {
                    g_pendingTravelDisable = PendingTravelDisable{ MainUpdateHook::TickCount(), std::move(waitForDisabled) };
                    awaitCleanupDisable = true;
                }
            }
            if (sync) {
                MapDeviceRuntime::SyncMapLayer2ToStatEngine();
            }
            if (awaitBossDeath) {
                SKSE::log::info("MapDeviceRuntime: return run {} (kind={}, ending={}) waits for the dying boss to be processed.",
                    context.runId, static_cast<int>(kind), endingRunId != 0);
            } else if (awaitCleanupDisable) {
                SKSE::log::info("MapDeviceRuntime: return (kind={}) waits for cleaned-up spawns to be disabled before moving.",
                    static_cast<int>(kind));
            } else if (endingRunId != 0) {
                auto actorIds = DisableRunSpawnsOnMainThread(endingRunId);
                std::scoped_lock lk(g_mutex);
                if (!g_pendingTravelDisable || g_travelContext.runId != endingRunId) {
                    return { false, "회차가 바뀌어 귀환 요청을 취소했습니다." };
                }
                auto& waitIds = g_pendingTravelDisable->actorIds;
                for (const auto id : actorIds) {
                    if (std::ranges::find(waitIds, id) == waitIds.end()) {
                        waitIds.push_back(id);
                    }
                }
            } else if (!IssueTravelMove(player, target)) {
                std::scoped_lock lk(g_mutex);
                ClearTravelLocked();
                return { false, "이동을 요청하지 못했습니다. 다시 시도하세요." };
            }
            return { true, std::move(message) };
        }

        MapActionResult RequestReturn(RE::PlayerCharacter* player, MapTravelKind kind,
            const Core::MapTravelTarget& saved, std::uint64_t runId, std::string message,
            std::vector<RE::FormID> waitForDisabled = {})
        {
            const auto target = kind == MapTravelKind::SafeEscape ? g_safeReturnTarget : ResolveReturnTarget(saved);
            if (!target) {
                return { false, "안전 귀환 마커를 찾을 수 없어 이동을 요청하지 못했습니다. 다시 시도하세요." };
            }
            return StartTravel(player, kind, *target, { .runId = runId }, std::move(message), std::move(waitForDisabled));
        }

        void ShowMapMessage(const MapActionResult& result)
        {
            RE::SendHUDMessage::ShowHUDMessage(result.message.c_str());
        }

        void RecoverFromMapFall(RE::PlayerCharacter* player, const Core::MapDungeonDef& dungeon, bool authorized, bool usePortal)
        {
            if (!authorized) {
                ShowMapMessage(MapDeviceRuntime::SafeEscape(player));
                return;
            }
            const auto* forms = MapPackRuntime::FindForms(&dungeon);
            const auto target = forms && !usePortal ? EntryTarget(*forms) : std::nullopt;
            if (target && IssueTravelMove(player, *target)) {
                {
                    std::scoped_lock lk(g_mutex);
                    ++g_fallRecoveryCount;
                }
                RE::SendHUDMessage::ShowHUDMessage("낙하하여 던전 입구로 돌아갑니다.");
                return;
            }
            // 입구로 옮길 수 없거나 옮긴 뒤에도 문턱 아래면 계속 떨어지지 않게 포털 귀환(포털 차감 없음)으로 내보낸다.
            // 결정 175에 따라 보스 처치 뒤나 포털 0(결투장)이면 이 귀환으로 회차가 끝난다.
            SKSE::log::warn("MapDeviceRuntime: could not recover at the entry after a fall in {}; returning instead.", dungeon.key);
            ShowMapMessage(MapDeviceRuntime::ReturnPortal(player));
        }

        bool RefundEntryOnMainThread(
            RE::PlayerCharacter* player, std::uint8_t tier, bool arena,
            const std::optional<Core::PoEItemRecord>& tracked)
        {
            const auto localId = arena ? FormIds::kAscendancy4Tribute : FormIds::MapItemLocalFormId(tier);
            auto* item = FormIds::Lookup<RE::TESObjectMISC>(localId);
            if (!player || !item) {
                SKSE::log::error("MapDeviceRuntime: missing player or entry item 0x{:06X} for refund.", localId);
                return false;
            }
            const auto beforeCount = player->GetItemCount(item);
            const auto addPlain = [&]() {
                player->AddObjectToContainer(item, nullptr, 1, nullptr);
                return player->GetItemCount(item) > beforeCount;
            };
            if (!tracked) {
                return addPlain();
            }

            // 재검수 R-15: 추적에 실패해도 원래 소유한 지도는 플레이어에게 남긴다.
            bool untrackedLeft = false;
            if (auto* extra = UniqueIdUtil::CreateGameExtraDataList()) {
                const auto key = ItemTracker::AddObjectWithTrackedUniqueId(player, item, extra, nullptr,
                    ItemTracker::UntrackedAddPolicy::Keep, &untrackedLeft);
                if (key.IsValid()) {
                    if (player->GetItemCount(item) <= beforeCount) {
                        return false;
                    }
                    auto record = *tracked;
                    record.baseFormId = item->GetFormID();
                    ItemTracker::Upsert(key, record, false);
                    return true;
                }
            }
            if (!(untrackedLeft ? player->GetItemCount(item) > beforeCount : addPlain())) {
                return false;
            }
            SKSE::log::error("MapDeviceRuntime: refunded map without tracking; its affixes were lost.");
            RE::SendHUDMessage::ShowHUDMessage("지도를 돌려받았지만 추적에 실패해 지도 속성이 사라졌습니다.");
            return true;
        }

        std::optional<Core::PoEItemRecord> RecreateSavedMapRefund(const Core::ActiveMapState& state)
        {
            if (!state.active || state.isArena4 || state.mods.empty()) {
                return std::nullopt;
            }
            // MAPS v1·v2의 어픽스와 티어로 제작 정보를 복원한다. 양쪽에 하나씩 붙은 두 속성은 매직으로 복원한다.
            Core::PoEItemRecord record;
            record.rarity = Core::ItemRarity::Magic;
            if (state.mods.size() > 2) {
                record.rarity = Core::ItemRarity::Rare;
            } else if (state.mods.size() == 2 && g_itemData) {
                const auto* first = g_itemData->FindAffix(state.mods[0].affixId);
                const auto* second = g_itemData->FindAffix(state.mods[1].affixId);
                if (first && second && first->type == second->type) {
                    record.rarity = Core::ItemRarity::Rare;
                }
            }
            record.slotGroup = Core::TokenSlotGroup::Map;
            record.itemLevel = Core::MapSystem::MonsterLevelForTier(state.tier);
            record.affixes = state.mods;
            return record;
        }

        // 그 던전 셀의 스폰 원형(보스·무리·증원 NPC) FormID. 셀을 모르면 빈 목록(소유를 입증할 수 없음).
        std::vector<RE::FormID> SpawnTemplateIdsForCell(RE::FormID runCellId)
        {
            std::vector<RE::FormID> ids;
            const auto* dungeon = MapPackRuntime::FindDungeonByCell(runCellId);
            const auto* forms = dungeon ? MapPackRuntime::FindForms(dungeon) : nullptr;
            if (!forms) {
                return ids;
            }
            if (forms->bossNpc) {
                ids.push_back(forms->bossNpc->GetFormID());
            }
            for (const auto* npc : forms->packNpcs) {
                ids.push_back(npc->GetFormID());
            }
            for (const auto* npc : forms->addNpcs) {
                ids.push_back(npc->GetFormID());
            }
            return ids;
        }

        // 진단(2026-11-23 실행 15): 붙지 않은 셀에 남긴 스폰 가운데 보스만 셀 초기화 뒤에도 살아 남았다. 무엇이 다른지 가르려고
        // 엔진 쪽 상태(전투·처리 단계·영속 표시 등)를 남긴다. 동작은 바꾸지 않는다.
        std::string DescribeSpawnEngineState(const RE::Actor* actor)
        {
            if (!actor) {
                return "actor=null";
            }
            auto* self = const_cast<RE::Actor*>(actor);
            const auto& runtime = self->GetActorRuntimeData();
            const auto* process = runtime.currentProcess;
            const char* processLevel = !process ? "none"
                : process->InHighProcess() ? "high"
                : process->InMiddleHighProcess() ? "middle-high"
                : "low-or-middle-low";
            const auto target = runtime.currentCombatTarget.get();
            const auto* player = RE::PlayerCharacter::GetSingleton();
            return std::format(
                "dead={}, disabled={}, deleted={}, inCombat={}, target={}, angryWithPlayer={}, process={}, 3D={}, persistent={}, teammate={}, commanded={}",
                self->IsDead(), self->IsDisabled(), self->IsDeleted(), self->IsInCombat(),
                !target ? "none" : target.get() == player ? "player" : std::format("0x{:08X}", target->GetFormID()),
                self->IsAngryWithPlayer(), processLevel, self->Is3DLoaded(), self->IsPersistent(), self->IsPlayerTeammate(),
                self->IsCommandedActor());
        }

        void ResetMapSpawnTracking(RE::FormID actorId)
        {
            AilmentRuntime::OnActorReset(actorId);
            MonsterRuntime::OnActorReset(actorId);
        }

        enum class DisableSpawnResult
        {
            Requested,        // Disable을 예약하고 삭제 대기에 넣음
            AlreadyPending,   // 같은 회차로 이미 삭제 대기 중
            Missing,          // 조회 불가·삭제 표시·플레이어·동적 FormID 아님
            NotOwned,         // 회차 셀·스폰 원형이 맞지 않음(FormID 재사용 대비)
            CellDetached,     // 셀이 붙어 있지 않아 Disable하지 않음(결정 178 ⑦)
            Stale             // 처리 중 불러오기 등으로 세대가 바뀜
        };

        const char* DisableSpawnResultName(DisableSpawnResult result) noexcept
        {
            switch (result) {
                case DisableSpawnResult::Requested: return "requested";
                case DisableSpawnResult::AlreadyPending: return "pending";
                case DisableSpawnResult::Missing: return "missing";
                case DisableSpawnResult::NotOwned: return "not-owned";
                case DisableSpawnResult::CellDetached: return "cell-detached";
                case DisableSpawnResult::Stale: return "stale";
            }
            return "?";
        }

        // 결정 178 ①: 비활성화할 때도 우리 스폰임을 입증한다. FormID가 재사용돼 다른 액터(소환수 등)를
        // 끄거나 지우지 않도록, 그 회차 셀에 있고 기본 객체가 그 던전의 스폰 원형인 액터만 끈다.
        DisableSpawnResult DisableMapSpawnOnMainThread(RE::FormID actorId, std::uint64_t runId, RE::FormID runCellId,
            std::span<const RE::FormID> templateIds)
        {
            if ((actorId & kDynamicFormMask) != kDynamicFormMask || runCellId == 0 || templateIds.empty()) {
                return DisableSpawnResult::Missing;
            }
            std::uint64_t generation = 0;
            {
                std::scoped_lock lk(g_mutex);
                if (std::ranges::any_of(g_pendingActorDeletes, [actorId, runId](const PendingActorDelete& entry) {
                        return entry.actorId == actorId && entry.runId == runId;
                    })) {
                    return DisableSpawnResult::AlreadyPending;
                }
                generation = g_pendingActorDeleteGeneration;
            }
            const RE::NiPointer<RE::Actor> actor{ RE::TESForm::LookupByID<RE::Actor>(actorId) };
            if (!actor || actor->IsPlayerRef() || actor->IsDeleted()) {
                return DisableSpawnResult::Missing;
            }
            const auto* cell = actor->GetParentCell();
            const auto* baseObject = actor->GetBaseObject();
            const RE::FormID actorBaseId = baseObject ? baseObject->GetFormID() : 0;
            if (!cell || cell->GetFormID() != runCellId ||
                std::ranges::find(templateIds, actorBaseId) == templateIds.end()) {
                return DisableSpawnResult::NotOwned;
            }
            // 사망한 액터의 희귀도와 상태이상은 예약된 처치 처리가 읽고 정리한다.
            if (!actor->IsDead()) {
                ResetMapSpawnTracking(actorId);
            }
            // 결정 178 ⑦(2026-11-23 사용자 확인): 붙어 있지 않은 셀의 임시 액터는 엔진이 정리하며 지울 수 있다. 예약된 Disable이
            // 삭제된 액터에서 실행되며 CTD가 난 기록이 두 번 있다(실행 2의 1·13회차, 그 Disable을 우리가 예약했는지는 미확정).
            // 그런 액터는 끄지 않고 회차 종료 때 예약하는 셀 초기화에 맡긴다.
            if (!cell->IsAttached()) {
                SKSE::log::info("MapDeviceRuntime: run {} spawn 0x{:08X} (base 0x{:08X}) left to Cell.Reset in detached cell 0x{:08X}: {}.",
                    runId, actorId, actorBaseId, runCellId, DescribeSpawnEngineState(actor.get()));
                return DisableSpawnResult::CellDetached;
            }
            actor->Disable();

            RE::FormID evictedActorId = 0;
            {
                std::scoped_lock lk(g_mutex);
                if (generation != g_pendingActorDeleteGeneration) {
                    return DisableSpawnResult::Stale;
                }
                // 다른 회차의 같은 FormID는 번호가 재사용된 것이므로 새 대기 항목으로 바꾼다.
                std::erase_if(g_pendingActorDeletes, [actorId](const PendingActorDelete& entry) { return entry.actorId == actorId; });
                if (g_pendingActorDeletes.size() >= kMaxPendingMapActorDeletes) {
                    evictedActorId = g_pendingActorDeletes.front().actorId;
                    g_pendingActorDeletes.pop_front();
                }
                g_pendingActorDeletes.push_back({ actorId, runId, actorBaseId, runCellId, std::nullopt, 0.0, std::nullopt });
            }
            if (evictedActorId != 0) {
                ResetMapSpawnTracking(evictedActorId);
                SKSE::log::warn("MapDeviceRuntime: dropped pending deletion of actor 0x{:08X} at queue capacity {}.",
                    evictedActorId, kMaxPendingMapActorDeletes);
            }
            return DisableSpawnResult::Requested;
        }

        // 스폰 정리 결과를 종류별 개수와 FormID로 남긴다(정리 경로에서 누구에게 Disable을 걸었는지 가리기 위함).
        struct DisableSpawnTally
        {
            std::array<std::vector<RE::FormID>, 6> ids;

            void Add(DisableSpawnResult result, RE::FormID id) { ids[static_cast<std::size_t>(result)].push_back(id); }

            [[nodiscard]] std::string Describe() const
            {
                std::string out;
                for (std::size_t i = 0; i < ids.size(); ++i) {
                    if (ids[i].empty()) {
                        continue;
                    }
                    out += std::format("{}{}={}[", out.empty() ? "" : " ",
                        DisableSpawnResultName(static_cast<DisableSpawnResult>(i)), ids[i].size());
                    for (std::size_t k = 0; k < ids[i].size(); ++k) {
                        out += std::format("{}{:08X}", k == 0 ? "" : ",", ids[i][k]);
                    }
                    out += "]";
                }
                return out.empty() ? std::string("none") : out;
            }
        };

        std::vector<RE::FormID> DisableRunSpawnsOnMainThread(std::uint64_t runId)
        {
            std::vector<RE::FormID> spawnedIds;
            std::vector<RE::FormID> requestedIds;
            RE::FormID cellId = 0;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_activeMap.runId != runId) {
                    return {};
                }
                spawnedIds = Core::MapSystem::MapSpawnCleanupIds(g_activeMap);
                requestedIds = g_runSpawnDisableRequests;
                cellId = g_activeCellFormId.load(std::memory_order_relaxed);
                g_activeMap.spawnsDisabled = true;
                g_bossMissingSec = 0.0;
            }
            const auto templateIds = SpawnTemplateIdsForCell(cellId);
            const auto previouslyRequested = requestedIds.size();
            DisableSpawnTally tally;
            for (const auto id : spawnedIds) {
                if (std::ranges::find(requestedIds, id) != requestedIds.end()) {
                    continue;
                }
                const auto result = DisableMapSpawnOnMainThread(id, runId, cellId, templateIds);
                if (result == DisableSpawnResult::Requested) {
                    requestedIds.push_back(id);
                } else {
                    tally.Add(result, id);
                }
            }
            SKSE::log::info("MapDeviceRuntime: ending return run {} requested Disable for {} actor(s) (tracking {}, frame={}; not disabled: {}).",
                runId, requestedIds.size() - previouslyRequested, requestedIds.size(), MainUpdateHook::TickCount(),
                tally.Describe());
            {
                std::scoped_lock lk(g_mutex);
                if (g_activeMap.active && g_activeMap.runId == runId) {
                    g_runSpawnDisableRequests = requestedIds;
                }
            }
            return requestedIds;
        }

        void TickPendingActorDeletes(double deltaSec)
        {
            if (!std::isfinite(deltaSec) || deltaSec <= 0.0) {
                return;
            }
            std::vector<PendingActorDelete> due;
            std::vector<RE::FormID> expired;
            std::uint64_t generation = 0;
            {
                std::scoped_lock lk(g_mutex);
                generation = g_pendingActorDeleteGeneration;
                std::erase_if(g_pendingActorDeletes, [deltaSec, &expired](PendingActorDelete& pending) {
                    pending.ageSec += deltaSec;
                    if (pending.ageSec < kMapActorDeleteGiveUpSec) {
                        return false;
                    }
                    expired.push_back(pending.actorId);
                    return true;
                });
                for (auto& pending : g_pendingActorDeletes) {
                    if (!pending.untilLookupSec) {
                        // 현재 프레임 delta에는 비활성화 전의 시간도 들어 있을 수 있다.
                        pending.untilLookupSec = kMapActorDeleteDelaySec;
                        continue;
                    }
                    *pending.untilLookupSec -= deltaSec;
                    if (*pending.untilLookupSec <= 0.0) {
                        due.push_back(pending);
                        pending.untilLookupSec = kMapActorDeleteRetrySec;
                    }
                }
            }
            const auto isCurrentGeneration = [generation]() {
                std::scoped_lock lk(g_mutex);
                return generation == g_pendingActorDeleteGeneration;
            };
            for (const auto id : expired) {
                if (!isCurrentGeneration()) {
                    return;
                }
                ResetMapSpawnTracking(id);
            }
            if (!expired.empty()) {
                SKSE::log::info("MapDeviceRuntime: gave up deleting {} disabled map actor(s) after {:.0f}s.",
                    expired.size(), kMapActorDeleteGiveUpSec);
            }
            for (const auto& pending : due) {
                if (!isCurrentGeneration()) {
                    return;
                }
                auto* form = RE::TESForm::LookupByID(pending.actorId);
                if (!form) {
                    continue;
                }
                const RE::NiPointer<RE::Actor> actor{ form->As<RE::Actor>() };
                const auto* base = actor ? actor->GetBaseObject() : nullptr;
                const auto* cell = actor ? actor->GetParentCell() : nullptr;
                const Core::MapSpawnDeletionObservation observed{
                    .isActor = static_cast<bool>(actor),
                    .isPlayer = actor && actor->IsPlayerRef(),
                    .isDisabled = actor && actor->IsDisabled(),
                    .actorBaseId = base ? base->GetFormID() : 0,
                    .cellId = cell ? cell->GetFormID() : 0
                };
                if (actor && !observed.isPlayer && observed.actorBaseId == pending.actorBaseId &&
                    ((!cell && observed.isDisabled) || (cell && observed.cellId == pending.runCellId && !observed.isDisabled))) {
                    // 원형이 맞는 액터는 셀이 다시 읽히거나(비활성인데 셀 없음), 예약된 Disable이 아직 실행되지 않았으면
                    // (그 셀에 있고 아직 활성) 다시 관찰한다. 끝내 안 되면 포기 시간(kMapActorDeleteGiveUpSec)에 정리된다.
                    continue;
                }
                {
                    std::scoped_lock lk(g_mutex);
                    if (generation != g_pendingActorDeleteGeneration) {
                        return;
                    }
                    if (std::erase_if(g_pendingActorDeletes, [&pending](const PendingActorDelete& entry) {
                            return entry.actorId == pending.actorId && entry.runId == pending.runId;
                        }) == 0) {
                        continue;
                    }
                }
                if (actor && Core::MapSystem::CanDeleteMapSpawn(pending.actorBaseId, pending.runCellId, observed)) {
                    const auto actorId = actor->GetFormID();
                    const auto cellState = cell->cellState.get();
                    const bool loaded3D = actor->Is3DLoaded();
                    // 귀환 대기에서 관찰하지 않은 액터는 이 삭제 검사에서 처음 kDisabled를 확인했다.
                    const double sinceDisabled = pending.ageSec - pending.firstDisabledAgeSec.value_or(pending.ageSec);
                    actor->SetDelete(true);
                    SKSE::log::info("MapDeviceRuntime: SetDelete run {}, actor=0x{:08X}, base=0x{:08X}, cell=0x{:08X}, cellState={}, Is3DLoaded={}, sinceDisableRequest={:.3f}s, sinceFirstObservedDisabled={:.3f}s, IsDeleted={}.",
                        pending.runId, actorId, observed.actorBaseId, observed.cellId, static_cast<unsigned>(cellState),
                        loaded3D, pending.ageSec, sinceDisabled, actor->IsDeleted());
                } else {
                    SKSE::log::warn("MapDeviceRuntime: skipped deleting actor 0x{:08X}: actor={}, player={}, disabled={}, base=0x{:08X}/0x{:08X}, cell=0x{:08X}/0x{:08X}.",
                        pending.actorId, observed.isActor, observed.isPlayer, observed.isDisabled,
                        observed.actorBaseId, pending.actorBaseId, observed.cellId, pending.runCellId);
                }
                ResetMapSpawnTracking(pending.actorId);
            }
        }

        // 이번에 Disable을 요청한 액터 목록을 돌려준다. 플레이어가 아직 그 셀에 있으면 이동 전에 비활성화를 기다려야 한다.
        std::vector<RE::FormID> CleanupClosedRunOnMainThread(std::span<const RE::FormID> spawnedIds, std::uint64_t runId,
            RE::FormID cellId, std::span<const RE::FormID> disableRequested = {})
        {
            WorldDropTracker::DiscardDropsFromCorpses(spawnedIds, cellId);
            // 회차가 닫힌 뒤 늦게 온 사망 확정이 등록부 밖(오픈월드) 처치로 보상받지 않게 기여 기록을 지운다.
            DeathListener::ForgetKillRecords(spawnedIds);
            // 목록 밖의 보스도 정리하되, 삭제 전에 원형과 셀을 확인한다.
            const auto templateIds = SpawnTemplateIdsForCell(cellId);
            std::vector<RE::FormID> requested;
            DisableSpawnTally tally;
            for (const auto id : spawnedIds) {
                if (std::ranges::find(disableRequested, id) != disableRequested.end()) {
                    continue;
                }
                const auto result = DisableMapSpawnOnMainThread(id, runId, cellId, templateIds);
                tally.Add(result, id);
                if (result == DisableSpawnResult::Requested) {
                    requested.push_back(id);
                }
            }
            const auto* cell = cellId != 0 ? RE::TESForm::LookupByID<RE::TESObjectCELL>(cellId) : nullptr;
            SKSE::log::info("MapDeviceRuntime: closed run {} cleanup of {} spawn(s) (already handled {}; cell 0x{:08X} attached={}, frame={}): {}.",
                runId, spawnedIds.size(), disableRequested.size(), cellId, cell && cell->IsAttached(),
                MainUpdateHook::TickCount(), tally.Describe());
            if (cellId != 0) {
                std::scoped_lock lk(g_mutex);
                QueueCellResetLocked(cellId);
            }
            return requested;
        }

        void FinishCancelledRunOnMainThread(
            RE::PlayerCharacter* player, const Core::ActiveMapState& snap, bool returnPlayer,
            std::optional<bool> refunded, std::string_view reason, RE::FormID cellId)
        {
            // refunded: nullopt = 돌려줄 대상이 아님(보스를 처치했거나 실패한 회차), true/false = 환불 성공 여부
            auto disabledIds = CleanupClosedRunOnMainThread(Core::MapSystem::MapSpawnCleanupIds(snap), snap.runId, cellId);
            const std::string_view refundText = !refunded.has_value()
                ? "이미 끝난 회차(보스 처치 또는 실패)라 소모품은 돌려주지 않습니다."
                : (*refunded ? (snap.isArena4 ? "공물을 돌려받았습니다." : "지도를 돌려받았습니다.")
                             : "소모품을 돌려주지 못했습니다.");
            const auto msg = std::format("{} {}", reason, refundText);
            SKSE::log::warn("MapDeviceRuntime: cancelled run {}. {}", snap.runId, msg);
            RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
            if (returnPlayer) {
                // 끝낼 귀환과 같이, 방금 요청한 Disable이 실행된 뒤에 셀을 떠난다(Disable은 작업 풀 예약이라 비동기).
                ShowMapMessage(RequestReturn(player, MapTravelKind::CancelledRun, SavedReturn(snap), 0, "귀환을 요청했습니다.",
                    std::move(disabledIds)));
            }
        }

        void EscapeDamagedRunOnMainThread(RE::PlayerCharacter* player)
        {
            RE::SendHUDMessage::ShowHUDMessage("[지도 장치] 회차 기록이 손상되어 지도를 복원할 수 없습니다.");
            ShowMapMessage(RequestReturn(player, MapTravelKind::SafeEscape, {}, 0, "안전 탈출을 요청했습니다."));
        }

        void ShowAtlasDamageOnMainThread()
        {
            RE::SendHUDMessage::ShowHUDMessage("[지도 장치] 아틀라스 기록을 읽지 못해 진행도가 초기화되었습니다. 첫 공물은 지급된 것으로 처리합니다.");
        }

        [[nodiscard]] bool HasMapSpawnCapacityLocked()
        {
            const auto& ids = g_activeMap.spawnedRefFormIds;
            const auto bossId = g_activeMap.bossRefFormId;
            const bool separateBoss = bossId != 0 && std::ranges::find(ids, bossId) == ids.end();
            return ids.size() + (separateBoss ? 1u : 0u) < Core::kMaxMapSpawnedActors;
        }

        RE::NiPointer<RE::Actor> SpawnMapNpcAtAnchor(
            RE::PlayerCharacter* player,
            RE::TESObjectREFR* anchor,
            RE::TESNPC* npcBase,
            const RE::NiPoint3& offset,
            std::uint64_t runId,
            bool isBoss = false)
        {
            if (!player || !anchor || anchor->IsDeleted() || !npcBase) {
                return {};
            }
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_activeMap.runId != runId || !g_dataPack) {
                    return {};
                }
                if (!HasMapSpawnCapacityLocked()) {
                    SKSE::log::warn("MapDeviceRuntime: skipped spawn for run {} (tracking cap {}).",
                        runId, Core::kMaxMapSpawnedActors);
                    return {};
                }
            }
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            auto* cell = anchor->GetParentCell();
            if (!dataHandler || !cell || cell->GetFormID() != g_activeCellFormId.load(std::memory_order_acquire)) {
                return {};
            }
            RE::NiPoint3 spawnPos = anchor->GetPosition();
            spawnPos.x += offset.x;
            spawnPos.y += offset.y;
            spawnPos.z += offset.z;
            const auto handle = dataHandler->CreateReferenceAtLocation(
                npcBase,
                spawnPos,
                anchor->GetAngle(),
                cell,
                anchor->GetWorldspace(),
                nullptr,
                nullptr,
                RE::ObjectRefHandle{},
                false,
                true);
            auto ptr = handle.get();
            auto* actor = ptr ? ptr->As<RE::Actor>() : nullptr;
            if (!actor) {
                if (ptr && (ptr->GetFormID() & 0xFF000000u) == 0xFF000000u && !ptr->IsDeleted()) {
                    ptr->Disable();
                    ptr->SetDelete(true);
                }
                return {};
            }
            const auto actorId = actor->GetFormID();
            bool registered = false;
            {
                std::scoped_lock lk(g_mutex);
                // 결정 170 U-3: 첫 교전 전에 회차 목록과 액터별 난이도 배율을 함께 등록한다.
                if (g_activeMap.active && g_activeMap.runId == runId &&
                    HasMapSpawnCapacityLocked()) {
                    g_activeMap.spawnedRefFormIds.push_back(actorId);
                    if (isBoss) {
                        g_activeMap.bossRefFormId = actorId;
                    }
                    const auto multipliers = Core::MapSystem::GetMapMonsterMultipliers(
                        Core::MapSystem::RewardMonsterLevel(g_activeMap), *g_dataPack);
                    std::scoped_lock actorLock(g_mapActorsMutex);
                    g_mapActors.Register(actorId, { runId, multipliers.lifeMultiplier, multipliers.damageMultiplier });
                    if (isBoss) {
                        g_activeBossFormId.store(actorId, std::memory_order_release);
                    }
                    registered = true;
                }
            }
            if (!registered) {
                const RE::FormID spawnedTemplate[] = { npcBase->GetFormID() };
                (void)DisableMapSpawnOnMainThread(actorId, runId, cell->GetFormID(), spawnedTemplate);
                return {};
            }
            actor->StartCombat(player);
            return RE::NiPointer<RE::Actor>{ actor };
        }

        void SpawnHarmlessVfxAtActor(RE::Actor* target, RE::FormID vfxLocalId)
        {
            if (!target || vfxLocalId == 0) {
                return;
            }
            auto* expl = FormIds::Lookup<RE::BGSExplosion>(vfxLocalId);
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!expl || !dataHandler) {
                return;
            }
            (void)dataHandler->CreateReferenceAtLocation(
                expl,
                target->GetPosition(),
                RE::NiPoint3{},
                target->GetParentCell(),
                target->GetWorldspace(),
                nullptr,
                nullptr,
                RE::ObjectRefHandle{},
                false,
                true);
        }

        void ExecuteArena4PhaseTransitionOnMainThread(
            std::uint8_t newPhase, RE::FormID bossFormId, std::uint64_t runId)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || MapDeviceRuntime::IsUninstallCleanupArmed()) {
                return;
            }
            {
                std::scoped_lock lk(g_mutex);
                const auto& travel = g_travel.Current();
                if (!g_activeMap.active || !g_activeMap.isArena4 || g_activeMap.runId != runId ||
                    g_activeMap.bossRefFormId != bossFormId || g_activeMap.bossKilled || g_activeMap.failed ||
                    g_activeMap.endRequested || (travel && !Core::IsMapEntry(travel->kind))) {
                    return;
                }
            }
            const auto boss = LookupMapBoss(bossFormId, g_activeCellFormId.load(std::memory_order_acquire));
            if (!boss || boss->IsDead() || boss->IsDisabled()) {
                return;
            }
            {
                SpawnHarmlessVfxAtActor(boss.get(), FormIds::kVfxFireBurst);
                PromoteMapBoss(boss.get(), true, newPhase, {});
            }

            const auto* arena = MapPackRuntime::FindForms(&MapPackRuntime::Manifest().arena);
            constexpr std::size_t kReinforcementCount = 2;
            std::size_t addedCount = 0;
            if (arena && !arena->addNpcs.empty()) {
                for (std::size_t i = 0; i < kReinforcementCount && i < arena->packAnchors.size(); ++i) {
                    const RE::NiPoint3 off{ (i == 0 ? 40.0f : -40.0f), 30.0f, 0.0f };
                    if (SpawnMapNpcAtAnchor(player, arena->packAnchors[i], arena->addNpcs[i % arena->addNpcs.size()], off, runId)) {
                        ++addedCount;
                    }
                }
            }
            {
                std::scoped_lock lk(g_mutex);
                if (g_activeMap.active && g_activeMap.runId == runId && g_activeMap.bossRefFormId == bossFormId) {
                    g_completedArenaPhase = std::max(g_completedArenaPhase, newPhase);
                }
            }
            if (addedCount < kReinforcementCount) {
                SKSE::log::warn("MapDeviceRuntime: arena run {} phase {} spawned {}/{} reinforcements.",
                    runId, newPhase, addedCount, kReinforcementCount);
            }
            const auto msg = addedCount > 0
                ? std::format("[4차 전직 결투장] 보스 {}페이즈 돌입! 증원군 출현!", newPhase)
                : std::format("[4차 전직 결투장] 보스 {}페이즈 돌입!", newPhase);
            RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        }

        // 결정 178 ⑦ 보완(2026-11-23 사용자 확인 "보완해서 고침"): 붙지 않은 셀이라 끄지 않고 셀 초기화에 맡긴 스폰이 초기화 뒤에도
        // 살아 남는 일이 있었다(실행 15, 보스 하나, 원인 미확정). 그 셀이 다시 붙으면(플레이어가 들어와 셀이 안정되었을 때와 새 회차를
        // 스폰한 직후) 지금 회차에 등록되지 않은 스폰 원형 액터를 찾아, 셀이 붙은 상태에서만 끄고 지연 삭제에 넘긴다.
        // 기억해 둔 목록이 아니라 셀을 직접 훑으므로 저장·불러오기 뒤에도 같은 기준으로 찾는다.
        // 셀을 실제로 훑었으면 true(남은 스폰이 없었어도). 훑지 못했으면 false라 다음 틱에 다시 시도한다.
        bool SweepLeftoverSpawnsInCell(RE::FormID cellId, std::string_view when)
        {
            if (cellId == 0 || MapDeviceRuntime::IsUninstallCleanupActive()) {
                return false;
            }
            auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(cellId);
            auto templateIds = SpawnTemplateIdsForCell(cellId);
            if (!cell || templateIds.empty() || !cell->IsAttached()) {
                return false;
            }
            std::vector<RE::FormID> current;
            std::vector<RE::FormID> pendingDeletes;
            std::uint64_t activeRunId = 0;
            RE::FormID bossId = 0;
            bool bossUnresolved = false;
            bool bossSettled = true;
            {
                std::scoped_lock lk(g_mutex);
                // 이동이 걸렸거나 사망 귀환 중이면 미룬다(입장·스폰 두 경로 공통). Disable은 작업 풀 예약이라 셀이 떨어진 뒤 실행될 수 있다.
                if (PendingTravelKindLocked() || g_deathEscapeRunId.load(std::memory_order_acquire) != 0) {
                    return false;
                }
                // 다른 회차 번호로 이미 끄기를 요청해 삭제를 기다리는 액터는 다시 끄지 않는다(Disable 실행 전이면 아직 활성으로 보인다).
                for (const auto& entry : g_pendingActorDeletes) {
                    pendingDeletes.push_back(entry.actorId);
                }
                if (g_activeMap.active && g_activeCellFormId.load(std::memory_order_relaxed) == cellId) {
                    current = Core::MapSystem::MapSpawnCleanupIds(g_activeMap);
                    activeRunId = g_activeMap.runId;
                    bossId = g_activeMap.bossRefFormId;
                    bossUnresolved = g_activeMap.unresolvedBossRefFormId != 0;
                    bossSettled = g_activeMap.bossKilled || g_activeMap.spawnsDisabled || !g_spawnedForCurrentRun;
                }
            }
            bool bossDeferred = false;
            if (activeRunId != 0 && !bossSettled) {
                // 지금 회차의 보스를 찾지 못하는 동안(불러온 뒤 참조가 풀리지 않음 등)에는 보스 분실 복구
                // (RecoverMissingBossOnMainThread)가 셀에서 같은 원형의 액터를 찾아 다시 보스로 묶는다. 그 후보를 여기서 끄지 않는다.
                // 보스가 있는지는 분실 감시와 같은 기준(LookupMapBoss: 원형·셀 일치)으로 본다.
                if (bossUnresolved || !LookupMapBoss(bossId, cellId)) {
                    const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::FindDungeonByCell(cellId));
                    if (forms && forms->bossNpc) {
                        std::erase(templateIds, forms->bossNpc->GetFormID());
                        bossDeferred = true;
                    }
                }
            }
            std::vector<RE::NiPointer<RE::Actor>> leftovers;
            cell->ForEachReference([&](RE::TESObjectREFR* reference) {
                auto* actor = reference ? reference->As<RE::Actor>() : nullptr;
                const auto* base = actor ? actor->GetBaseObject() : nullptr;
                const auto* parent = actor ? actor->GetParentCell() : nullptr;
                const Core::MapLeftoverObservation observed{
                    .isActor = actor != nullptr,
                    .isPlayer = actor && actor->IsPlayerRef(),
                    .isFollowerOrCommanded = actor && (actor->IsPlayerTeammate() || actor->IsCommandedActor()),
                    .isDeleted = actor && actor->IsDeleted(),
                    .isDisabled = actor && actor->IsDisabled(),
                    .formId = actor ? actor->GetFormID() : 0u,
                    .actorBaseId = base ? base->GetFormID() : 0u,
                    .cellId = parent ? parent->GetFormID() : 0u
                };
                if (Core::MapSystem::IsLeftoverMapSpawn(cellId, true, templateIds, current, observed) &&
                    std::ranges::find(pendingDeletes, observed.formId) == pendingDeletes.end()) {
                    leftovers.emplace_back(actor);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (leftovers.empty()) {
                // 보스 원형을 미뤘으면 보스가 다시 묶인 뒤 남은 같은 원형 액터를 찾도록 이 입장을 끝난 것으로 기록하지 않는다.
                return !bossDeferred;
            }
            std::vector<RE::FormID> ids;
            ids.reserve(leftovers.size());
            for (const auto& actor : leftovers) {
                const auto pos = actor->GetPosition();
                SKSE::log::warn("MapDeviceRuntime: found leftover map actor 0x{:08X} (base 0x{:08X}) in attached cell 0x{:08X} on {} (active run {}) at {:.0f},{:.0f},{:.0f}: {}.",
                    actor->GetFormID(), actor->GetBaseObject()->GetFormID(), cellId, when, activeRunId, pos.x, pos.y, pos.z,
                    DescribeSpawnEngineState(actor.get()));
                ids.push_back(actor->GetFormID());
            }
            // 기여 기록만 지운다. 바닥 드랍은 지우지 않는다: DiscardDropsFromCorpses는 그 셀에 있는 추적 드랍을 모두 지워
            // 지금 회차의 드랍까지 없앤다(Codex 검토). 남은 액터의 회차 드랍은 그 회차를 닫을 때 이미 지웠다.
            DeathListener::ForgetKillRecords(ids);
            DisableSpawnTally tally;
            std::vector<RE::FormID> requested;
            for (const auto id : ids) {
                // 회차 번호 0: 어느 회차에서 남았는지 알 수 없는 정리. 삭제 대기는 원형과 셀로 다시 확인한다.
                const auto result = DisableMapSpawnOnMainThread(id, 0, cellId, templateIds);
                tally.Add(result, id);
                if (result == DisableSpawnResult::Requested) {
                    requested.push_back(id);
                }
            }
            {
                std::scoped_lock lk(g_mutex);
                for (const auto id : requested) {
                    if (std::ranges::find(g_sweepDisableRequested, id) == g_sweepDisableRequested.end()) {
                        g_sweepDisableRequested.push_back(id);
                    }
                }
            }
            g_leftoverSweepCleaned.fetch_add(requested.size(), std::memory_order_relaxed);
            SKSE::log::warn("MapDeviceRuntime: swept {} leftover map actor(s) in cell 0x{:08X} on {} (disable requested {}): {}.",
                ids.size(), cellId, when, requested.size(), tally.Describe());
            return !bossDeferred;
        }

        void SpawnMapEncounterOnMainThread(RE::PlayerCharacter* player)
        {
            Core::ActiveMapState snap;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_activeMap.endRequested || g_spawnedForCurrentRun ||
                    !g_playerInActiveMapCell.load(std::memory_order_relaxed)) {
                    return;
                }
                snap = g_activeMap;
            }

            const auto* dungeon = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(snap));
            bool packSpawned = false;
            auto boss = dungeon ? SpawnMapNpcAtAnchor(player, dungeon->bossAnchor, dungeon->bossNpc, {}, snap.runId, true)
                                : RE::NiPointer<RE::Actor>{};
            if (boss && snap.isArena4) {
                const std::uint16_t p1Mask =
                    static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Armoured));
                MonsterRuntime::ForcePromoteForTest(boss.get(), Core::MonsterRarity::Rare, p1Mask, -1.0);
                for (std::size_t i = 0; i < 2 && i < dungeon->packAnchors.size(); ++i) {
                    auto* npc = dungeon->addNpcs[i % dungeon->addNpcs.size()];
                    if (SpawnMapNpcAtAnchor(player, dungeon->packAnchors[i], npc, {}, snap.runId)) {
                        packSpawned = true;
                    }
                }
            } else if (boss) {
                std::uint16_t bossMods =
                    static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Armoured));
                if (snap.summary.forceHasted) {
                    bossMods |= static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted));
                }
                if (snap.summary.forceDeadly) {
                    bossMods |= static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Deadly));
                }
                MonsterRuntime::ForcePromoteForTest(boss.get(), Core::MonsterRarity::Rare, bossMods, -1.0);

                const auto basePacks = g_config ? g_config->mapBasePackCount : 3u;
                const auto actorCount = Core::MapSystem::MapPackActorCount(basePacks, snap.summary.packSizePct);
                constexpr std::array<RE::NiPoint3, 3> kPackOffsets = {{
                    { 0.0f, 0.0f, 0.0f },
                    { 85.0f, 45.0f, 0.0f },
                    { -85.0f, -45.0f, 0.0f }
                }};
                const std::size_t anchorCount = dungeon->packAnchors.size();
                for (std::size_t i = 0; i < actorCount; ++i) {
                    const auto packIndex = i / kPackOffsets.size();
                    auto* anchor = dungeon->packAnchors[packIndex % anchorCount];
                    RE::NiPoint3 offset = kPackOffsets[i % kPackOffsets.size()];
                    offset.x += static_cast<float>((packIndex / anchorCount) * 110);
                    auto* npc = dungeon->packNpcs[i % dungeon->packNpcs.size()];
                    if (SpawnMapNpcAtAnchor(player, anchor, npc, offset, snap.runId)) {
                        packSpawned = true;
                    }
                }
            }

            if (!boss || !packSpawned) {
                bool cancelled = false;
                bool refunded = false;
                SaveLoad::RunSerializedTransaction([&]() {
                    std::optional<Core::PoEItemRecord> tracked;
                    {
                        std::scoped_lock lk(g_mutex);
                        if (!g_activeMap.active || g_activeMap.runId != snap.runId) {
                            return;
                        }
                        snap = std::move(g_activeMap);
                        tracked = std::move(g_consumedMapRecord);
                        SetActiveMapLocked({});
                        g_spawnedForCurrentRun = false;
                        g_pendingSpawnRunId = 0;
                        g_arenaPhaseInvulnSec = 0.0;
                        g_permittedCell = 0;
                        g_retryReturnRunId = 0;
                        g_deathEscapeExitElapsedSec = 0.0;
                        g_deathEscapeRunId.store(0, std::memory_order_release);
                        RefreshAtomicsLocked();
                    }
                    refunded = RefundEntryOnMainThread(player, snap.tier, snap.isArena4, tracked);
                    cancelled = true;
                });
                if (cancelled) {
                    FinishCancelledRunOnMainThread(player, snap, true, refunded,
                        boss ? "무리 생성에 실패해 회차를 취소했습니다." : "보스 생성에 실패해 회차를 취소했습니다.",
                        dungeon ? dungeon->cellId : 0);
                    MapDeviceRuntime::SyncMapLayer2ToStatEngine();
                }
                return;
            }

            std::size_t spawnedCount = 0;
            RE::FormID bossId = 0;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_activeMap.runId != snap.runId) {
                    return;
                }
                bossId = g_activeMap.bossRefFormId;
                spawnedCount = g_activeMap.spawnedRefFormIds.size();
                g_spawnedForCurrentRun = true;
                g_pendingSpawnRunId = 0;
            }

            SKSE::log::info("MapDeviceRuntime: spawned {} actor(s) (boss=0x{:08X}, arena4={}, T{}, rewardLv={}).",
                spawnedCount, bossId, snap.isArena4, snap.tier, Core::MapSystem::RewardMonsterLevel(snap));
            (void)SweepLeftoverSpawnsInCell(dungeon ? dungeon->cellId : 0, "spawn");
        }

        void EndRunOnMainThread()
        {
            std::vector<std::uint32_t> toClean;
            std::vector<RE::FormID> disableRequested;
            RE::FormID cellId = 0;
            std::uint64_t runId = 0;
            {
                std::scoped_lock lk(g_mutex);
                cellId = g_activeCellFormId.load(std::memory_order_relaxed);
                runId = g_activeMap.runId;
                toClean = Core::MapSystem::MapSpawnCleanupIds(g_activeMap);
                disableRequested = std::move(g_runSpawnDisableRequests);
                SetActiveMapLocked({});
                g_spawnedForCurrentRun = false;
                g_pendingSpawnRunId = 0;
                g_arenaPhaseInvulnSec = 0.0;
                g_permittedCell = 0;
                g_retryReturnRunId = 0;
                RefreshAtomicsLocked();
            }
            CleanupClosedRunOnMainThread(toClean, runId, cellId, disableRequested);
            MapDeviceRuntime::SyncMapLayer2ToStatEngine();
            RE::SendHUDMessage::ShowHUDMessage("지도 회차를 종료했습니다.");
        }

        Core::MapBossLossObservation BossLossObservationLocked(bool stableRunCell, bool gamePaused)
        {
            return {
                .stableRunCell = stableRunCell,
                .playerAuthorized = g_playerInActiveMapCell.load(std::memory_order_relaxed),
                .spawned = g_spawnedForCurrentRun,
                .travelPending = PendingTravelKindLocked().has_value() ||
                    g_deathEscapeRunId.load(std::memory_order_relaxed) == g_activeMap.runId,
                .deathConfirmed = g_bossDeathConfirmedRunId == g_activeMap.runId,
                .deathPending = g_bossDeathPendingRunId == g_activeMap.runId,
                .paused = gamePaused
            };
        }

        void RecoverMissingBossOnMainThread(RE::PlayerCharacter* player, RE::FormID cellId, RE::FormID previousBossId,
            std::uint64_t runId)
        {
            const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::FindDungeonByCell(cellId));
            if (!forms || !forms->cell || !forms->bossNpc || player->GetParentCell() != forms->cell) {
                return;
            }

            std::vector<RE::NiPointer<RE::TESObjectREFR>> references;
            forms->cell->ForEachReference([&](RE::TESObjectREFR* reference) {
                references.emplace_back(reference);
                return RE::BSContainer::ForEachResult::kContinue;
            });
            RE::FormID livingBoss = 0;
            std::vector<RE::FormID> deadBosses;
            for (const auto& reference : references) {
                auto* actor = reference->As<RE::Actor>();
                if (!actor || actor->GetBaseObject() != forms->bossNpc || actor->GetParentCell() != forms->cell ||
                    actor->IsDeleted() || actor->IsDisabled()) {
                    continue;
                }
                // 지도 팩에는 배치된 NPC가 없고 런타임 스폰만 회차 참조로 기록한다.
                const auto actorId = actor->GetFormID();
                if ((actorId & kDynamicFormMask) != kDynamicFormMask) {
                    continue;
                }
                if (!actor->IsDead()) {
                    livingBoss = actorId;
                    break;
                }
                deadBosses.push_back(actorId);
            }

            Core::ActiveMapState cancelledRun;
            std::optional<bool> refunded;
            RE::FormID foundBoss = 0;
            std::uint8_t pendingArenaPhase = 0;
            std::uint8_t completedArenaPhase = 0;
            bool isArena4 = false;
            Core::MapModSummary summary{};
            SaveLoad::RunSerializedTransaction([&]() {
                std::optional<Core::PoEItemRecord> tracked;
                {
                    std::scoped_lock lk(g_mutex);
                    if (g_activeMap.runId != runId || g_activeMap.bossRefFormId != previousBossId ||
                        !Core::MapSystem::CanCheckBossLoss(g_activeMap,
                            BossLossObservationLocked(cellId == g_activeCellFormId.load(std::memory_order_relaxed), false))) {
                        return;
                    }
                    foundBoss = Core::MapSystem::SelectMapBossCandidate(livingBoss, deadBosses, g_activeMap.spawnedRefFormIds);
                    if (foundBoss != 0) {
                        Core::MapSystem::RebindMapBoss(g_activeMap, foundBoss);
                        g_activeMap.bossKilled = livingBoss == 0;
                        isArena4 = g_activeMap.isArena4;
                        completedArenaPhase = g_completedArenaPhase;
                        summary = g_activeMap.summary;
                        if (livingBoss != 0 && isArena4 && g_activeMap.arenaPhase > completedArenaPhase) {
                            pendingArenaPhase = g_activeMap.arenaPhase;
                        }
                        g_bossMissingSec = 0.0;
                        g_bossDeathPendingRunId = 0;
                        g_bossDeathConfirmedRunId = 0;
                        g_bossDeathStart.reset();
                        RefreshMapActorsLocked();
                        RefreshAtomicsLocked();
                        return;
                    }
                    cancelledRun = std::move(g_activeMap);
                    tracked = std::move(g_consumedMapRecord);
                    SetActiveMapLocked({});
                    g_spawnedForCurrentRun = false;
                    g_pendingSpawnRunId = 0;
                    g_arenaPhaseInvulnSec = 0.0;
                    g_permittedCell = 0;
                    g_retryReturnRunId = 0;
                    RefreshAtomicsLocked();
                }
                if (!tracked) {
                    tracked = RecreateSavedMapRefund(cancelledRun);
                }
                refunded = RefundEntryOnMainThread(player, cancelledRun.tier, cancelledRun.isArena4, tracked);
            });

            if (foundBoss != 0) {
                SKSE::log::info("MapDeviceRuntime: rebound run {} boss 0x{:08X} -> 0x{:08X} (dead={}).",
                    runId, previousBossId, foundBoss, livingBoss == 0);
                if (livingBoss == 0) {
                    RE::SendHUDMessage::ShowHUDMessage("[지도 장치] 저장된 보스 처치를 반영했습니다. 보상 없음.");
                } else {
                    const auto boss = LookupMapBoss(foundBoss, cellId);
                    PromoteMapBoss(boss.get(), isArena4, completedArenaPhase, summary);
                    if (pendingArenaPhase != 0) {
                        ExecuteArena4PhaseTransitionOnMainThread(pendingArenaPhase, foundBoss, runId);
                    }
                }
            } else if (refunded.has_value()) {
                FinishCancelledRunOnMainThread(player, cancelledRun, true, refunded,
                    "보스를 찾을 수 없어 회차를 취소했습니다.", cellId);
                MapDeviceRuntime::SyncMapLayer2ToStatEngine();
            }
        }

        Core::MapEntryAccess GetEntryAccessLocked(const Core::MapTravelObservation& observation)
        {
            return Core::CheckMapEntryAccess(g_activeCellFormId.load(std::memory_order_relaxed),
                g_permittedCell, observation);
        }

        void PermitEntryLocked(RE::FormID cell)
        {
            g_permittedCell = cell;
            g_retryReturnRunId = 0;
            if (!g_spawnedForCurrentRun) {
                g_pendingSpawnRunId = g_activeMap.runId;
            }
            g_deathEscapeExitElapsedSec = 0.0;
            g_deathEscapeRunId.store(0, std::memory_order_release);
        }

        MapActionResult GetArena4EntryRejection()
        {
            const auto entry = Core::MapSystem::GetArena4EntryStatus(SaveLoad::GetStateCopy().tree.ascendancyMilestonesBitmask);
            if (entry.denial == Core::Arena4EntryDenial::AlreadyCleared) {
                return { false, "이미 4차 전직 결투장을 정복했습니다.", MapEntryDenial::ArenaAlreadyCleared };
            }
            if (entry.denial == Core::Arena4EntryDenial::MissingMilestones) {
                return { false,
                    std::format("1~3차 전직 이정표 {}/3 · 모두 달성하면 입장할 수 있습니다.", entry.milestonesCompleted),
                    MapEntryDenial::ArenaMissingMilestones };
            }
            return {};
        }

        MapActionResult CompleteEntry(RE::PlayerCharacter* player, const Core::MapTravelRequest& request, const TravelContext& context)
        {
            const auto kind = request.kind;
            const auto& origin = context.entryOrigin;
            std::string message;
            if (kind == MapTravelKind::Reenter) {
                std::scoped_lock lk(g_mutex);
                if (g_activeMap.runId != context.runId || !Core::MapSystem::TryConsumePortalForReenter(g_activeMap)) {
                    return { false, "재입장 포털을 소비하지 못했습니다." };
                }
                g_activeMap.returnCellFormId = origin.cell;
                g_activeMap.returnWorldspaceFormId = origin.worldspace;
                g_activeMap.returnPos = origin.position;
                g_activeMap.returnAngle = origin.angle;
                PermitEntryLocked(request.target.cell);
                message = std::format("지도 재입장 완료 (남은 포털: {}/{})",
                    g_activeMap.portalsRemaining, Core::kNormalMapMaxPortals);
            } else {
                const bool arena = kind == MapTravelKind::OpenArena;
                const auto& pack = MapPackRuntime::Manifest();
                const auto tier = FormIds::MapTierFromLocalFormId(FormIds::ResolveLocalFormIdIfOurs(context.item));
                const auto* dungeon = arena ? &pack.arena : (tier ? pack.ForTier(*tier) : nullptr);
                auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(context.item);
                if (!dungeon || !item) {
                    return { false, "입장 비용을 소비하지 못했습니다." };
                }

                bool committed = false;
                SaveLoad::RunSerializedTransaction([&]() {
                    if (arena) {
                        auto rejection = GetArena4EntryRejection();
                        if (rejection.entryDenial != MapEntryDenial::None) {
                            message = std::move(rejection.message);
                            return;
                        }
                    }
                    const auto beforeCount = player->GetItemCount(item);
                    if (beforeCount <= 0) {
                        return;
                    }
                    std::optional<Core::PoEItemRecord> tracked;
                    if (context.mapKey.IsValid()) {
                        tracked = ItemTracker::FindByKey(context.mapKey);
                        auto* extra = tracked ? ItemTracker::FindExtraListByKey(player, item, context.mapKey) : nullptr;
                        if (!extra) {
                            return;
                        }
                        (void)player->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kRemove, extra, nullptr);
                    } else if (!UniqueIdUtil::RemovePlainItem(player, item, 1)) {
                        if (!arena || MapDeviceRuntime::CountPlayerTributes(player) <= 0) {
                            return;
                        }
                        // 공물은 추적 제작 아이템이 아니므로 기존 공물 소비 경로도 허용한다.
                        (void)player->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    }
                    // RemoveItem의 반환은 제거 수량이 아니므로 가방 수량으로 소비를 확인한다.
                    if (player->GetItemCount(item) != beforeCount - 1) {
                        return;
                    }
                    if (tracked && !ItemTracker::FindExtraListByKey(player, item, context.mapKey)) {
                        ItemTracker::RemoveByKey(context.mapKey);
                    }

                    std::scoped_lock lk(g_mutex);
                    const auto runId = g_nextRunId;
                    g_nextRunId = Core::NextMapRunId(runId);
                    auto next = arena
                        ? Core::MapSystem::CreateArena4Run(runId, origin.cell, origin.worldspace, origin.position, origin.angle)
                        : Core::MapSystem::CreateNormalMapRun(runId, *tier,
                            tracked ? tracked->affixes : std::vector<Core::RolledAffix>{},
                            origin.cell, origin.worldspace, origin.position, origin.angle);
                    next.dungeonKey = dungeon->key;
                    next.packVersion = pack.version;
                    message = arena
                        ? std::format("4차 전직 결투장 {} 입장 완료", dungeon->name)
                        : std::format("T{} {} 입장 완료 (Lv.{}, 남은 포털 {}/{})", next.tier, dungeon->name,
                            Core::MapSystem::RewardMonsterLevel(next), next.portalsRemaining, Core::kNormalMapMaxPortals);
                    SetActiveMapLocked(std::move(next), std::move(tracked));
                    g_spawnedForCurrentRun = false;
                    g_arenaPhaseInvulnSec = 0.0;
                    PermitEntryLocked(request.target.cell);
                    RefreshAtomicsLocked();
                    committed = true;
                });
                if (!committed) {
                    return { false, message.empty() ? "입장 비용을 소비하지 못했습니다." : std::move(message) };
                }
            }

            return { true, std::move(message) };
        }

        void CompleteReturn(MapTravelKind kind, std::uint64_t runId)
        {
            if (kind == MapTravelKind::SafeEscape || kind == MapTravelKind::CancelledRun) {
                RE::SendHUDMessage::ShowHUDMessage(kind == MapTravelKind::SafeEscape ? "안전 탈출을 완료했습니다." : "귀환을 완료했습니다.");
                return;
            }
            bool endRun = false;
            std::uint8_t portals = 0;
            bool arena = false;
            bool defer = false;
            const auto bossId = kind == MapTravelKind::Return ? g_activeBossFormId.load(std::memory_order_acquire) : 0u;
            const bool bossDeadNow = IsBossDeadNow(bossId);
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_activeMap.runId != runId) {
                    if (g_pendingReturnConfirmation && g_pendingReturnConfirmation->runId == runId) {
                        g_pendingReturnConfirmation.reset();
                    }
                    return;
                }
                defer = g_queue && !MapDeviceRuntime::IsUninstallCleanupArmed() &&
                    ShouldDeferBossDeathLocked(bossDeadNow && bossId == g_activeMap.bossRefFormId);
                if (defer) {
                    g_pendingReturnConfirmation = PendingReturnConfirmation{ runId, kind };
                } else {
                    g_retryReturnRunId = 0;
                    endRun = kind == MapTravelKind::Abandon || Core::MapSystem::ShouldEndOnReturn(g_activeMap);
                    portals = g_activeMap.portalsRemaining;
                    arena = g_activeMap.isArena4;
                    if (!endRun) {
                        g_pendingReturnConfirmation.reset();
                    }
                }
            }
            if (defer) {
                // 귀환 요청 뒤에 확인된 보스 사망도 다음 프레임의 회차 처리까지 기다린다.
                g_queue->Enqueue([kind, runId]() { CompleteReturn(kind, runId); });
            } else if (endRun) {
                EndRunOnMainThread();
            } else {
                const auto message = std::format("귀환 완료 (남은 포털: {}/{})",
                    portals, arena ? Core::kArena4MaxPortals : Core::kNormalMapMaxPortals);
                RE::SendHUDMessage::ShowHUDMessage(message.c_str());
            }
        }

        void ReportTravelFailure(MapTravelKind kind, std::uint64_t runId)
        {
            if (!Core::IsMapEntry(kind)) {
                // 귀환이 끝내 실패하면 지도 안에서 무적으로 남지 않게 푼다(사망 귀환이 합류한 일반 귀환·포기 포함).
                // 다음 치명타는 다시 귀환을 요청한다.
                std::scoped_lock lk(g_mutex);
                if (Core::MapSystem::ReleaseDeathEscape(g_deathEscapeRunId, runId)) {
                    g_deathEscapeExitElapsedSec = 0.0;
                }
            }
            if (kind == MapTravelKind::Return || kind == MapTravelKind::DeathEscape) {
                std::scoped_lock lk(g_mutex);
                if (g_activeMap.active && g_activeMap.runId == runId) {
                    g_retryReturnRunId = runId;
                }
            }
            RE::SendHUDMessage::ShowHUDMessage(Core::IsMapEntry(kind)
                ? "입구 도착을 확인하지 못해 입장 요청을 취소했습니다. 다시 시도하세요."
                : (kind == MapTravelKind::SafeEscape || kind == MapTravelKind::CancelledRun)
                    ? "귀환 위치에 도착하지 못했습니다. 지도 탭의 안전 탈출로 다시 시도하세요."
                    : "귀환 위치에 도착하지 못했습니다. 다시 시도하세요.");
        }

        bool AdvancePendingTravelDisable(RE::PlayerCharacter* player, const Core::MapTravelObservation& observation,
            double deltaSec, bool gamePaused)
        {
            std::uint64_t runId = 0;
            std::uint64_t requestFrame = 0;
            std::vector<RE::FormID> actorIds;
            bool releaseBossWait = false;
            bool disableOnRelease = false;
            double bossWaitSec = 0.0;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_pendingTravelDisable) {
                    return false;
                }
                requestFrame = g_pendingTravelDisable->requestFrame;
                // 큐는 Tick보다 먼저 돈다. 그 큐에서 요청했어도 엔진에 최소 한 프레임을 준다.
                if (gamePaused || observation.loading || requestFrame == MainUpdateHook::TickCount()) {
                    return true;
                }
                runId = g_travelContext.runId;
                if (g_pendingTravelDisable->awaitingBossDeath) {
                    // 결정 179 ⑦: 사망이 시작된 보스의 확정과 처치 반영(큐)이 끝나거나 상한이 지나야 떠난다.
                    g_pendingTravelDisable->bossWaitSec += deltaSec;
                    const bool currentRun = g_activeMap.active && g_activeMap.runId == runId;
                    const bool pending = currentRun && BossKillProcessingPendingLocked(runId);
                    if (pending && Core::MapSystem::ShouldHoldEndingDisableForBossDeath(
                            true, g_activeMap.bossKilled, g_pendingTravelDisable->bossWaitSec)) {
                        return true;
                    }
                    bossWaitSec = g_pendingTravelDisable->bossWaitSec;
                    if (pending && !g_activeMap.bossKilled && g_bossDeathStart && g_bossDeathStart->runId == runId &&
                        g_bossDeathConfirmedRunId != runId) {
                        // 상한까지 확정이 오지 않았다. 기록을 지워 다음 귀환이 다시 기다리지 않게 하고,
                        // 늦게 온 사망 확정은 그때의 상태로 판정한다. 이미 확정됐으면 큐의 처치 반영이 이 기록을 쓰므로 둔다.
                        g_bossDeathStart.reset();
                    }
                    // 처치가 반영됐으면 결정 175대로 이 귀환이 회차를 끝내므로 그때 비활성화 여부를 다시 정한다.
                    disableOnRelease = currentRun && Core::MapSystem::ShouldEndOnReturn(g_activeMap);
                    releaseBossWait = true;
                } else {
                    actorIds = g_pendingTravelDisable->actorIds;
                }
            }
            if (releaseBossWait) {
                if (bossWaitSec >= Core::kBossDeathProcessWaitSec) {
                    SKSE::log::warn("MapDeviceRuntime: return run {} stopped waiting for the dying boss after {:.3f}s.",
                        runId, bossWaitSec);
                }
                // 비활성화하지 않는 귀환은 빈 목록으로 다음 프레임에 바로 이동한다.
                auto disabledIds = disableOnRelease ? DisableRunSpawnsOnMainThread(runId) : std::vector<RE::FormID>{};
                std::scoped_lock lk(g_mutex);
                if (!g_pendingTravelDisable || g_pendingTravelDisable->requestFrame != requestFrame ||
                    g_travelContext.runId != runId) {
                    return true;
                }
                g_pendingTravelDisable->awaitingBossDeath = false;
                g_pendingTravelDisable->bossHoldDone = true;
                // 이동 직전 재확인으로 다시 기다린 경우 앞서 요청한 액터도 계속 확인한다.
                auto& waitIds = g_pendingTravelDisable->actorIds;
                for (const auto id : disabledIds) {
                    if (std::ranges::find(waitIds, id) == waitIds.end()) {
                        waitIds.push_back(id);
                    }
                }
                g_pendingTravelDisable->requestFrame = MainUpdateHook::TickCount();
                g_pendingTravelDisable->elapsedSec = 0.0;
                SKSE::log::info("MapDeviceRuntime: return run {} resumed after waiting {:.3f}s for the dying boss (disable={}, bossKilled={}).",
                    runId, bossWaitSec, disableOnRelease, g_activeMap.bossKilled);
                return true;
            }

            std::vector<RE::FormID> disabled;
            std::vector<RE::FormID> remaining;
            for (const auto id : actorIds) {
                const RE::NiPointer<RE::Actor> actor{ RE::TESForm::LookupByID<RE::Actor>(id) };
                if (!actor || actor->IsDeleted()) {
                    continue;
                }
                (actor->IsDisabled() ? disabled : remaining).push_back(id);
            }

            Core::MapTravelTarget target;
            MapTravelKind kind;
            Core::MapSpawnDisableWaitResult wait;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_pendingTravelDisable || g_pendingTravelDisable->requestFrame != requestFrame ||
                    g_travelContext.runId != runId) {
                    return true;
                }
                for (auto& pending : g_pendingActorDeletes) {
                    if (pending.runId == runId && !pending.firstDisabledAgeSec &&
                        std::ranges::find(disabled, pending.actorId) != disabled.end()) {
                        pending.firstDisabledAgeSec = pending.ageSec;
                    }
                }
                wait = Core::AdvanceMapSpawnDisableWait(g_pendingTravelDisable->elapsedSec, deltaSec, remaining.size());
                if (wait.outcome == Core::MapSpawnDisableWaitOutcome::Waiting) {
                    g_pendingTravelDisable->elapsedSec = wait.elapsedSec;
                    return true;
                }
                // 결정 179 ⑦: 귀환을 접수한 뒤 이동하기 전에 보스의 사망이 시작됐으면, 처치 반영을 기다린 뒤 다시 확인한다.
                if (!g_pendingTravelDisable->bossHoldDone && g_activeMap.active && g_activeMap.runId == runId &&
                    Core::MapSystem::ShouldHoldEndingDisableForBossDeath(
                        BossKillProcessingPendingLocked(runId), g_activeMap.bossKilled, 0.0)) {
                    g_pendingTravelDisable->awaitingBossDeath = true;
                    g_pendingTravelDisable->bossWaitSec = 0.0;
                    SKSE::log::info("MapDeviceRuntime: return run {} found a boss death started before moving; waiting for it.", runId);
                    return true;
                }
                target = g_travel.Current()->target;
                kind = g_travel.Current()->kind;
                g_pendingTravelDisable.reset();
            }
            if (wait.outcome == Core::MapSpawnDisableWaitOutcome::TimedOut) {
                std::string ids;
                for (const auto id : remaining) {
                    ids += std::format(" 0x{:08X}", id);
                }
                SKSE::log::info("MapDeviceRuntime: ending return run {} Disable wait timed out with {} actor(s) remaining:{}.",
                    runId, remaining.size(), ids);
            }
            SKSE::log::info("MapDeviceRuntime: ending return run {} issuing move after {:.3f}s (kDisabled={}, absentOrDeleted={}, remaining={}, requestFrame={}, moveFrame={}).",
                runId, wait.elapsedSec, disabled.size(), actorIds.size() - disabled.size() - remaining.size(), remaining.size(),
                requestFrame, MainUpdateHook::TickCount());
            if (!IssueTravelMove(player, target)) {
                SKSE::log::warn("MapDeviceRuntime: could not issue ending return move for run {}.", runId);
                {
                    std::scoped_lock lk(g_mutex);
                    ClearTravelLocked();
                }
                ReportTravelFailure(kind, runId);
            }
            return true;
        }

        void AdvanceTravel(RE::PlayerCharacter* player, const Core::MapTravelObservation& observation,
            double deltaSec, bool gamePaused)
        {
            if (AdvancePendingTravelDisable(player, observation, deltaSec, gamePaused)) {
                return;
            }
            std::optional<Core::MapTravelRequest> request;
            TravelContext context;
            Core::MapTravelOutcome outcome;
            {
                std::scoped_lock lk(g_mutex);
                request = g_travel.Current();
                if (!request) {
                    return;
                }
                outcome = g_travel.Tick(observation, deltaSec, gamePaused);
                if (outcome == Core::MapTravelOutcome::Waiting) {
                    return;
                }
                context = g_travelContext;
                if (outcome == Core::MapTravelOutcome::Retry || outcome == Core::MapTravelOutcome::Fallback) {
                    request = g_travel.Current();
                } else {
                    g_travelContext = {};
                }
            }

            if (outcome == Core::MapTravelOutcome::Arrived) {
                if (!Core::IsMapEntry(request->kind)) {
                    CompleteReturn(request->kind, context.runId);
                } else {
                    const auto result = CompleteEntry(player, *request, context);
                    ShowMapMessage(result);
                    if (!result.ok) {
                        ShowMapMessage(RequestReturn(player, MapTravelKind::CancelledRun, context.entryOrigin, 0, "귀환을 요청했습니다."));
                    }
                }
                return;
            }
            if (outcome != Core::MapTravelOutcome::Failed) {
                if (IssueTravelMove(player, request->target)) {
                    return;
                }
                SKSE::log::warn("MapDeviceRuntime: could not issue travel attempt {}.", request->attempts);
                if (outcome == Core::MapTravelOutcome::Retry) {
                    return;
                }
                std::scoped_lock lk(g_mutex);
                ClearTravelLocked();
            }
            ReportTravelFailure(request->kind, context.runId);
        }

        void ExecuteDeathEscapeOnMainThread(std::uint64_t runId)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            Core::MapTravelTarget saved;
            bool alreadyReturning = false;
            bool failedRun = false;
            bool arenaRun = false;
            {
                std::scoped_lock lk(g_mutex);
                const auto pending = PendingTravelKindLocked();
                if (g_deathEscapeRunId.load(std::memory_order_relaxed) != runId) {
                    return;
                }
                if (!g_activeMap.active || g_activeMap.runId != runId || !player ||
                    MapDeviceRuntime::IsUninstallCleanupArmed() || (pending && Core::IsMapEntry(*pending))) {
                    if (Core::MapSystem::ReleaseDeathEscape(g_deathEscapeRunId, runId)) {
                        g_deathEscapeExitElapsedSec = 0.0;
                    }
                    return;
                }
                // 훅에서 실패를 먼저 표시한다. 여기서는 이미 실패한 회차도 스폰을 정리하고 귀환시킨다.
                auto death = g_activeMap;
                if (!Core::MapSystem::ConsumePortalOnDeathEscape(death)) {
                    failedRun = true;
                    arenaRun = g_activeMap.isArena4;
                    if (!std::exchange(g_activeMap.failed, true)) {
                        RefreshMapActorsLocked();
                    }
                }
                g_activeMap.portalsRemaining = death.portalsRemaining;
                saved = SavedReturn(g_activeMap);
                alreadyReturning = pending.has_value();
                g_deathEscapeExitElapsedSec = 0.0;
            }

            AilmentRuntime::ClearPlayerAilments();
            if (auto* av = player->AsActorValueOwner()) {
                for (const auto avKind : { RE::ActorValue::kHealth, RE::ActorValue::kMagicka, RE::ActorValue::kStamina }) {
                    const float maxVal = std::max(10.0f, av->GetPermanentActorValue(avKind));
                    const float curVal = av->GetActorValue(avKind);
                    if (curVal < maxVal * 0.50f) {
                        av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, avKind, maxVal * 0.50f - curVal);
                    }
                }
            }

            if (failedRun) {
                // 이미 이동을 낸 귀환에 합류하면 도착 후 회차를 닫을 때 스폰을 정리한다.
                RE::SendHUDMessage::ShowHUDMessage(arenaRun
                    ? "[4차 전직 결투장] 치명타로 도전에 실패했습니다. 귀환합니다."
                    : "[지도 장치] 남은 포털이 없어 지도에 실패했습니다. 귀환합니다.");
            }
            // UI나 문에서 이미 요청한 귀환도 도착할 때 같은 회차 종료 규칙을 적용한다.
            if (!alreadyReturning) {
                const auto result = RequestReturn(player, MapTravelKind::DeathEscape, saved, runId,
                    "치명타를 막아 귀환을 요청했습니다.");
                if (!result.ok) {
                    // 요청이 서지 않으면 실패 처리기가 오지 않으므로 여기서 무적을 푼다.
                    std::scoped_lock lk(g_mutex);
                    if (Core::MapSystem::ReleaseDeathEscape(g_deathEscapeRunId, runId)) {
                        g_deathEscapeExitElapsedSec = 0.0;
                    }
                }
                ShowMapMessage(result);
            }
        }

        // 귀환 문과 출구 구역(결정 177 ③ 재보완)이 함께 쓰는 지도 떠나기. 허가된 회차 셀이면 포털 귀환, 허가가 없거나 안전 제거
        // 정리 상태면 안전 탈출. 이미 이동 중이면 아무것도 하지 않는다(nullopt).
        // fromExitZone: 위치로 판정한 출구 구역에서 온 요청이면 참. 직접 여는 귀환 문은 결정 182 거절을 받지 않는다.
        std::optional<MapActionResult> LeaveMapThroughExit(RE::PlayerCharacter* player, bool fromExitZone)
        {
            const auto status = MapDeviceRuntime::GetTravelStatus(player);
            if (status.kind) {
                return std::nullopt;
            }
            // 안전 제거 정리 뒤에도 갇히지 않도록 안전 탈출은 허용한다.
            if (status.unauthorized || MapDeviceRuntime::IsUninstallCleanupArmed()) {
                return MapDeviceRuntime::SafeEscape(player);
            }
            // 결정 182: 밀려 들어가도 보스 처치 전에 회차가 끝나지 않게 한다. 지도 탭의 귀환·포기는 그대로 끝낼 수 있다.
            if (fromExitZone) {
                if (auto refusal = MapDeviceRuntime::ExitZoneReturnRefusal()) {
                    return MapActionResult{ false, std::move(*refusal) };
                }
            }
            return MapDeviceRuntime::ReturnPortal(player);
        }

        // 직접 여는 귀환 문의 활성화를 우리 귀환으로 바꾼다. 출구 자동 통로 문은 ESP에서 꺼 두고 출구 구역으로 판정한다.
        class MapReturnDoorSink final : public RE::BSTEventSink<RE::TESActivateEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* event,
                RE::BSTEventSource<RE::TESActivateEvent>*) override
            {
                if (ModState::AllowsGameplay() && event && event->actionRef && event->actionRef->IsPlayerRef() &&
                    event->objectActivated && g_queue) {
                    const auto generation = g_queue->CurrentGeneration();
                    const auto doorId = event->objectActivated->GetFormID();
                    const auto cellId = MapPackRuntime::ReturnDoorCell(doorId);
                    if (cellId != 0) {
                        const auto runId = MapDeviceRuntime::GetActiveRunId();
                        SKSE::log::info("MapDeviceRuntime: return door 0x{:08X} activated by the player in cell 0x{:08X} (run {}).",
                            doorId, cellId, runId);
                        g_queue->EnqueueForGeneration(generation, [cellId, runId, doorId]() {
                            if (MapDeviceRuntime::GetActiveRunId() != runId) {
                                return;
                            }
                            auto* player = RE::PlayerCharacter::GetSingleton();
                            const auto observation = ObservePlayer(player);
                            if (observation.cell != cellId || observation.loading) {
                                SKSE::log::info("MapDeviceRuntime: door 0x{:08X} return skipped (playerCell=0x{:08X}, expected=0x{:08X}, loading={}).",
                                    doorId, observation.cell, cellId, observation.loading);
                                return;
                            }
                            if (const auto result = LeaveMapThroughExit(player, false)) {
                                ShowMapMessage(*result);
                            }
                        });
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        std::string EntryUnavailable(RE::PlayerCharacter* player, MapTravelKind kind)
        {
            if (const auto& reason = MapPackRuntime::UnavailableReason(); !reason.empty()) {
                return reason;
            }
            if (!player) {
                return "플레이어를 찾을 수 없습니다.";
            }
            if (MapDeviceRuntime::IsUninstallCleanupArmed()) {
                return "안전 제거 정리 완료 상태입니다. 세이브 후 종료하거나 다시 불러오세요.";
            }
            {
                std::scoped_lock lk(g_mutex);
                if (PendingTravelKindLocked()) {
                    return "이동 중입니다. 도착 후 다시 시도하세요.";
                }
                if (kind != MapTravelKind::Reenter && g_activeMap.active) {
                    return "진행 중인 회차가 있습니다. 보스를 처치하고 귀환하거나 포기한 뒤 여세요.";
                }
            }
            const auto* cell = player->GetParentCell();
            if (!cell) {
                return "현재 셀을 찾을 수 없습니다. 이동을 마치고 다시 시도하세요.";
            }
            if (MapPackRuntime::OwnsCell(cell)) {
                return "지도 팩 셀 안에서는 입장을 요청할 수 없습니다. 먼저 귀환하세요.";
            }
            if (player->IsInCombat()) {
                return "전투 중에는 입장을 요청할 수 없습니다.";
            }
            return {};
        }

        MapActionResult RequestEntry(RE::PlayerCharacter* player, MapTravelKind kind,
            const MapPackRuntime::DungeonForms& forms, TravelContext context, std::string message)
        {
            const auto target = EntryTarget(forms);
            if (!target) {
                return { false, "지도 입구 마커를 찾을 수 없습니다." };
            }
            if (kind != MapTravelKind::Reenter) {
                // 입장 직전에 같은 셀로 Reset을 보내면 VM 실행 시점에 따라 이번 회차의 재입장에 초기화가 걸릴 수 있다(U-5).
                // 그래서 여기서 보내지 않고, Tick이 먼저 보내도록 이번 요청을 거절한다. 전송이 계속 실패하는 셀만 그대로 연다.
                std::scoped_lock lk(g_mutex);
                if (const auto pending = g_pendingCellResets.find(forms.cellId); pending != g_pendingCellResets.end()) {
                    if (pending->second.waitedSec < kCellResetMaxEntryWaitSec) {
                        return { false, "던전 초기화를 기다리는 중입니다. 잠시 뒤 다시 시도하세요." };
                    }
                    SKSE::log::warn("MapDeviceRuntime: opening a run while Cell.Reset has not finished for 0x{:08X} after {:.0f}s.",
                        forms.cellId, pending->second.waitedSec);
                }
            }
            context.entryOrigin = CaptureLocation(*player);
            return StartTravel(player, kind, *target, context, std::move(message));
        }
    }

    void MapDeviceRuntime::Initialize(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        const Core::ItemData* itemData,
        const Core::PassiveTreeData* treeData,
        const Core::Config* config,
        Core::EnergyShieldPool* esPool,
        Core::CharmSystem* charmSystem,
        const Core::MapTravelSettings& travelSettings)
    {
        g_queue = queue;
        g_statEngine = statEngine;
        g_itemData = itemData;
        g_treeData = treeData;
        g_config = config;
        g_esPool = esPool;
        g_charmSystem = charmSystem;
        g_travelSettings = travelSettings;
        ResolveSafeReturnMarker();

        // 안전 제거 뒤에는 일반 지도 틱이 멈추지만 이미 비활성화한 액터의 삭제는 마저 처리한다.
        MainUpdateHook::AddFrameCallback([](RE::Main*, double deltaSec) {
            if (IsUninstallCleanupArmed()) {
                auto* ui = RE::UI::GetSingleton();
                if ((!ui || !ui->GameIsPaused()) && !IsTravelLoading()) {
                    TickPendingActorDeletes(deltaSec);
                }
            }
        });
    }

    void MapDeviceRuntime::InstallReturnDoorSink()
    {
        static MapReturnDoorSink sink;
        static bool installed = false;
        if (installed) {
            return;
        }
        if (auto* source = RE::ScriptEventSourceHolder::GetSingleton()) {
            source->AddEventSink<RE::TESActivateEvent>(&sink);
            installed = true;
        } else {
            SKSE::log::error("MapDeviceRuntime: could not register the return door activation sink.");
        }
    }

    void MapDeviceRuntime::BindDataPack(const Core::DataPack* dataPack)
    {
        std::scoped_lock lk(g_mutex);
        g_dataPack = dataPack;
        RefreshMapActorsLocked();
    }

    bool MapDeviceRuntime::MovePlayerToSpace(
        RE::PlayerCharacter* player,
        RE::TESObjectCELL* targetCell,
        RE::TESWorldSpace* targetWorldspace,
        const RE::NiPoint3& pos,
        const RE::NiPoint3& angle)
    {
        if (!player) {
            return false;
        }
        return InvokeMoveToImpl(player, player->CreateRefHandle(), targetCell, targetWorldspace, pos, angle);
    }

    void MapDeviceRuntime::SyncMapLayer2ToStatEngine()
    {
        if (!g_statEngine) {
            return;
        }
        bool inActive = false;
        Core::MapModSummary summary{};
        {
            std::scoped_lock lk(g_mutex);
            inActive = g_activeMap.active && g_playerInActiveMapCell.load(std::memory_order_acquire);
            if (inActive) {
                summary = g_activeMap.summary;
            }
        }
        g_statEngine->UpdateLayer2([inActive, summary](Core::Layer2Input& l2) {
            l2.inActiveMap = inActive;
            l2.activeMapMods = summary;
        });
    }

    void MapDeviceRuntime::Tick(double deltaSec, bool gamePaused)
    {
        if (IsUninstallCleanupArmed()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            {
                std::scoped_lock lk(g_mutex);
                g_lastObservedCell = 0;
                g_bossMissingSec = 0.0;
            }
            if (g_playerInActiveMapCell.exchange(false, std::memory_order_acq_rel)) {
                SyncMapLayer2ToStatEngine();
            }
            return;
        }
        auto observation = ObservePlayer(player);
        if (!gamePaused && !observation.loading) {
            TickPendingActorDeletes(deltaSec);
        }
        {
            bool travelPending = false;
            {
                std::scoped_lock lk(g_mutex);
                travelPending = PendingTravelKindLocked().has_value();
            }
            ObserveEngineFallSafety(player, observation, travelPending);
        }
        if (observation.cell != g_activeCellFormId.load(std::memory_order_acquire) &&
            g_playerInActiveMapCell.exchange(false, std::memory_order_acq_rel)) {
            SyncMapLayer2ToStatEngine();
        }
        AdvanceTravel(player, observation, deltaSec, gamePaused);
        // 도착 처리에서 새 귀환을 요청하거나 재시도할 수 있으므로 엔진 상태를 다시 읽는다.
        observation = ObservePlayer(player);

        bool accessChanged = false;
        bool warnUnauthorized = false;
        bool shouldSpawnNow = false;
        bool fallAuthorized = false;
        bool fallUsePortal = false;
        bool endRunNow = false;
        bool atlasDamaged = false;
        bool escapeDamagedRun = false;
        std::uint64_t bossCheckRunId = 0;
        RE::FormID bossCheckFormId = 0;
        bool stableRunCell = false;
        RE::FormID leftoverSweepCell = 0;
        bool exitZoneRequest = false;
        RE::FormID exitDoorCheckCell = 0;
        const Core::MapDungeonDef* fallDungeon = nullptr;
        std::vector<std::pair<RE::FormID, std::uint64_t>> resetCandidates;
        {
            std::scoped_lock lk(g_mutex);
            const auto access = GetEntryAccessLocked(observation);
            if (!observation.loading) {
                atlasDamaged = std::exchange(g_atlasDamagedAfterLoad, false);
            }
            g_permittedCell = access.permittedCell;
            accessChanged = g_playerInActiveMapCell.exchange(access.inActiveMap, std::memory_order_acq_rel) != access.inActiveMap;
            const auto pending = PendingTravelKindLocked();
            // 결정 182: 허가된 회차가 포털 0이고 보스를 아직 처치하지 못했으면(처치 반영 대기 포함) 출구 구역은 그 안에서만 귀환을 시도한다.
            // 거절된 뒤 구역을 벗어나면 래치를 풀어 결투 내내 다시 시도하거나 처치 순간 멀리서 귀환하지 않게 하고,
            // 출구 쪽으로 나가지 않으므로 구역 안에서도 첫 스폰을 막지 않는다.
            const bool exitZoneInsideOnly = access.inActiveMap && Core::MapSystem::ShouldRefuseExitZoneReturn(g_activeMap, false);
            resetCandidates = CollectCellResetCandidatesLocked(observation);
            if (!observation.loading) {
                if (!access.unauthorized) {
                    g_warnedUnauthorizedCell = 0;
                } else if (!g_runDamagedAfterLoad && observation.cell != g_warnedUnauthorizedCell && !pending) {
                    g_warnedUnauthorizedCell = observation.cell;
                    warnUnauthorized = true;
                }
            }
            if (observation.loading) {
                g_lastObservedCell = 0;
                g_leftoverSweptCell = 0;
                g_exitDoorsCheckedCell = 0;
                // 출구 구역 유지는 실제 로딩 화면에서만 푼다. 같은 셀의 페이드(낙하 복귀 등)로 풀면, 거절된 뒤 구역을 지나친
                // 출구 귀환을 다시 걸 수 없다(Codex 검토 2026-11-24). 셀이 바뀌면 NextExitZoneLatch와 팩 밖 관측이 푼다.
                if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
                    g_exitZoneLatchCell = 0;
                }
            }
            if (g_endCheckAfterLoad && !observation.loading && observation.cell != 0) {
                // 결정 175: 불러온 직후 셀을 못 읽었던 회차의 종료 판단을 셀이 확인된 뒤에 한다.
                g_endCheckAfterLoad = false;
                endRunNow = g_activeMap.active && !observation.inPackCell && Core::MapSystem::ShouldEndOnReturn(g_activeMap);
            }
            if (g_runDamageEscapeReady && g_runDamagedAfterLoad && !observation.loading && observation.cell != 0) {
                g_runDamagedAfterLoad = false;
                escapeDamagedRun = observation.inPackCell;
            }
            // 낙하 복귀와 보스 확인은 3D가 준비된 같은 셀을 두 틱 연속 본 뒤 시작한다.
            const bool observedReady = !observation.loading && observation.loaded3D && observation.cell != 0;
            const bool cellStable = observedReady && observation.cell == g_lastObservedCell;
            if (!gamePaused) {
                g_lastObservedCell = observedReady ? observation.cell : 0;
            }
            stableRunCell = cellStable && observation.cell == g_activeCellFormId.load(std::memory_order_relaxed);
            if (Core::MapSystem::CanCheckBossLoss(g_activeMap, BossLossObservationLocked(stableRunCell, gamePaused))) {
                bossCheckRunId = g_activeMap.runId;
                bossCheckFormId = g_activeMap.bossRefFormId;
            } else {
                g_bossMissingSec = 0.0;
            }
            bool belowMap = false;
            if (!gamePaused && !observation.loading) {
                const bool validDelta = std::isfinite(deltaSec) && deltaSec > 0.0;
                if (validDelta) {
                    g_fallRecoveryCooldownSec = std::max(0.0, g_fallRecoveryCooldownSec - deltaSec);
                    g_exitZoneRetryCooldownSec = std::max(0.0, g_exitZoneRetryCooldownSec - deltaSec);
                    g_exitZoneMessageCooldownSec = std::max(0.0, g_exitZoneMessageCooldownSec - deltaSec);
                    AdvanceCellResetTimersLocked(deltaSec);
                }
                if (cellStable && !observation.inPackCell) {
                    // 지도 팩 밖에서 관측되면 앞선 낙하 복귀의 이동(안전 탈출·포털 귀환)은 끝났다. 대기 시간은 로딩 중
                    // 줄지 않으므로, 다시 들어와 곧바로 떨어지면 남은 대기 때문에 판정을 건너뛰었다(2026-11-23 실행 11 H27).
                    g_fallRecoveryCooldownSec = 0.0;
                    g_fallRecoveryStreak = 0;
                    g_fallSkipLogged = false;
                    g_leftoverSweptCell = 0;
                    g_exitZoneLatchCell = 0;
                    g_exitDoorsCheckedCell = 0;
                }
                if (cellStable && observation.inPackCell) {
                    const auto* dungeon = MapPackRuntime::FindDungeonByCell(observation.cell);
                    const bool deathEscaping = g_deathEscapeRunId.load(std::memory_order_acquire) != 0;
                    if (observation.cell != g_exitDoorsCheckedCell) {
                        exitDoorCheckCell = observation.cell;
                    }
                    // 결정 177 ③ 재보완: 출구 구역은 같은 틱의 낙하 복귀보다 먼저 본다(출구에 닿았으면 지도를 떠나는 것이 맞다).
                    // 한 번 구역에 들면 같은 셀에 있는 동안 이동이 시작될 때까지 다시 시도한다.
                    const auto exitHit = dungeon
                        ? Core::FindExitZoneHit(observation, dungeon->autoExits, MapPackRuntime::Manifest().exitZoneRadius)
                        : std::nullopt;
                    const auto latched = Core::NextExitZoneLatch(
                        g_exitZoneLatchCell, observation, exitHit.has_value(), exitZoneInsideOnly);
                    if (exitHit && g_exitZoneLatchCell != latched) {
                        const auto& exit = dungeon->autoExits[exitHit->index];
                        SKSE::log::info("MapDeviceRuntime: entered the exit zone of {} exit 0x{:06X} at ({:.0f}, {:.0f}, {:.1f}), {:.0f} units "
                                        "(radius {:.0f}); requesting an exit (travelPending={}, deathEscape={}, insideOnly={}).",
                            dungeon->key, exit.localId, observation.position[0], observation.position[1], observation.position[2],
                            exitHit->distance, MapPackRuntime::Manifest().exitZoneRadius, pending.has_value(), deathEscaping, exitZoneInsideOnly);
                    }
                    g_exitZoneLatchCell = latched;
                    // 사망 가로채기가 꺼져 있으면 deathEscaping이 서지 않으므로 플레이어 사망도 따로 본다(Codex 검토 2026-11-24).
                    const bool playerDead = player && player->IsDead();
                    if (latched != 0 && !pending && !deathEscaping && !playerDead && g_exitZoneRetryCooldownSec == 0.0) {
                        exitZoneRequest = true;
                        g_exitZoneRetryCooldownSec = kExitZoneRetrySec;
                    }
                    belowMap = dungeon && Core::HasFallenBelowMap(observation, dungeon->navmeshMinZ, g_travelSettings.fallMargin);
                    if (!belowMap) {
                        g_fallRecoveryStreak = 0;
                        g_fallSkipLogged = false;
                    } else if (exitZoneRequest) {
                        // 출구 구역의 이동을 이번 틱에 건다. 거절되면 다음 틱부터 낙하 복귀도 다시 판정한다.
                    } else if (!pending && g_fallRecoveryCooldownSec == 0.0 && !deathEscaping) {
                        fallDungeon = dungeon;
                        fallAuthorized = access.inActiveMap;
                        fallUsePortal = ++g_fallRecoveryStreak >= 2;
                        g_fallRecoveryCooldownSec = kFallRecoveryCooldownSec;
                        g_fallSkipLogged = false;
                        SKSE::log::info("MapDeviceRuntime: fell below {} at Z={:.1f} (threshold {:.1f}); recovering (authorized={}, portal={}).",
                            dungeon->key, observation.position[2], dungeon->navmeshMinZ - g_travelSettings.fallMargin,
                            fallAuthorized, fallUsePortal);
                    } else if (!g_fallSkipLogged) {
                        g_fallSkipLogged = true;
                        SKSE::log::info("MapDeviceRuntime: fell below {} at Z={:.1f} but recovery waits (travelPending={}, cooldown={:.2f}s, deathEscape={}).",
                            dungeon->key, observation.position[2], pending.has_value(), g_fallRecoveryCooldownSec, deathEscaping);
                    }
                }
                if (const auto escapeRunId = g_deathEscapeRunId.load(std::memory_order_acquire); escapeRunId != 0) {
                    if (observation.inPackCell || !observation.loaded3D || observation.cell == 0) {
                        g_deathEscapeExitElapsedSec = 0.0;
                    } else if (validDelta) {
                        g_deathEscapeExitElapsedSec += deltaSec;
                        if (g_deathEscapeExitElapsedSec >= kDeathEscapeReleaseCooldownSec) {
                            g_deathEscapeExitElapsedSec = 0.0;
                            (void)Core::MapSystem::ReleaseDeathEscape(g_deathEscapeRunId, escapeRunId);
                        }
                    }
                }
                if (g_activeMap.active && g_arenaPhaseInvulnSec > 0.0 && validDelta) {
                    g_arenaPhaseInvulnSec = std::max(0.0, g_arenaPhaseInvulnSec - deltaSec);
                }
                shouldSpawnNow = access.inActiveMap && observation.loaded3D && !pending && !belowMap &&
                    (g_exitZoneLatchCell == 0 || exitZoneInsideOnly) &&
                    !g_spawnedForCurrentRun &&
                    g_pendingSpawnRunId != 0 && g_pendingSpawnRunId == g_activeMap.runId;
                // 남은 스폰 청소는 이동이 시작되지 않을 때만 한다. Disable은 작업 풀 예약이라, 같은 틱에 이동(낙하 복귀·안전 탈출·
                // 사망 귀환)이 걸리면 셀이 떨어진 뒤 실행될 수 있다(결정 178 ⑥과 같은 위험). 청소를 마친 뒤에 그 셀을 기록한다.
                if (cellStable && observation.inPackCell && observation.cell != g_leftoverSweptCell && !pending && !belowMap &&
                    !fallDungeon && !exitZoneRequest && !escapeDamagedRun && g_deathEscapeRunId.load(std::memory_order_acquire) == 0) {
                    leftoverSweepCell = observation.cell;
                }
            }
        }

        if (accessChanged) {
            SyncMapLayer2ToStatEngine();
        }
        if (leftoverSweepCell != 0 && SweepLeftoverSpawnsInCell(leftoverSweepCell, "entry")) {
            std::scoped_lock lk(g_mutex);
            g_leftoverSweptCell = leftoverSweepCell;
        }
        if (atlasDamaged) {
            ShowAtlasDamageOnMainThread();
        }
        if (escapeDamagedRun) {
            EscapeDamagedRunOnMainThread(player);
            return;
        }
        if (!resetCandidates.empty()) {
            SendCellResets(resetCandidates);
        }
        if (endRunNow) {
            EndRunOnMainThread();
            SKSE::log::info("MapDeviceRuntime: ended a run loaded outside the pack once the player cell was known.");
            return;
        }
        if (exitDoorCheckCell != 0) {
            const auto reenabled = MapPackRuntime::DisableEnabledExitDoors(exitDoorCheckCell);
            std::scoped_lock lk(g_mutex);
            g_exitDoorsCheckedCell = exitDoorCheckCell;
            g_exitDoorsReenabledCount += reenabled;
        }
        if (warnUnauthorized && !exitZoneRequest) {
            // 결정 177 ③ 재보완: 귀환 문이 없는 지도(파인문, 결투장)는 출구 구역으로 나갈 수 있으므로 함께 안내한다.
            RE::SendHUDMessage::ShowHUDMessage("입장 허가가 없습니다. 귀환 문이나 출구, 지도 탭의 안전 탈출을 이용하세요.");
        }
        if (exitZoneRequest) {
            if (const auto result = LeaveMapThroughExit(player, true)) {
                bool show = false;
                {
                    std::scoped_lock lk(g_mutex);
                    if (result->ok) {
                        ++g_exitZoneTravelCount;
                    } else {
                        g_exitZoneRetryCooldownSec = kExitZoneRefusedRetrySec;
                    }
                    show = result->ok || g_exitZoneMessageCooldownSec == 0.0;
                    if (show) {
                        g_exitZoneMessageCooldownSec = kExitZoneMessageSec;
                    }
                }
                if (show) {
                    SKSE::log::info("MapDeviceRuntime: exit zone travel {} ({}).", result->ok ? "started" : "refused", result->message);
                    ShowMapMessage(*result);
                }
            }
            return;  // 다음 틱에서 이동 상태를 다시 관측한다.
        }
        if (fallDungeon) {
            {
                std::scoped_lock lk(g_mutex);
                g_bossMissingSec = 0.0;
            }
            RecoverFromMapFall(player, *fallDungeon, fallAuthorized, fallUsePortal);
            return;  // 다음 틱에서 옮긴 위치를 다시 관측한 뒤 스폰한다.
        }
        if (shouldSpawnNow) {
            SpawnMapEncounterOnMainThread(player);
        }
        if (bossCheckRunId != 0) {
            const auto boss = LookupMapBoss(bossCheckFormId, observation.cell);
            const bool missing = !boss;
            bool suspected = false;
            {
                std::scoped_lock lk(g_mutex);
                if (g_activeMap.runId == bossCheckRunId && g_activeMap.bossRefFormId == bossCheckFormId) {
                    auto status = BossLossObservationLocked(stableRunCell, gamePaused);
                    status.bossMissing = missing;
                    const auto progress = Core::MapSystem::ObserveBossLoss(g_activeMap, status, g_bossMissingSec, deltaSec);
                    g_bossMissingSec = progress.missingSec;
                    suspected = progress.suspected;
                }
            }
            if (suspected) {
                RecoverMissingBossOnMainThread(player, observation.cell, bossCheckFormId, bossCheckRunId);
            }
        }
    }

    MapActionResult MapDeviceRuntime::OpenNormalMap(
        RE::PlayerCharacter* player, const Core::InventoryItemKey& mapKey, RE::FormID baseFormId)
    {
        if (auto reason = EntryUnavailable(player, MapTravelKind::OpenMap); !reason.empty()) {
            return { false, std::move(reason) };
        }
        const auto tier = FormIds::MapTierFromLocalFormId(FormIds::ResolveLocalFormIdIfOurs(baseFormId));
        if (!tier) {
            return { false, "유효한 지도 아이템이 아닙니다." };
        }
        const auto* dungeon = MapPackRuntime::Manifest().ForTier(*tier);
        const auto* forms = MapPackRuntime::FindForms(dungeon);
        if (!forms) {
            return { false, "지도 티어에 해당하는 복제 던전을 찾을 수 없습니다." };
        }

        auto* item = RE::TESForm::LookupByID<RE::TESBoundObject>(baseFormId);
        if (!item) {
            return { false, "지도 베이스 폼을 찾을 수 없습니다." };
        }
        if (mapKey.IsValid()) {
            if (!ItemTracker::FindByKey(mapKey) || !ItemTracker::FindExtraListByKey(player, item, mapKey)) {
                return { false, "인벤토리에서 해당 지도를 찾지 못했습니다." };
            }
        } else if (UniqueIdUtil::CountRemovablePlainItems(player, item) <= 0) {
            // 도착 때 소비(RemovePlainItem)와 같은 기준이어야 이름·즐겨찾기 등 보호된 지도만 있을 때 헛걸음하지 않는다.
            return { false, "인벤토리에 소비할 수 있는 일반 지도가 없습니다." };
        }
        return RequestEntry(player, MapTravelKind::OpenMap, *forms, { .mapKey = mapKey, .item = baseFormId },
            std::format("T{} {} 입장을 요청했습니다.", *tier, dungeon->name));
    }

    MapActionResult MapDeviceRuntime::OpenArena4(RE::PlayerCharacter* player)
    {
        if (auto reason = EntryUnavailable(player, MapTravelKind::OpenArena); !reason.empty()) {
            return { false, std::move(reason) };
        }
        if (auto rejection = GetArena4EntryRejection(); rejection.entryDenial != MapEntryDenial::None) {
            return rejection;
        }
        const auto& dungeon = MapPackRuntime::Manifest().arena;
        const auto* forms = MapPackRuntime::FindForms(&dungeon);
        if (!forms) {
            return { false, "복제 결투장을 찾을 수 없습니다." };
        }
        auto* tribute = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
        if (!tribute || CountPlayerTributes(player) <= 0) {
            return { false, "4차 전직의 공물이 필요합니다 (T6+ 지도 보스 처치로 획득)." };
        }
        return RequestEntry(player, MapTravelKind::OpenArena, *forms, { .item = tribute->GetFormID() },
            std::format("4차 전직 결투장 {} 입장을 요청했습니다.", dungeon.name));
    }

    MapActionResult MapDeviceRuntime::ReenterActiveMap(RE::PlayerCharacter* player)
    {
        if (auto reason = EntryUnavailable(player, MapTravelKind::Reenter); !reason.empty()) {
            return { false, std::move(reason) };
        }
        const MapPackRuntime::DungeonForms* forms = nullptr;
        std::uint64_t runId = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active || g_activeMap.failed || g_activeMap.isArena4 || g_activeMap.endRequested) {
                return { false, "재입장 가능한 활성 지도가 없습니다.", MapEntryDenial::NoReenterableRun };
            }
            if (g_activeMap.portalsRemaining == 0) {
                return { false, "남은 포털이 없습니다.", MapEntryDenial::NoPortals };
            }
            forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(g_activeMap));
            runId = g_activeMap.runId;
        }
        if (!forms) {
            return { false, "저장된 회차와 일치하는 복제 던전을 찾을 수 없습니다." };
        }
        return RequestEntry(player, MapTravelKind::Reenter, *forms, { .runId = runId }, "지도 재입장을 요청했습니다.");
    }

    MapActionResult MapDeviceRuntime::ReturnPortal(RE::PlayerCharacter* player)
    {
        if (!player) {
            return { false, "플레이어를 찾을 수 없습니다." };
        }
        const auto observation = ObservePlayer(player);
        Core::MapTravelTarget saved;
        std::uint64_t runId = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (PendingTravelKindLocked()) {
                return { false, "이동 중입니다. 도착 후 다시 시도하세요." };
            }
            if (!g_activeMap.active) {
                return { false, "활성 지도가 없습니다." };
            }
            const auto access = GetEntryAccessLocked(observation);
            if (!access.inActiveMap && (observation.inPackCell || g_retryReturnRunId != g_activeMap.runId)) {
                return { false, "허가된 회차 셀에서 귀환을 요청할 수 있습니다." };
            }
            saved = SavedReturn(g_activeMap);
            runId = g_activeMap.runId;
        }
        return RequestReturn(player, MapTravelKind::Return, saved, runId, "포털 귀환을 요청했습니다.");
    }

    MapActionResult MapDeviceRuntime::AbandonActiveMap(RE::PlayerCharacter* player)
    {
        if (!player) {
            return { false, "플레이어를 찾을 수 없습니다." };
        }
        Core::MapTravelTarget saved;
        std::uint64_t runId = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (PendingTravelKindLocked()) {
                return { false, "이동 중입니다. 도착 후 다시 시도하세요." };
            }
            if (!g_activeMap.active) {
                return { false, "종료할 활성 지도가 없습니다." };
            }
            saved = SavedReturn(g_activeMap);
            runId = g_activeMap.runId;
        }
        const auto* cell = player->GetParentCell();
        if (!cell) {
            return { false, "현재 셀을 찾을 수 없습니다. 이동을 마치고 다시 시도하세요." };
        }
        if (MapPackRuntime::OwnsCell(cell)) {
            return RequestReturn(player, MapTravelKind::Abandon, saved, runId, "회차 포기를 위한 귀환을 요청했습니다.");
        }
        EndRunOnMainThread();
        return { true, "지도 회차를 종료했습니다." };
    }

    MapActionResult MapDeviceRuntime::SafeEscape(RE::PlayerCharacter* player)
    {
        const auto status = GetTravelStatus(player);
        if (status.kind) {
            return { false, "이동 중입니다. 도착 후 다시 시도하세요." };
        }
        if (!status.unauthorized) {
            return { false, "입장 허가 없이 지도 팩 셀에 있을 때 안전 탈출을 사용할 수 있습니다." };
        }
        return RequestReturn(player, MapTravelKind::SafeEscape, {}, 0, "안전 탈출을 요청했습니다.");
    }

    MapTravelStatus MapDeviceRuntime::GetTravelStatus(RE::PlayerCharacter* player)
    {
        const auto observation = ObservePlayer(player);
        std::scoped_lock lk(g_mutex);
        return { PendingTravelKindLocked(), observation.inPackCell,
            GetEntryAccessLocked(observation).unauthorized, g_retryReturnRunId != 0 };
    }

    double MapDeviceRuntime::GetTravelArrivalRadius() noexcept
    {
        return g_travelSettings.arrivalRadius;
    }

    Core::MapTravelSettings MapDeviceRuntime::GetTravelSettings() noexcept
    {
        return g_travelSettings;
    }

    double MapDeviceRuntime::GetFallMargin() noexcept
    {
        return g_travelSettings.fallMargin;
    }

    bool MapDeviceRuntime::IsLeftoverSweepPending(RE::FormID actorId)
    {
        std::scoped_lock lk(g_mutex);
        return std::ranges::find(g_sweepDisableRequested, actorId) != g_sweepDisableRequested.end();
    }

    std::size_t MapDeviceRuntime::GetLeftoverSweepCleanedCount() noexcept
    {
        return g_leftoverSweepCleaned.load(std::memory_order_relaxed);
    }

    std::size_t MapDeviceRuntime::GetFallRecoveryCount()
    {
        std::scoped_lock lk(g_mutex);
        return g_fallRecoveryCount;
    }

    std::size_t MapDeviceRuntime::GetExitZoneTravelCount()
    {
        std::scoped_lock lk(g_mutex);
        return g_exitZoneTravelCount;
    }

    std::optional<std::string> MapDeviceRuntime::ExitZoneReturnRefusal()
    {
        // 보스 조회는 폼을 찾으므로 잠금 밖에서 한다. 아직 스폰 전이거나 찾지 못한 보스는 살아 있는 것으로 본다.
        const auto bossId = g_activeBossFormId.load(std::memory_order_acquire);
        const bool bossLooksDead = IsBossDeadNow(bossId);
        std::scoped_lock lk(g_mutex);
        // 귀환 쪽(RequestReturn)과 같이 조회한 보스가 지금 회차의 보스인지 보고, 사망 시작·확정 기록이 있으면
        // 액터를 찾지 못해도 쓰러진 것으로 본다(그 뒤 대기는 ReturnPortal의 처치 반영 대기가 맡음).
        const bool bossDeadNow = (bossLooksDead && bossId == g_activeMap.bossRefFormId) ||
            BossKillProcessingPendingLocked(g_activeMap.runId);
        if (!Core::MapSystem::ShouldRefuseExitZoneReturn(g_activeMap, bossDeadNow)) {
            return std::nullopt;
        }
        return g_activeMap.isArena4
            ? std::string("결투 중에는 출구로 나갈 수 없습니다. 보스를 처치한 뒤 나가세요(지도 탭의 귀환·포기는 회차를 끝냅니다).")
            : std::string("남은 포털이 없어 보스 처치 전에는 출구로 나갈 수 없습니다(지도 탭의 귀환·포기는 회차를 끝냅니다).");
    }

    std::size_t MapDeviceRuntime::GetExitDoorReenabledCount()
    {
        std::scoped_lock lk(g_mutex);
        return g_exitDoorsReenabledCount;
    }

    MapCellResetStatus MapDeviceRuntime::GetCellResetStatus(RE::FormID cellId)
    {
        std::scoped_lock lk(g_mutex);
        const auto sent = g_sentCellResets.find(cellId);
        const auto completed = g_completedCellResets.find(cellId);
        return { g_pendingCellResets.contains(cellId), sent == g_sentCellResets.end() ? 0 : sent->second,
            completed == g_completedCellResets.end() ? 0 : completed->second };
    }

    namespace
    {
        std::size_t CountGemAndSockets(const Core::GemInstance& gem)
        {
            std::size_t count = 1;
            for (const auto& socketed : gem.socketedGems) {
                count += CountGemAndSockets(socketed);
            }
            return count;
        }

        UninstallCleanupRemaining CountUninstallCleanupRemaining(const PersistentState& state)
        {
            UninstallCleanupRemaining remaining;
            for (const auto& slot : state.slots) {
                if (slot.occupied) {
                    remaining.gems += CountGemAndSockets(slot.gem);
                }
            }
            remaining.jewels = state.socketedJewels.size();
            remaining.charms = static_cast<std::size_t>(std::ranges::count_if(state.charmSlots,
                [](const auto& slot) { return slot.occupied; }));
            return remaining;
        }

        bool ReturnUninstallGem(RE::PlayerCharacter* player, const Core::GemInstance& gem,
            std::size_t& returned, std::size_t& lost)
        {
            const auto localId = FormIds::FindLocalFormIdForGem(gem.gemId);
            auto* base = localId != 0 ? FormIds::Lookup<RE::TESObjectMISC>(localId) : nullptr;
            if (!base) {
                ++lost;
                return true;
            }
            const auto countBefore = player->GetItemCount(base);
            if (!GemInventory::ReturnGemToPlayerInventory(player, gem)) {
                if (player->GetItemCount(base) <= countBefore) {
                    return false;
                }
                SKSE::log::error("MapDeviceRuntime: uninstall returned gem instance {} ('{}') without tracking; gem data lost.",
                    gem.instanceId, gem.gemId);
                RE::SendHUDMessage::ShowHUDMessage("젬을 가방으로 돌려받았지만 추적에 실패해 젬 정보가 사라졌습니다.");
            }
            ++returned;
            return true;
        }
    }

    std::optional<UninstallCleanupRemaining> MapDeviceRuntime::GetUninstallCleanupRemaining(const PersistentState& state)
    {
        if (!g_uninstallCleanupIncomplete.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        return CountUninstallCleanupRemaining(state);
    }

    MapActionResult MapDeviceRuntime::RunSafeUninstallCleanup(RE::PlayerCharacter* player)
    {
        if (!player) {
            return { false, "플레이어를 찾을 수 없습니다." };
        }
        const auto travel = GetTravelStatus(player);
        if (travel.kind || travel.inPackCell || !player->GetParentCell()) {
            return { false, "이동을 마치고 지도 밖에서 실행하세요." };
        }

        auto* av = player->AsActorValueOwner();
        if (!av) {
            return { false, "플레이어의 체력과 매지카를 확인할 수 없습니다." };
        }
        auto idle = CleanupStatus::Idle;
        if (!g_uninstallCleanupStatus.compare_exchange_strong(idle, CleanupStatus::InProgress, std::memory_order_acq_rel)) {
            return { false, "안전 제거 정리가 진행 중이거나 이미 완료되었습니다." };
        }
        struct ClearInProgressOnExit
        {
            ~ClearInProgressOnExit()
            {
                auto inProgress = CleanupStatus::InProgress;
                g_uninstallCleanupStatus.compare_exchange_strong(inProgress, CleanupStatus::Idle, std::memory_order_acq_rel);
            }
        } clearInProgressOnExit;

        // 회차 드랍은 액터 정리 전에 버리고, 나머지는 안전 제거 절차대로 각 시체에 돌려준다.
        CycloneRuntime::StopNowOnMainThread(player);
        if (g_activeMapRun.load(std::memory_order_acquire)) {
            EndRunOnMainThread();
        }
        WorldDropTracker::ReturnAllToCorpsesNow();
        (void)WorldDropTracker::SweepOrphanBeamsNearPlayer();

        std::size_t returnedGems = 0;
        std::size_t returnedJewels = 0;
        std::size_t returnedCharms = 0;
        std::size_t lostGems = 0;
        std::size_t lostJewels = 0;
        std::size_t lostCharms = 0;
        double appliedHpDelta = 0.0;
        double appliedMpDelta = 0.0;
        UninstallCleanupRemaining remaining;
        SaveLoad::RunSerializedTransaction([&]() {
            auto st = SaveLoad::GetStateCopy();
            appliedHpDelta = st.appliedHealthDelta;
            appliedMpDelta = st.appliedMagickaDelta;

            Core::GemTransaction::ReturnSlottedGemsForUninstall(st.slots, [&](const Core::GemInstance& gem) {
                return ReturnUninstallGem(player, gem, returnedGems, lostGems);
            });

            // 주얼은 원래 폼과 색상 기본 폼을 모두 확인한다. 찾은 경우 재검수 R-15의 반환 규칙을 쓴다.
            for (auto it = st.socketedJewels.begin(); it != st.socketedJewels.end();) {
                const auto& jewel = it->second;
                auto* base = RE::TESForm::LookupByID<RE::TESBoundObject>(jewel.baseFormId);
                if (!base) {
                    base = FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(jewel.jewelColor));
                }
                if (!base) {
                    ++lostJewels;
                    it = st.socketedJewels.erase(it);
                    continue;
                }
                if (auto* extra = UniqueIdUtil::CreateGameExtraDataList()) {
                    bool untrackedLeft = false;
                    const auto key = ItemTracker::AddObjectWithTrackedUniqueId(
                        player, base, extra, nullptr, ItemTracker::UntrackedAddPolicy::RemoveAgain, &untrackedLeft);
                    if (key.IsValid()) {
                        auto returned = jewel;
                        returned.baseFormId = base->GetFormID();
                        ItemTracker::Upsert(key, returned, false);
                        ++returnedJewels;
                        it = st.socketedJewels.erase(it);
                        continue;
                    }
                    if (untrackedLeft) {
                        SKSE::log::error("MapDeviceRuntime: uninstall returned jewel from node {} without tracking; affixes lost.", it->first);
                        RE::SendHUDMessage::ShowHUDMessage("주얼을 가방으로 돌려받았지만 추적에 실패해 어픽스가 사라졌습니다.");
                        ++returnedJewels;
                        it = st.socketedJewels.erase(it);
                        continue;
                    }
                }
                SKSE::log::warn("MapDeviceRuntime: uninstall could not return jewel from node {}; kept in the socket.", it->first);
                ++it;
            }

            if (g_charmSystem) {
                st.charmSlots = g_charmSystem->Slots();
            }
            for (auto& charm : st.charmSlots) {
                if (!charm.occupied) {
                    continue;
                }
                const auto localId = FormIds::CharmLocalFormId(charm.kind);
                auto* base = localId != 0 ? FormIds::Lookup<RE::TESObjectMISC>(localId) : nullptr;
                if (!base) {
                    ++lostCharms;
                    charm = Core::CharmSlotState{};
                    continue;
                }
                const auto countBefore = player->GetItemCount(base);
                player->AddObjectToContainer(base, nullptr, 1, nullptr);
                if (player->GetItemCount(base) > countBefore) {
                    charm = Core::CharmSlotState{};
                    ++returnedCharms;
                }
            }
            remaining = CountUninstallCleanupRemaining(st);
            if (g_charmSystem) {
                if (remaining.Empty()) {
                    g_charmSystem->ResetOnRevert();
                } else {
                    g_charmSystem->SetSlots(st.charmSlots, g_charmSystem->UnlockedSlots());
                }
            }

            if (remaining.Empty()) {
                if (std::abs(appliedHpDelta) > 0.01) {
                    const float revertHp = -static_cast<float>(appliedHpDelta);
                    if (revertHp < 0.0f) {
                        const float curHp = av->GetActorValue(RE::ActorValue::kHealth);
                        if (curHp > 0.0f && (curHp + revertHp) < 1.0f) {
                            const float comp = std::min(-revertHp, 1.0f - (curHp + revertHp));
                            if (comp > 0.0f) {
                                av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, comp);
                            }
                        }
                    }
                    av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kHealth, revertHp);
                }
                if (std::abs(appliedMpDelta) > 0.01) {
                    av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kMagicka,
                        -static_cast<float>(appliedMpDelta));
                }
                av->SetActorValue(RE::ActorValue::kVariable09, 0.0f);
            }

            SaveLoad::UpdateState([&](PersistentState& state) {
                state.slots = std::move(st.slots);
                state.socketedJewels = std::move(st.socketedJewels);
                state.charmSlots = st.charmSlots;
                if (remaining.Empty()) {
                    state.activeAuraMask = 0;
                    state.virtualRing2Key = {};
                    state.virtualRing2BaseFormId = 0;
                    state.virtualBeltKey = {};
                    state.virtualBeltBaseFormId = 0;
                    state.appliedHealthDelta = 0.0;
                    state.appliedMagickaDelta = 0.0;
                } else {
                    for (std::size_t i = 0; i < state.slots.size(); ++i) {
                        if (!state.slots[i].occupied) {
                            state.activeAuraMask &= static_cast<std::uint16_t>(~(1u << i));
                        }
                    }
                }
            });
        });

        std::string lostNotice;
        if (lostGems != 0 || lostJewels != 0 || lostCharms != 0) {
            lostNotice = std::format(" 돌려줄 수 없음(폼 유실) · 젬 {}개, 주얼 {}개, 참 {}개.",
                lostGems, lostJewels, lostCharms);
            SKSE::log::warn("MapDeviceRuntime: uninstall cleared missing item forms (gems={}, jewels={}, charms={}).",
                lostGems, lostJewels, lostCharms);
        }

        g_uninstallCleanupIncomplete.store(!remaining.Empty(), std::memory_order_release);
        if (!remaining.Empty()) {
            g_uninstallCleanupStatus.store(CleanupStatus::Idle, std::memory_order_release);
            EquipmentSync::RefreshFromPlayerNow();
            SaveLoad::SyncStateToStatEngine();
            SyncMapLayer2ToStatEngine();

            const auto failMsg = std::format(
                "안전 제거 미완료. 장착된 젬 {}개, 주얼 {}개, 참 {}개가 남았습니다. 정리를 다시 시도하세요.{}",
                remaining.gems, remaining.jewels, remaining.charms, lostNotice);
            RE::SendHUDMessage::ShowHUDMessage(failMsg.c_str());
            SKSE::log::error("MapDeviceRuntime: SafeUninstallCleanup incomplete (remaining gems={}, jewels={}, charms={}).",
                remaining.gems, remaining.jewels, remaining.charms);
            return { false, failMsg };
        }

        ProxySpellRuntime::RemoveAllProxySpells(player);
        AilmentRuntime::ClearPlayerAilments();
        AilmentRuntime::RevertAllTrackedSpeedsNow();
        MonsterRuntime::RevertAllHasteSpeedsNow();
        const auto pendingSpeedReverts = SaveLoad::RevertAllPendingSpeedModsNow();

        if (g_esPool) {
            g_esPool->Clear();
        }
        if (g_statEngine) {
            g_statEngine->SetLayer1(Core::Layer1Input{});
            g_statEngine->SetLayer2(Core::Layer2Input{});
        }

        {
            std::scoped_lock lk(g_mutex);
            ClearTravelLocked();
        }
        g_uninstallCleanupStatus.store(CleanupStatus::Armed, std::memory_order_release);
        if (g_queue) {
            g_queue->InvalidatePending();
        }

        auto msg = std::format(
            "안전 제거 정리 완료. 젬 {}개, 주얼 {}개, 참 {}개를 반환했습니다. 저장 후 종료하세요.{}",
            returnedGems, returnedJewels, returnedCharms, lostNotice);
        if (pendingSpeedReverts > 0) {
            msg += std::format(" 속도 원복 대기 NPC {}명. 이 NPC들이 로드될 때 DLL이 원복하므로 지금 모드를 제거하면 속도 변경이 남을 수 있습니다.",
                pendingSpeedReverts);
        }
        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        SKSE::log::info("MapDeviceRuntime: SafeUninstallCleanup armed (gems={}, jewels={}, charms={}, hpDelta={:.1f}, mpDelta={:.1f}, pendingSpeedReverts={}).",
            returnedGems, returnedJewels, returnedCharms, appliedHpDelta, appliedMpDelta, pendingSpeedReverts);
        return { true, msg };
    }

    void MapDeviceRuntime::OnActorKilled(RE::FormID actorId, std::uint64_t runId, MapKillReward reward)
    {
        if (actorId == 0 || runId == 0 || IsUninstallCleanupActive()) {
            return;
        }
        bool progressed = false;
        bool isArena4 = false;
        std::uint8_t tier = 0;
        auto denial = Core::Arena4EntryDenial::None;
        bool firstClear = false;
        std::uint32_t totalAtlasPts = 0;
        SaveLoad::RunSerializedTransaction([&]() {
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_activeMap.runId != runId || g_activeMap.bossRefFormId != actorId) {
                    return;
                }
                // 결정 179 개정: 회차가 실패하기 전에 사망이 시작된 보스는 그 뒤 실패해도 처치를 반영한다.
                const bool killedBeforeFailure = g_bossDeathStart && g_bossDeathStart->runId == runId;
                if ((g_activeMap.failed && !killedBeforeFailure) || g_activeMap.bossKilled) {
                    g_bossDeathPendingRunId = 0;
                    return;
                }
                g_activeMap.bossKilled = true;
                g_bossDeathPendingRunId = 0;
                isArena4 = g_activeMap.isArena4;
                tier = g_activeMap.tier;
            }
            progressed = true;
            if (reward != MapKillReward::Eligible) {
                return;
            }
            if (isArena4) {
                SaveLoad::UpdateState([&](PersistentState& st) {
                    denial = Core::MapSystem::TryUnlockArena4(st.tree.ascendancyMilestonesBitmask);
                    if (denial == Core::Arena4EntryDenial::None && g_treeData && g_treeData->IsLoaded() && g_config) {
                        const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                            *g_treeData, st.tree, st.exileLevel, *g_config, st.atlas.completedTierMask);
                        st.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
                    }
                });
                return;
            }
            SaveLoad::UpdateState([&](PersistentState& st) {
                firstClear = Core::MapSystem::RecordMapClear(st.atlas, tier);
                totalAtlasPts = Core::MapSystem::CountAtlasTierPoints(st.atlas.completedTierMask);
                if (firstClear && g_treeData && g_treeData->IsLoaded() && g_config) {
                    const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                        *g_treeData, st.tree, st.exileLevel, *g_config, st.atlas.completedTierMask);
                    st.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
                }
            });
        });
        if (!progressed) {
            return;
        }
        SKSE::log::info("MapDeviceRuntime: boss {:08X} kill progressed run {} (reward={}, arena4={}, tier={}, firstClear={}).",
            actorId, runId, static_cast<int>(reward), isArena4, static_cast<unsigned>(tier), firstClear);
        if (reward != MapKillReward::Eligible) {
            if (!isArena4) {
                RE::SendHUDMessage::ShowHUDMessage(reward == MapKillReward::NoPlayerContribution
                    ? "[지도 장치] 보스 처치로 회차 진행 · 플레이어 기여가 없어 아틀라스 보상 없음"
                    : "[지도 장치] 보스 처치로 회차 진행 · 아틀라스 보상 자격 없음");
            } else {
                RE::SendHUDMessage::ShowHUDMessage(
                    "[결투장] 보상 조건(3페이즈 처치·플레이어 기여)을 채우지 못해 4차 전직을 얻지 못했습니다.");
            }
            return;
        }
        if (isArena4) {
            if (denial == Core::Arena4EntryDenial::None) {
                SaveLoad::SyncStateToStatEngine();
                RE::SendHUDMessage::ShowHUDMessage(
                    "[SkyrimPoE] 4차 전직 결투장 정복! 전직 8/8, 패시브 +2, 기본 최대 정신력 220 해금!");
                SKSE::log::info("MapDeviceRuntime: 4th Ascendancy Arena cleared -> bitmask |= 0x8 (8/8 asc, Spirit 220).");
            } else if (denial == Core::Arena4EntryDenial::MissingMilestones) {
                RE::SendHUDMessage::ShowHUDMessage("[결투장] 1~3차 전직 이정표가 부족해 4차 전직을 얻지 못했습니다.");
            } else {
                RE::SendHUDMessage::ShowHUDMessage("[SkyrimPoE] 4차 전직 결투장 보스 처치 완료!");
            }
            return;
        }

        if (firstClear) {
            SaveLoad::SyncStateToStatEngine();
            const auto msg = std::format("[아틀라스] T{} 첫 클리어! 패시브 포인트 +1 (아틀라스 {}/16)",
                tier, totalAtlasPts);
            RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
            SKSE::log::info("MapDeviceRuntime: Atlas T{} first clear (totalAtlasPoints={}/16).",
                tier, totalAtlasPts);
        } else {
            const auto msg = std::format("[지도 장치] T{} 지도 보스 처치 완료!", tier);
            RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        }
    }

    double MapDeviceRuntime::FilterArena4BossIncomingDamage(
        RE::Actor* boss,
        double rawSkyrimDamage,
        double diffMult,
        std::optional<std::uint64_t> eventGeneration)
    {
        if (!boss || !std::isfinite(rawSkyrimDamage) || rawSkyrimDamage <= 0.0) {
            return std::max(0.0, std::isfinite(rawSkyrimDamage) ? rawSkyrimDamage : 0.0);
        }
        if (!g_queue || IsUninstallCleanupActive() || !g_activeIsArena4.load(std::memory_order_acquire) ||
            boss->GetFormID() != g_activeBossFormId.load(std::memory_order_acquire)) {
            return rawSkyrimDamage;
        }
        const auto generation = eventGeneration.value_or(g_queue->CurrentGeneration());
        const double safeDiff = (std::isfinite(diffMult) && diffMult > 1e-4) ? diffMult : 1.0;
        const auto* av = boss->AsActorValueOwner();
        const double curHp = av ? std::max(0.0, static_cast<double>(av->GetActorValue(RE::ActorValue::kHealth))) : 100.0;
        const double maxHp = av ? std::max(1.0, static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth))) : 100.0;

        Core::ArenaDamageClampResult clampRes{};
        const RE::FormID bossId = boss->GetFormID();
        std::uint64_t runId = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (IsUninstallCleanupActive() || generation != g_queue->CurrentGeneration() ||
                !g_activeMap.active || g_activeMap.failed || !g_activeMap.isArena4 || g_activeMap.bossRefFormId != bossId ||
                g_activeMap.bossKilled || g_activeMap.endRequested) {
                return rawSkyrimDamage;
            }
            clampRes = Core::MapSystem::ClampArenaBossDamage(
                g_activeMap.arenaPhase,
                IsArena4PhaseInvulnerableLocked(),
                curHp,
                maxHp,
                rawSkyrimDamage * safeDiff);
            if (clampRes.triggeredNextPhase != 0) {
                runId = g_activeMap.runId;
                g_activeMap.arenaPhase = clampRes.triggeredNextPhase;
                g_arenaPhaseInvulnSec = Core::kArena4PhaseInvulnSec;
            }
        }
        if (clampRes.triggeredNextPhase != 0) {
            const std::uint8_t nextPhase = clampRes.triggeredNextPhase;
            g_queue->EnqueueForGeneration(generation, [nextPhase, bossId, runId]() {
                ExecuteArena4PhaseTransitionOnMainThread(nextPhase, bossId, runId);
            });
        }
        return std::max(0.0, clampRes.clampedDamage / safeDiff);
    }

    namespace
    {
        enum class DeathHitDecision { Ignore, ClampInFlight, RequestEscape };

        DeathHitDecision GetDeathHitDecision(
            bool enabled, double curHp, double effectiveDamage, std::uint64_t generation) noexcept
        {
            if (!enabled || !g_queue || !g_config ||
                MapDeviceRuntime::IsUninstallCleanupActive() || generation != g_queue->CurrentGeneration() ||
                !g_playerInActiveMapCell.load(std::memory_order_acquire)) {
                return DeathHitDecision::Ignore;
            }
            if (g_deathEscapeRunId.load(std::memory_order_acquire) != 0) {
                return DeathHitDecision::ClampInFlight;
            }
            constexpr double kEscapeHealthFloor = 1.0;
            return std::isfinite(curHp) && std::isfinite(effectiveDamage) && effectiveDamage > 0.0 &&
                (curHp - effectiveDamage) <= kEscapeHealthFloor ? DeathHitDecision::RequestEscape : DeathHitDecision::Ignore;
        }

        bool CanRequestDeathEscapeLocked(std::uint64_t generation) noexcept
        {
            return !MapDeviceRuntime::IsUninstallCleanupActive() && generation == g_queue->CurrentGeneration() &&
                g_activeMap.active && g_playerInActiveMapCell.load(std::memory_order_relaxed);
        }
    }

    bool MapDeviceRuntime::WouldInterceptLethalHit(
        bool enabled, double curHp, double effectiveDamage, std::uint64_t generation)
    {
        const auto decision = GetDeathHitDecision(enabled, curHp, effectiveDamage, generation);
        if (decision != DeathHitDecision::RequestEscape) {
            return decision == DeathHitDecision::ClampInFlight;
        }
        std::scoped_lock lk(g_mutex);
        return CanRequestDeathEscapeLocked(generation);
    }

    bool MapDeviceRuntime::TryInterceptLethalHitFromHook(
        double curHp, double effectiveDamage, std::uint64_t generation, RE::FormID attackerId)
    {
        if (!g_config) {
            return false;
        }
        const bool enabled = std::atomic_ref<const bool>(
            g_config->mapDeathInterceptEnabled).load(std::memory_order_relaxed);
        const auto decision = GetDeathHitDecision(enabled, curHp, effectiveDamage, generation);
        if (decision != DeathHitDecision::RequestEscape) {
            return decision == DeathHitDecision::ClampInFlight;
        }

        std::uint64_t runId = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (!CanRequestDeathEscapeLocked(generation)) {
                return false;
            }
            runId = g_activeMap.runId;
            if (g_deathEscapeRunId.load(std::memory_order_relaxed) == runId) {
                return true;
            }
            g_deathEscapeRunId.store(runId, std::memory_order_release);
            const auto requestCount = g_deathEscapeRequestCount.fetch_add(1, std::memory_order_relaxed) + 1;
            g_lastDeathEscapeRequest = { requestCount, attackerId, std::chrono::steady_clock::now() };
            if (!g_activeMap.failed && (g_activeMap.isArena4 || g_activeMap.portalsRemaining == 0)) {
                g_activeMap.failed = true;
                RefreshMapActorsLocked();
            }
        }
        g_queue->EnqueueForGeneration(generation, [runId]() {
            ExecuteDeathEscapeOnMainThread(runId);
        });
        // 무엇이 사망 귀환을 일으켰는지 남긴다(실행 15의 H4는 시험이 피해를 보내기 전에 이미 귀환 중이었다). 사망 귀환을 새로
        // 접수할 때만 닿는다(귀환이 실패하거나 팩 밖에서 표시가 풀리면 같은 회차에서 다시 닿을 수 있다).
        SKSE::log::info("MapDeviceRuntime: death escape requested for run {} (attacker=0x{:08X}, health={:.1f}, damage={:.1f}).",
            runId, attackerId, curHp, effectiveDamage);
        return true;
    }

    MapDeathEscapeRequestObservation MapDeviceRuntime::GetLastDeathEscapeRequest()
    {
        std::scoped_lock lk(g_mutex);
        return g_lastDeathEscapeRequest;
    }

    std::uint64_t MapDeviceRuntime::GetDeathEscapeRequestCount() noexcept
    {
        return g_deathEscapeRequestCount.load(std::memory_order_relaxed);
    }

    bool MapDeviceRuntime::IsPlayerInActiveMapCell() noexcept
    {
        return g_playerInActiveMapCell.load(std::memory_order_acquire);
    }

    bool MapDeviceRuntime::IsActiveMapRun() noexcept
    {
        return g_activeMapRun.load(std::memory_order_acquire);
    }

    bool MapDeviceRuntime::IsActiveMapCell(RE::FormID cellFormId) noexcept
    {
        return cellFormId != 0 &&
               g_activeMapRun.load(std::memory_order_acquire) &&
               g_activeCellFormId.load(std::memory_order_acquire) == cellFormId;
    }

    std::uint8_t MapDeviceRuntime::GetActiveMapTier() noexcept
    {
        return g_activeMapTier.load(std::memory_order_acquire);
    }

    bool MapDeviceRuntime::IsActiveMapOrArenaBoss(RE::FormID actorFormId) noexcept
    {
        return actorFormId != 0 &&
               g_activeMapRun.load(std::memory_order_acquire) &&
               g_activeBossFormId.load(std::memory_order_acquire) == actorFormId;
    }

    bool MapDeviceRuntime::IsActiveArena4Boss(RE::FormID actorFormId) noexcept
    {
        return actorFormId != 0 &&
               g_activeIsArena4.load(std::memory_order_acquire) &&
               g_activeBossFormId.load(std::memory_order_acquire) == actorFormId;
    }

    bool MapDeviceRuntime::IsArena4BossInvulnerable(RE::FormID actorId)
    {
        std::scoped_lock lk(g_mutex);
        return actorId != 0 && g_activeMap.active && g_activeMap.isArena4 &&
               g_activeMap.bossRefFormId == actorId && IsArena4PhaseInvulnerableLocked();
    }

    Arena4BossPhaseStatus MapDeviceRuntime::GetArena4BossPhaseStatusForAutoTest(RE::FormID actorId)
    {
        std::scoped_lock lk(g_mutex);
        if (actorId == 0 || !g_activeMap.active || !g_activeMap.isArena4 || g_activeMap.bossRefFormId != actorId) {
            return {};
        }
        return { g_activeMap.arenaPhase, g_completedArenaPhase, IsArena4PhaseInvulnerableLocked() };
    }

    bool MapDeviceRuntime::IsDeathEscapeInFlight() noexcept
    {
        return g_deathEscapeRunId.load(std::memory_order_acquire) != 0;
    }

    bool MapDeviceRuntime::IsUninstallCleanupArmed() noexcept
    {
        return g_uninstallCleanupStatus.load(std::memory_order_acquire) == CleanupStatus::Armed;
    }

    bool MapDeviceRuntime::IsUninstallCleanupActive() noexcept
    {
        // The combat and stat hooks use this gate even outside the main update.
        return !ModState::AllowsGameplay() ||
            g_uninstallCleanupStatus.load(std::memory_order_acquire) != CleanupStatus::Idle;
    }

    void MapDeviceRuntime::DisarmUninstallCleanup() noexcept
    {
        g_uninstallCleanupIncomplete.store(false, std::memory_order_release);
        g_uninstallCleanupStatus.store(CleanupStatus::Idle, std::memory_order_release);
    }

    Core::ActiveMapState MapDeviceRuntime::GetActiveMapStateCopy()
    {
        std::scoped_lock lk(g_mutex);
        return g_activeMap;
    }

    std::uint64_t MapDeviceRuntime::GetActiveRunId()
    {
        std::scoped_lock lk(g_mutex);
        return g_activeMap.active ? g_activeMap.runId : 0;
    }

    bool MapDeviceRuntime::IsRunActive(std::uint64_t runId)
    {
        std::scoped_lock lk(g_mutex);
        return runId != 0 && g_activeMap.active && g_activeMap.runId == runId;
    }

    bool MapDeviceRuntime::IsRunRewardable(std::uint64_t runId)
    {
        std::scoped_lock lk(g_mutex);
        return runId != 0 && g_activeMap.active && !g_activeMap.failed && g_activeMap.runId == runId;
    }

    bool MapDeviceRuntime::IsActiveRunMapActor(RE::FormID actorFormId)
    {
        if (actorFormId == 0) {
            return false;
        }
        std::scoped_lock lk(g_mapActorsMutex);
        return g_mapActors.Find(actorFormId).has_value();
    }

    Core::MapActorScaling MapDeviceRuntime::GetMapActorScaling(RE::FormID actorId, Core::MapModSummary* mods)
    {
        std::scoped_lock lk(g_mapActorsMutex);
        const auto actor = g_mapActors.Find(actorId);
        if (mods) {
            *mods = actor ? g_mapActorMods : Core::MapModSummary{};
        }
        return actor.value_or(Core::MapActorScaling{});
    }

    void MapDeviceRuntime::NoteMapBossDeathStart(RE::FormID actorId, RE::FormID playerCellId)
    {
        if (actorId == 0 || g_activeBossFormId.load(std::memory_order_acquire) != actorId) {
            return;
        }
        std::scoped_lock lk(g_mutex);
        if (!g_activeMap.active || g_activeMap.failed || g_activeMap.bossKilled ||
            g_activeMap.bossRefFormId != actorId ||
            (g_bossDeathStart && g_bossDeathStart->runId == g_activeMap.runId)) {
            return;
        }
        g_bossDeathStart = MapBossDeathStart{
            g_activeMap.runId,
            IsMapRewardBlockedLocked(playerCellId),
            g_activeMap.isArena4
                ? std::optional<Core::Arena4BossDeathState>{ { g_completedArenaPhase, IsArena4PhaseInvulnerableLocked() } }
                : std::nullopt
        };
        SKSE::log::info("MapDeviceRuntime: boss {:08X} death started in run {} (rewardBlocked={}, arenaPhase={}, invulnerable={}).",
            actorId, g_activeMap.runId, g_bossDeathStart->rewardBlocked,
            g_bossDeathStart->arena ? static_cast<unsigned>(g_bossDeathStart->arena->completedPhase) : 0u,
            g_bossDeathStart->arena && g_bossDeathStart->arena->invulnerable);
    }

    void MapDeviceRuntime::ClearMapBossDeathStart(RE::FormID actorId)
    {
        if (actorId == 0 || g_activeBossFormId.load(std::memory_order_acquire) != actorId) {
            return;
        }
        std::scoped_lock lk(g_mutex);
        // 사망 확정 뒤의 리셋은 이미 확정된 처치를 취소하지 않는다(큐의 처치 반영이 이 기록을 쓴다).
        if (!g_activeMap.active || g_activeMap.bossRefFormId != actorId ||
            !g_bossDeathStart || g_bossDeathStart->runId != g_activeMap.runId ||
            g_bossDeathConfirmedRunId == g_activeMap.runId) {
            return;
        }
        // 확정 전 리셋으로 사망이 취소됐다. 귀환 대기(BossKillProcessingPendingLocked)도 이 기록이 사라지면 풀린다.
        // 이 사망 때문에 시작한 귀환 지연 시계도 다음 사망에 이어지지 않게 지운다.
        g_bossDeathStart.reset();
        g_firstBossDeathReturnDeferral.reset();
    }

    MapKillContext MapDeviceRuntime::GetMapKillContext(RE::FormID actorId, RE::FormID playerCellId)
    {
        std::scoped_lock lk(g_mutex);
        return GetMapKillContextLocked(actorId, playerCellId);
    }

    std::optional<MapKillContext> MapDeviceRuntime::ConfirmMapKill(RE::FormID actorId, RE::FormID playerCellId)
    {
        std::scoped_lock lk(g_mutex);
        const auto context = GetMapKillContextLocked(actorId, playerCellId);
        if (context.isBoss) {
            if (g_activeMap.bossKilled || g_bossDeathConfirmedRunId == context.runId) {
                SKSE::log::info("MapDeviceRuntime: boss {:08X} death confirm ignored for run {} (bossKilled={}, alreadyConfirmed={}).",
                    actorId, context.runId, g_activeMap.bossKilled, g_bossDeathConfirmedRunId == context.runId);
                return std::nullopt;
            }
            // 결정 175·179: 허가와 관계없이 보스 처리가 끝날 때까지 귀환 확정을 미룬다.
            // 치명타가 회차 실패보다 먼저였으면(결정 179 개정) context.runFailed가 거짓이라 함께 미룬다.
            if (!context.runFailed) {
                g_bossDeathPendingRunId = context.runId;
            }
            g_bossDeathConfirmedRunId = context.runId;
        }
        return context;
    }

    std::int32_t MapDeviceRuntime::CountPlayerTributes(RE::PlayerCharacter* player)
    {
        if (!player) {
            return 0;
        }
        auto* tributeObj = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
        if (!tributeObj) {
            return 0;
        }
        const auto counts = player->GetInventoryCounts([tributeObj](RE::TESBoundObject& obj) {
            return &obj == tributeObj;
        });
        const auto it = counts.find(tributeObj);
        return it != counts.end() ? it->second : 0;
    }

    std::vector<PlayerMapItemEntry> MapDeviceRuntime::ListPlayerMaps(RE::PlayerCharacter* player)
    {
        std::vector<PlayerMapItemEntry> result;
        if (!player) {
            return result;
        }
        auto inv = player->GetInventory([](RE::TESBoundObject& obj) {
            if (!obj.As<RE::TESObjectMISC>()) {
                return false;
            }
            return FormIds::IsMapItemLocalFormId(FormIds::ResolveLocalFormIdIfOurs(obj.GetFormID()));
        });

        for (const auto& [boundObj, pair] : inv) {
            const auto totalCount = pair.first;
            const auto& entryData = pair.second;
            if (!boundObj || totalCount <= 0) {
                continue;
            }
            const auto localId = FormIds::ResolveLocalFormIdIfOurs(boundObj->GetFormID());
            const auto tierOpt = FormIds::MapTierFromLocalFormId(localId);
            if (!tierOpt) {
                continue;
            }
            const std::uint8_t tier = *tierOpt;
            const auto* dungeon = MapPackRuntime::Manifest().ForTier(tier);
            const auto displayName = std::format("T{} {} (Lv.{})", tier,
                dungeon ? dungeon->name : "지도", Core::MapSystem::MonsterLevelForTier(tier));
            std::int32_t trackedCount = 0;

            if (entryData && entryData->extraLists) {
                for (auto* xList : *entryData->extraLists) {
                    if (!xList) {
                        continue;
                    }
                    const auto* uid = xList->GetByType<RE::ExtraUniqueID>();
                    if (!uid || uid->uniqueID == 0) {
                        continue;
                    }
                    const Core::InventoryItemKey key{ uid->baseID, uid->uniqueID };
                    const auto recOpt = ItemTracker::FindByKey(key);
                    if (!recOpt) {
                        continue;
                    }
                    const std::int32_t c = std::max<std::int32_t>(1, xList->GetCount());
                    trackedCount += c;

                    PlayerMapItemEntry item{};
                    item.key = key;
                    item.baseFormId = boundObj->GetFormID();
                    item.tier = tier;
                    item.count = c;
                    item.tracked = true;
                    item.rarity = recOpt->rarity;
                    item.summary = Core::MapSystem::AggregateMapMods(recOpt->affixes);
                    item.displayName = displayName;
                    item.modLines.reserve(recOpt->affixes.size());
                    for (const auto& af : recOpt->affixes) {
                        item.modLines.push_back(Core::MapSystem::FormatMapModKoreanLine(af));
                    }
                    result.push_back(std::move(item));
                }
            }

            const std::int32_t plainCount = std::max<std::int32_t>(0, totalCount - trackedCount);
            if (plainCount > 0) {
                PlayerMapItemEntry item{};
                item.key = {};
                item.baseFormId = boundObj->GetFormID();
                item.tier = tier;
                item.count = plainCount;
                item.tracked = false;
                item.rarity = Core::ItemRarity::Normal;
                item.displayName = displayName;
                result.push_back(std::move(item));
            }
        }

        std::sort(result.begin(), result.end(), [](const PlayerMapItemEntry& a, const PlayerMapItemEntry& b) {
            if (a.tier != b.tier) {
                return a.tier < b.tier;
            }
            if (a.rarity != b.rarity) {
                return static_cast<std::uint8_t>(a.rarity) > static_cast<std::uint8_t>(b.rarity);
            }
            return a.key.uniqueId < b.key.uniqueId;
        });
        return result;
    }

    Core::ActiveMapState MapDeviceRuntime::TakeSnapshotForSave()
    {
        std::scoped_lock lk(g_mutex);
        auto snapshot = g_activeMap;
        snapshot.arenaPhase = Core::MapSystem::Arena4PhaseForSave(snapshot.arenaPhase, g_completedArenaPhase);
        snapshot.arenaPhaseInvulnSec = static_cast<float>(g_arenaPhaseInvulnSec);
        // 결정 176 ④: 도착 확인을 거친 허가만 저장한다. 허가 없이 셀에 있던 세이브는 불러와도 허가가 생기지 않는다.
        const auto activeCell = g_activeCellFormId.load(std::memory_order_acquire);
        snapshot.playerAuthorized = g_activeMap.active && activeCell != 0 && g_permittedCell == activeCell;
        return snapshot;
    }

    void MapDeviceRuntime::RestoreFromSave(Core::ActiveMapState saved, RE::FormID legacyCell, bool atlasDamaged, bool runDamaged)
    {
        std::scoped_lock lk(g_mutex);
        ClearTravelLocked();
        g_runSpawnDisableRequests.clear();
        g_atlasDamagedAfterLoad = atlasDamaged;
        g_runDamagedAfterLoad = runDamaged;
        g_runDamageEscapeReady = false;
        g_permittedCell = 0;
        g_playerInActiveMapCell.store(false, std::memory_order_release);
        g_incompatibleSavedRun.reset();
        if (saved.active && !Core::IsValidMapRunId(saved.runId)) {
            SKSE::log::warn("MapDeviceRuntime: legacy map run ID {} is outside the current range; assigned {}.",
                saved.runId, g_nextRunId);
            saved.runId = g_nextRunId;
        }
        if (saved.active && saved.runId >= g_nextRunId) {
            g_nextRunId = Core::NextMapRunId(saved.runId);
        }
        if (saved.active && !MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(saved))) {
            // 결정 169, R4: 호환되지 않는 회차는 로드 중 등록부에 올리지 않고 주 스레드에서 정리한다.
            g_incompatibleSavedRun = IncompatibleSavedRun{ std::move(saved), legacyCell };
            SetActiveMapLocked({});
        } else {
            auto refund = RecreateSavedMapRefund(saved);
            SetActiveMapLocked(std::move(saved), std::move(refund));
        }
        g_spawnedForCurrentRun = g_activeMap.bossKilled || g_activeMap.spawnsDisabled || g_activeMap.unresolvedBossRefFormId != 0 ||
            g_activeMap.bossRefFormId != 0 || !g_activeMap.spawnedRefFormIds.empty();
        g_bossMissingSec = 0.0;
        g_pendingSpawnRunId = (g_activeMap.active && !g_spawnedForCurrentRun) ? g_activeMap.runId : 0;
        g_completedArenaPhase = g_activeMap.arenaPhase;
        g_arenaPhaseInvulnSec = g_activeMap.arenaPhaseInvulnSec;
        g_deathEscapeExitElapsedSec = 0.0;
        g_deathEscapeRunId.store(0, std::memory_order_release);
        RefreshAtomicsLocked();
    }

    void MapDeviceRuntime::OnPostLoadRebind()
    {
        DisarmUninstallCleanup();
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        const bool inPackCell = MapPackRuntime::OwnsCell(cell);
        const bool loading = IsTravelLoading();
        bool atlasDamaged = false;
        {
            std::scoped_lock lk(g_mutex);
            if (!loading) {
                atlasDamaged = std::exchange(g_atlasDamagedAfterLoad, false);
            }
            // 저장하지 않는 요청과 허가는 불러온 세이브에서 새로 시작한다.
            ClearTravelLocked();
            g_pendingReturnConfirmation.reset();
            g_bossDeathPendingRunId = 0;
            g_bossDeathConfirmedRunId = 0;
            g_firstBossDeathReturnDeferral.reset();
            g_bossDeathStart.reset();
            g_permittedCell = 0;
            g_warnedUnauthorizedCell = 0;
            g_retryReturnRunId = 0;
            g_pendingActorDeletes.clear();
            ++g_pendingActorDeleteGeneration;
            g_pendingCellResets.clear();
            g_sentCellResets.clear();
            g_completedCellResets.clear();
            g_fallRecoveryCooldownSec = 0.0;
            g_fallRecoveryStreak = 0;
            g_lastObservedCell = 0;
            g_leftoverSweptCell = 0;
            g_exitZoneLatchCell = 0;
            g_exitZoneRetryCooldownSec = 0.0;
            g_exitZoneMessageCooldownSec = 0.0;
            g_exitDoorsCheckedCell = 0;
            g_sweepDisableRequested.clear();
            g_fallSkipLogged = false;
            g_endCheckAfterLoad = false;
        }

        if (atlasDamaged) {
            ShowAtlasDamageOnMainThread();
        }
        if (player) {
            std::optional<IncompatibleSavedRun> cancelled;
            std::optional<bool> refunded;
            SaveLoad::RunSerializedTransaction([&]() {
                {
                    std::scoped_lock lk(g_mutex);
                    cancelled = std::exchange(g_incompatibleSavedRun, std::nullopt);
                }
                // 이미 보스를 처치했거나 실패한 회차는 입장 비용을 모두 사용했다.
                if (cancelled && !cancelled->state.bossKilled && !cancelled->state.failed) {
                    const auto& snap = cancelled->state;
                    refunded = RefundEntryOnMainThread(player, snap.tier, snap.isArena4, RecreateSavedMapRefund(snap));
                }
            });
            if (cancelled) {
                const auto& snap = cancelled->state;
                const auto& pack = MapPackRuntime::Manifest();
                const auto* dungeon = pack.Find(snap.dungeonKey);
                auto* savedDungeonCell = dungeon ? MapPackRuntime::ResolveCell(*dungeon) : nullptr;
                const bool inLegacyCell = cell && cancelled->legacyCell != 0 && cell->GetFormID() == cancelled->legacyCell;
                const bool inSavedDungeon = cell && cell == savedDungeonCell;
                // 키를 찾았어도 그 키의 셀이 바뀌었을 수 있으므로 팩 소속 셀 검사를 함께 한다.
                // 현재 셀을 모르면 출구 없는 셀에 남지 않도록 귀환시킨다.
                const bool returnPlayer = !cell || inSavedDungeon || inLegacyCell || MapPackRuntime::OwnsCell(cell);
                const auto& packError = MapPackRuntime::UnavailableReason();
                const auto reason = packError.empty()
                    ? std::string("저장된 던전 키나 팩 판이 현재 지도 팩과 달라 회차를 종료했습니다.")
                    : std::format("{} 저장된 회차를 종료했습니다.", packError);
                auto* legacyCell = cancelled->legacyCell ? RE::TESForm::LookupByID<RE::TESObjectCELL>(cancelled->legacyCell) : nullptr;
                auto* resetCell = MapPackRuntime::OwnsCell(legacyCell) ? legacyCell : savedDungeonCell;
                if (!MapPackRuntime::OwnsCell(resetCell)) {
                    resetCell = nullptr;
                    SKSE::log::warn("MapDeviceRuntime: cancelled run {} had no resolvable map pack cell to reset.", snap.runId);
                }
                FinishCancelledRunOnMainThread(player, snap, returnPlayer, refunded, reason, resetCell ? resetCell->GetFormID() : 0);
            }
        }

        const auto observedBossId = g_activeBossFormId.load(std::memory_order_acquire);
        const auto boss = LookupMapBoss(observedBossId, g_activeCellFormId.load(std::memory_order_acquire));
        const bool bossDeadNow = boss && boss->IsDead();
        bool restoredBossKill = false;
        bool isArena4 = false;
        RE::FormID bossId = 0;
        std::uint8_t arenaPhase = Core::kArena4FirstPhase;
        std::uint64_t runId = 0;
        Core::MapModSummary summaryCopy{};
        bool endRunAfterLoad = false;
        {
            std::scoped_lock lk(g_mutex);
            if (g_activeMap.active) {
                const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(g_activeMap));
                // 결정 176 ④: 저장 때 허가된 입장이었던 경우만 허가를 되살린다. 셀을 아직 못 읽었어도 허가를 두고,
                // 다른 셀로 확인되면 Tick의 허가 판정이 푼다.
                const bool authorized = forms && g_activeMap.playerAuthorized;
                g_permittedCell = authorized ? forms->cellId : 0;
                const bool inCell = authorized && cell == forms->cell;
                g_playerInActiveMapCell.store(inCell, std::memory_order_release);
                if (bossDeadNow && observedBossId == g_activeMap.bossRefFormId &&
                    !g_activeMap.failed && !g_activeMap.bossKilled) {
                    g_activeMap.bossKilled = true;
                    restoredBossKill = true;
                }
                // 끝나야 할 회차인데 플레이어가 이미 지도 팩 밖이면, 귀환 확정 전에 저장된 것이다.
                // 셀을 아직 못 읽었으면 Tick이 셀을 확인한 뒤 같은 판단을 한다.
                endRunAfterLoad = cell && !inPackCell && Core::MapSystem::ShouldEndOnReturn(g_activeMap);
                g_endCheckAfterLoad = !cell;
                isArena4 = g_activeMap.isArena4;
                runId = g_activeMap.runId;
                bossId = g_activeMap.bossRefFormId;
                arenaPhase = g_completedArenaPhase;
                summaryCopy = g_activeMap.summary;
            } else {
                g_playerInActiveMapCell.store(false, std::memory_order_release);
            }
            RefreshAtomicsLocked();
            // 결정 177 ①: 초기화 대기는 세이브에 남지 않으므로, 불러온 뒤 지금 쓰는 회차 셀을 뺀 팩 셀을 모두 다시 초기화한다.
            // 쓰지 않던 셀의 초기화는 해가 없고, 저장 경계에서 대기를 잃어 이전 회차의 상태가 남는 것을 막는다.
            const auto activeCell = g_activeCellFormId.load(std::memory_order_relaxed);
            for (const auto cellId : MapPackRuntime::DungeonCellIds()) {
                if (cellId != activeCell) {
                    QueueCellResetLocked(cellId);
                }
            }
        }

        if (restoredBossKill) {
            SKSE::log::info("MapDeviceRuntime: restored boss kill without rewards for run {} (boss 0x{:08X}).",
                runId, bossId);
            RE::SendHUDMessage::ShowHUDMessage("[지도 장치] 저장된 보스 처치를 반영했습니다. 보상 없음.");
        }
        if (endRunAfterLoad) {
            EndRunOnMainThread();
            SKSE::log::info("MapDeviceRuntime: ended a run saved outside the pack with a pending end condition.");
        } else {
            if (bossId == observedBossId) {
                PromoteMapBoss(boss.get(), isArena4, arenaPhase, summaryCopy);
            }
            SyncMapLayer2ToStatEngine();
        }
        {
            std::scoped_lock lk(g_mutex);
            g_runDamageEscapeReady = true;
        }
    }

    void MapDeviceRuntime::ResetOnRevert()
    {
        std::scoped_lock lk(g_mutex);
        ClearTravelLocked();
        g_runSpawnDisableRequests.clear();
        g_permittedCell = 0;
        g_warnedUnauthorizedCell = 0;
        g_retryReturnRunId = 0;
        g_pendingActorDeletes.clear();
        ++g_pendingActorDeleteGeneration;
        g_pendingCellResets.clear();
        g_sentCellResets.clear();
        g_completedCellResets.clear();
        g_fallRecoveryCooldownSec = 0.0;
        g_fallRecoveryStreak = 0;
        g_lastObservedCell = 0;
        g_leftoverSweptCell = 0;
        g_exitZoneLatchCell = 0;
        g_exitZoneRetryCooldownSec = 0.0;
        g_exitZoneMessageCooldownSec = 0.0;
        g_exitDoorsCheckedCell = 0;
        g_sweepDisableRequested.clear();
        g_fallSkipLogged = false;
        g_endCheckAfterLoad = false;
        g_atlasDamagedAfterLoad = false;
        g_runDamagedAfterLoad = false;
        g_runDamageEscapeReady = false;
        g_incompatibleSavedRun.reset();
        SetActiveMapLocked({});
        g_spawnedForCurrentRun = false;
        g_pendingSpawnRunId = 0;
        g_arenaPhaseInvulnSec = 0.0;
        g_deathEscapeExitElapsedSec = 0.0;
        g_deathEscapeRunId.store(0, std::memory_order_release);
        DisarmUninstallCleanup();
        RefreshAtomicsLocked();
    }
}
