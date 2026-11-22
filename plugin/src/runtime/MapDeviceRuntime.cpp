#include "PCH.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/CycloneRuntime.h"
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
        };
        std::deque<PendingActorDelete> g_pendingActorDeletes;
        std::uint64_t g_pendingActorDeleteGeneration = 0;
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
        RE::FormID g_permittedCell = 0;
        RE::FormID g_warnedUnauthorizedCell = 0;
        std::uint64_t g_retryReturnRunId = 0;
        // 결정 177 ①: Reset은 VM이 호출을 마쳤다는 콜백이 와야 대기에서 뺀다. 접수만으로는 새 회차를 열지 않는다.
        struct PendingCellReset
        {
            std::uint64_t seq = 0;       // 불러오기 뒤 늦게 온 옛 콜백이 새 대기를 지우지 않게 구분한다
            bool warned = false;         // 전송 실패 경고를 한 번만 남긴다
            bool inFlight = false;       // VM이 접수했고 콜백을 기다리는 중
            double waitedSec = 0.0;      // 대기에 들어온 뒤 지난 시간(일시정지·로딩 제외)
            double inFlightSec = 0.0;    // 접수 뒤 지난 시간(일시정지·로딩 제외)
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
        double g_fallRecoveryCooldownSec = 0.0;
        std::size_t g_fallRecoveryCount = 0;
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
        struct ArenaBossDeathStart
        {
            std::uint64_t runId;
            Core::Arena4BossDeathState state;
        };
        std::optional<ArenaBossDeathStart> g_arenaBossDeathStart;
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
            context.runFailed = g_activeMap.failed;
            context.isBoss = actorId == g_activeMap.bossRefFormId;
            context.rewardBlocked = playerCellId == 0 || playerCellId != g_permittedCell ||
                !g_playerInActiveMapCell.load(std::memory_order_acquire);
            if (context.isBoss && g_activeMap.isArena4) {
                context.arenaBossDeath = g_arenaBossDeathStart && g_arenaBossDeathStart->runId == context.runId
                    ? g_arenaBossDeathStart->state
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
                g_bossDeathPendingRunId = 0;
                g_bossDeathConfirmedRunId = 0;
                g_firstBossDeathReturnDeferral.reset();
                g_pendingReturnConfirmation.reset();
                g_completedArenaPhase = Core::kArena4FirstPhase;
                g_arenaBossDeathStart.reset();
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

        void DisableRunSpawnsOnMainThread(std::uint64_t runId);

        bool IssueTravelMove(RE::PlayerCharacter* player, const Core::MapTravelTarget& target,
            std::uint64_t endingRunId = 0)
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
            // 이동 목표 검증과 요청 접수 뒤, 엔진이 현재 셀을 떼기 전에 처리한다.
            if (endingRunId != 0) {
                DisableRunSpawnsOnMainThread(endingRunId);
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

        std::string_view DispatchCellReset(RE::TESObjectCELL* cell, std::uint64_t seq)
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
                const auto failure = DispatchCellReset(cell, seq);
                std::scoped_lock lk(g_mutex);
                const auto pending = g_pendingCellResets.find(cellId);
                if (pending == g_pendingCellResets.end() || pending->second.seq != seq) {
                    continue;
                }
                if (!failure.empty()) {
                    if (!std::exchange(pending->second.warned, true)) {
                        SKSE::log::warn("MapDeviceRuntime: Cell.Reset dispatch failed for 0x{:08X} ({}); will retry.", cellId, failure);
                    }
                    continue;
                }
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

        MapActionResult StartTravel(RE::PlayerCharacter* player, MapTravelKind kind,
            const Core::MapTravelTarget& target, const TravelContext& context, std::string message)
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
            bool sync = false;
            std::uint64_t endingRunId = 0;
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
                    if (Core::MapSystem::ShouldEndOnReturn(g_activeMap)) {
                        endingRunId = context.runId;
                    }
                }
            }
            if (sync) {
                MapDeviceRuntime::SyncMapLayer2ToStatEngine();
            }
            if (!IssueTravelMove(player, target, endingRunId)) {
                std::scoped_lock lk(g_mutex);
                g_travel.Clear();
                g_travelContext = {};
                return { false, "이동을 요청하지 못했습니다. 다시 시도하세요." };
            }
            return { true, std::move(message) };
        }

        MapActionResult RequestReturn(RE::PlayerCharacter* player, MapTravelKind kind,
            const Core::MapTravelTarget& saved, std::uint64_t runId, std::string message)
        {
            const auto target = kind == MapTravelKind::SafeEscape ? g_safeReturnTarget : ResolveReturnTarget(saved);
            if (!target) {
                return { false, "안전 귀환 마커를 찾을 수 없어 이동을 요청하지 못했습니다. 다시 시도하세요." };
            }
            return StartTravel(player, kind, *target, { .runId = runId }, std::move(message));
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

        void ResetMapSpawnTracking(RE::FormID actorId)
        {
            AilmentRuntime::OnActorReset(actorId);
            MonsterRuntime::OnActorReset(actorId);
        }

        // 결정 178 ①: 비활성화할 때도 우리 스폰임을 입증한다. FormID가 재사용돼 다른 액터(소환수 등)를
        // 끄거나 지우지 않도록, 그 회차 셀에 있고 기본 객체가 그 던전의 스폰 원형인 액터만 끈다.
        bool DisableMapSpawnOnMainThread(RE::FormID actorId, std::uint64_t runId, RE::FormID runCellId,
            std::span<const RE::FormID> templateIds)
        {
            if ((actorId & kDynamicFormMask) != kDynamicFormMask || runCellId == 0 || templateIds.empty()) {
                return false;
            }
            std::uint64_t generation = 0;
            {
                std::scoped_lock lk(g_mutex);
                if (std::ranges::any_of(g_pendingActorDeletes, [actorId, runId](const PendingActorDelete& entry) {
                        return entry.actorId == actorId && entry.runId == runId;
                    })) {
                    return false;
                }
                generation = g_pendingActorDeleteGeneration;
            }
            const RE::NiPointer<RE::Actor> actor{ RE::TESForm::LookupByID<RE::Actor>(actorId) };
            if (!actor || actor->IsPlayerRef() || actor->IsDeleted()) {
                return false;
            }
            const auto* cell = actor->GetParentCell();
            const auto* baseObject = actor->GetBaseObject();
            const RE::FormID actorBaseId = baseObject ? baseObject->GetFormID() : 0;
            if (!cell || cell->GetFormID() != runCellId ||
                std::ranges::find(templateIds, actorBaseId) == templateIds.end()) {
                return false;
            }
            // 사망한 액터의 희귀도와 상태이상은 예약된 처치 처리가 읽고 정리한다.
            if (!actor->IsDead()) {
                ResetMapSpawnTracking(actorId);
            }
            actor->Disable();

            RE::FormID evictedActorId = 0;
            {
                std::scoped_lock lk(g_mutex);
                if (generation != g_pendingActorDeleteGeneration) {
                    return false;
                }
                // 다른 회차의 같은 FormID는 번호가 재사용된 것이므로 새 대기 항목으로 바꾼다.
                std::erase_if(g_pendingActorDeletes, [actorId](const PendingActorDelete& entry) { return entry.actorId == actorId; });
                if (g_pendingActorDeletes.size() >= kMaxPendingMapActorDeletes) {
                    evictedActorId = g_pendingActorDeletes.front().actorId;
                    g_pendingActorDeletes.pop_front();
                }
                g_pendingActorDeletes.push_back({ actorId, runId, actorBaseId, runCellId, std::nullopt, 0.0 });
            }
            if (evictedActorId != 0) {
                ResetMapSpawnTracking(evictedActorId);
                SKSE::log::warn("MapDeviceRuntime: dropped pending deletion of actor 0x{:08X} at queue capacity {}.",
                    evictedActorId, kMaxPendingMapActorDeletes);
            }
            return true;
        }

        void DisableRunSpawnsOnMainThread(std::uint64_t runId)
        {
            std::vector<RE::FormID> spawnedIds;
            RE::FormID cellId = 0;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_activeMap.runId != runId) {
                    return;
                }
                spawnedIds = Core::MapSystem::MapSpawnCleanupIds(g_activeMap);
                cellId = g_activeCellFormId.load(std::memory_order_relaxed);
                g_activeMap.spawnsDisabled = true;
                g_bossMissingSec = 0.0;
            }
            const auto templateIds = SpawnTemplateIdsForCell(cellId);
            for (const auto id : spawnedIds) {
                DisableMapSpawnOnMainThread(id, runId, cellId, templateIds);
            }
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
                if (actor && !cell && !observed.isPlayer && observed.isDisabled && observed.actorBaseId == pending.actorBaseId) {
                    // 원형이 맞는 비활성 액터만 셀이 다시 읽힐 때까지 기다린다.
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
                    actor->SetDelete(true);
                } else {
                    SKSE::log::warn("MapDeviceRuntime: skipped deleting actor 0x{:08X}: actor={}, player={}, disabled={}, base=0x{:08X}/0x{:08X}, cell=0x{:08X}/0x{:08X}.",
                        pending.actorId, observed.isActor, observed.isPlayer, observed.isDisabled,
                        observed.actorBaseId, pending.actorBaseId, observed.cellId, pending.runCellId);
                }
                ResetMapSpawnTracking(pending.actorId);
            }
        }

        void CleanupClosedRunOnMainThread(std::span<const RE::FormID> spawnedIds, std::uint64_t runId, RE::FormID cellId)
        {
            WorldDropTracker::DiscardDropsFromCorpses(spawnedIds, cellId);
            // 목록 밖의 보스도 정리하되, 삭제 전에 원형과 셀을 확인한다.
            const auto templateIds = SpawnTemplateIdsForCell(cellId);
            for (const auto id : spawnedIds) {
                DisableMapSpawnOnMainThread(id, runId, cellId, templateIds);
            }
            if (cellId != 0) {
                std::scoped_lock lk(g_mutex);
                QueueCellResetLocked(cellId);
            }
        }

        void FinishCancelledRunOnMainThread(
            RE::PlayerCharacter* player, const Core::ActiveMapState& snap, bool returnPlayer,
            std::optional<bool> refunded, std::string_view reason, RE::FormID cellId)
        {
            // refunded: nullopt = 돌려줄 대상이 아님(보스를 처치했거나 실패한 회차), true/false = 환불 성공 여부
            CleanupClosedRunOnMainThread(Core::MapSystem::MapSpawnCleanupIds(snap), snap.runId, cellId);
            const std::string_view refundText = !refunded.has_value()
                ? "이미 끝난 회차(보스 처치 또는 실패)라 소모품은 돌려주지 않습니다."
                : (*refunded ? (snap.isArena4 ? "공물을 돌려받았습니다." : "지도를 돌려받았습니다.")
                             : "소모품을 돌려주지 못했습니다.");
            const auto msg = std::format("{} {}", reason, refundText);
            SKSE::log::warn("MapDeviceRuntime: cancelled run {}. {}", snap.runId, msg);
            RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
            if (returnPlayer) {
                ShowMapMessage(RequestReturn(player, MapTravelKind::CancelledRun, SavedReturn(snap), 0, "귀환을 요청했습니다."));
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
                DisableMapSpawnOnMainThread(actorId, runId, cell->GetFormID(), spawnedTemplate);
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
        }

        void EndRunOnMainThread()
        {
            std::vector<std::uint32_t> toClean;
            RE::FormID cellId = 0;
            std::uint64_t runId = 0;
            {
                std::scoped_lock lk(g_mutex);
                cellId = g_activeCellFormId.load(std::memory_order_relaxed);
                runId = g_activeMap.runId;
                toClean = Core::MapSystem::MapSpawnCleanupIds(g_activeMap);
                SetActiveMapLocked({});
                g_spawnedForCurrentRun = false;
                g_pendingSpawnRunId = 0;
                g_arenaPhaseInvulnSec = 0.0;
                g_permittedCell = 0;
                g_retryReturnRunId = 0;
                RefreshAtomicsLocked();
            }
            CleanupClosedRunOnMainThread(toClean, runId, cellId);
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
                        g_arenaBossDeathStart.reset();
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

        void AdvanceTravel(RE::PlayerCharacter* player, const Core::MapTravelObservation& observation,
            double deltaSec, bool gamePaused)
        {
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
                g_travel.Clear();
                g_travelContext = {};
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
                if (alreadyReturning) {
                    // 이미 접수된 귀환이 사망 실패로 회차를 끝내게 된 경우도 스폰을 정리한다.
                    DisableRunSpawnsOnMainThread(runId);
                }
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

        class MapReturnDoorSink final : public RE::BSTEventSink<RE::TESActivateEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent* event,
                RE::BSTEventSource<RE::TESActivateEvent>*) override
            {
                if (event && event->actionRef && event->actionRef->IsPlayerRef() && event->objectActivated && g_queue) {
                    const auto generation = g_queue->CurrentGeneration();
                    const auto cellId = MapPackRuntime::ReturnDoorCell(event->objectActivated->GetFormID());
                    if (cellId != 0) {
                        const auto runId = MapDeviceRuntime::GetActiveRunId();
                        g_queue->EnqueueForGeneration(generation, [cellId, runId]() {
                            if (MapDeviceRuntime::GetActiveRunId() != runId) {
                                return;
                            }
                            auto* player = RE::PlayerCharacter::GetSingleton();
                            const auto observation = ObservePlayer(player);
                            if (observation.cell != cellId || observation.loading) {
                                return;
                            }
                            const auto status = MapDeviceRuntime::GetTravelStatus(player);
                            if (!status.kind) {
                                // 안전 제거 정리 뒤에도 갇히지 않도록 안전 탈출은 허용한다.
                                ShowMapMessage(status.unauthorized || MapDeviceRuntime::IsUninstallCleanupArmed()
                                    ? MapDeviceRuntime::SafeEscape(player) : MapDeviceRuntime::ReturnPortal(player));
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
                    AdvanceCellResetTimersLocked(deltaSec);
                }
                if (cellStable && observation.inPackCell) {
                    const auto* dungeon = MapPackRuntime::FindDungeonByCell(observation.cell);
                    belowMap = dungeon && Core::HasFallenBelowMap(observation, dungeon->navmeshMinZ, g_travelSettings.fallMargin);
                    if (!belowMap) {
                        g_fallRecoveryStreak = 0;
                    } else if (!pending && g_fallRecoveryCooldownSec == 0.0 &&
                               g_deathEscapeRunId.load(std::memory_order_acquire) == 0) {
                        fallDungeon = dungeon;
                        fallAuthorized = access.inActiveMap;
                        fallUsePortal = ++g_fallRecoveryStreak >= 2;
                        g_fallRecoveryCooldownSec = kFallRecoveryCooldownSec;
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
                shouldSpawnNow = access.inActiveMap && observation.loaded3D && !pending && !belowMap && !g_spawnedForCurrentRun &&
                    g_pendingSpawnRunId != 0 && g_pendingSpawnRunId == g_activeMap.runId;
            }
        }

        if (accessChanged) {
            SyncMapLayer2ToStatEngine();
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
        if (warnUnauthorized) {
            RE::SendHUDMessage::ShowHUDMessage("입장 허가가 없습니다. 귀환 문이나 지도 탭의 안전 탈출을 이용하세요.");
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

    std::size_t MapDeviceRuntime::GetFallRecoveryCount()
    {
        std::scoped_lock lk(g_mutex);
        return g_fallRecoveryCount;
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
                if (g_activeMap.failed || g_activeMap.bossKilled) {
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
        return g_uninstallCleanupStatus.load(std::memory_order_acquire) != CleanupStatus::Idle;
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

    Core::MapActorScaling MapDeviceRuntime::GetMapActorScaling(RE::FormID actorId, Core::MapModSummary* mods)
    {
        std::scoped_lock lk(g_mapActorsMutex);
        const auto actor = g_mapActors.Find(actorId);
        if (mods) {
            *mods = actor ? g_mapActorMods : Core::MapModSummary{};
        }
        return actor.value_or(Core::MapActorScaling{});
    }

    void MapDeviceRuntime::NoteArena4BossDeathStart(RE::FormID actorId)
    {
        if (!IsActiveArena4Boss(actorId)) {
            return;
        }
        std::scoped_lock lk(g_mutex);
        if (g_activeMap.active && !g_activeMap.failed && g_activeMap.isArena4 && !g_activeMap.bossKilled &&
            g_activeMap.bossRefFormId == actorId && !g_arenaBossDeathStart) {
            g_arenaBossDeathStart = ArenaBossDeathStart{
                g_activeMap.runId, { g_completedArenaPhase, IsArena4PhaseInvulnerableLocked() }
            };
        }
    }

    void MapDeviceRuntime::ClearArena4BossDeathStart(RE::FormID actorId)
    {
        if (!IsActiveArena4Boss(actorId)) {
            return;
        }
        std::scoped_lock lk(g_mutex);
        if (g_activeMap.active && g_activeMap.isArena4 && g_activeMap.bossRefFormId == actorId) {
            g_arenaBossDeathStart.reset();
        }
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
                return std::nullopt;
            }
            // 결정 175·179: 허가와 관계없이 보스 처리가 끝날 때까지 귀환 확정을 미룬다.
            if (!g_activeMap.failed) {
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
            g_travel.Clear();
            g_travelContext = {};
            g_pendingReturnConfirmation.reset();
            g_bossDeathPendingRunId = 0;
            g_bossDeathConfirmedRunId = 0;
            g_firstBossDeathReturnDeferral.reset();
            g_arenaBossDeathStart.reset();
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
        g_travel.Clear();
        g_travelContext = {};
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
