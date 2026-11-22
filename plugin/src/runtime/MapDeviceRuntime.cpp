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
        constexpr double kArena4PhaseInvulnSec = 0.8;
        constexpr double kFallRecoveryCooldownSec = 3.0;
        constexpr double kMapActorDeleteDelaySec = 3.0;
        constexpr double kMapActorDeleteRetrySec = 1.0;
        // 이만큼(일시정지·로딩 제외) 조회되지 않으면 삭제를 포기한다. 비활성 상태로 남고 셀 초기화에 맡긴다.
        constexpr double kMapActorDeleteGiveUpSec = 300.0;
        constexpr std::size_t kMaxPendingMapActorDeletes = 512;

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
        // 입구로 옮긴 뒤에도 다음 판정에서 여전히 문턱 아래면 포털 귀환으로 바꾼다.
        std::uint32_t g_fallRecoveryStreak = 0;
        double g_fallRecoveryCooldownSec = 0.0;
        std::size_t g_fallRecoveryCount = 0;
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
        MapKillContext g_mapKillContext{};
        RE::FormID g_mapActorBossId = 0;
        bool g_spawnedForCurrentRun = false;
        std::uint64_t g_nextRunId = 1;
        std::uint64_t g_pendingSpawnRunId = 0;
        double g_arenaPhaseInvulnSec = 0.0;
        double g_deathEscapeExitElapsedSec = 0.0;

        // 락프리 원자 상태(CombatHooks 워커 스레드 안전 조회용)
        std::atomic_bool g_playerInActiveMapCell{ false };
        std::atomic_bool g_activeMapRun{ false };
        std::atomic<RE::FormID> g_activeCellFormId{ 0 };
        std::atomic<std::uint8_t> g_activeMapTier{ 0 };
        std::atomic<RE::FormID> g_activeBossFormId{ 0 };
        std::atomic_bool g_activeIsArena4{ false };
        std::atomic_bool g_deathEscapeInFlight{ false };
        std::atomic_bool g_uninstallCleanupArmed{ false };
        // 보상 자격이 있는 보스 사망을 사망 이벤트 때 표시한다. 큐에 있는 처치 처리보다 귀환 확정이 먼저 오지 않게 한다.
        std::atomic<std::uint64_t> g_bossDeathPendingRunId{ 0 };
        std::atomic<std::uint64_t> g_bossDeathConfirmedRunId{ 0 };
        std::optional<std::chrono::steady_clock::time_point> g_firstBossDeathReturnDeferral;

        void RefreshMapActorsLocked()
        {
            std::scoped_lock lk(g_mapActorsMutex);
            if (g_activeMap.active && g_dataPack) {
                g_mapActors.Rebuild(g_activeMap, *g_dataPack);
                g_mapActorMods = g_activeMap.summary;
                g_mapKillContext.runId = g_activeMap.runId;
                g_mapKillContext.rewardMonsterLevel = Core::MapSystem::RewardMonsterLevel(g_activeMap);
                g_mapKillContext.dropMapTier = g_activeMap.isArena4 ? std::uint8_t{ 0 } : g_activeMap.tier;
                g_mapKillContext.bonusIiqPct = g_activeMap.summary.bonusIiqPct;
                g_mapKillContext.bonusIirPct = g_activeMap.summary.bonusIirPct;
                g_mapKillContext.runFailed = g_activeMap.failed;
                g_mapActorBossId = g_activeMap.bossRefFormId;
            } else {
                g_mapActors.Clear();
                g_mapActorMods = {};
                g_mapKillContext = {};
                g_mapActorBossId = 0;
            }
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
                g_bossDeathPendingRunId.store(0, std::memory_order_release);
                g_bossDeathConfirmedRunId.store(0, std::memory_order_release);
                g_firstBossDeathReturnDeferral.reset();
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
            bool bossDeadNow = false;
            if (bossId != 0) {
                const RE::NiPointer<RE::Actor> boss{ RE::TESForm::LookupByID<RE::Actor>(bossId) };
                bossDeadNow = boss && boss->IsDead();
            }
            {
                std::scoped_lock lk(g_mutex);
                const bool currentRun = context.runId != 0 && g_activeMap.active && g_activeMap.runId == context.runId;
                if (kind == MapTravelKind::Return && currentRun && !g_travel.Current()) {
                    const auto now = std::chrono::steady_clock::now();
                    const double waitedSec = std::chrono::duration<double>(
                        now - g_firstBossDeathReturnDeferral.value_or(now)).count();
                    const bool deathConfirmed = g_bossDeathConfirmedRunId.load(std::memory_order_acquire) == context.runId;
                    const bool killPending = g_bossDeathPendingRunId.load(std::memory_order_acquire) == context.runId;
                    if (Core::MapSystem::ShouldDeferReturnForBossDeath(g_activeMap.bossKilled,
                            bossDeadNow && bossId == g_activeMap.bossRefFormId, deathConfirmed, killPending, waitedSec)) {
                        if (!g_firstBossDeathReturnDeferral) {
                            g_firstBossDeathReturnDeferral = now;
                        }
                        return { false, "보스 처치를 처리하는 중입니다. 잠시 후 다시 귀환하세요." };
                    }
                }
                if (!g_travel.TryStart(request)) {
                    return { false, "이동 중입니다. 도착 후 다시 시도하세요." };
                }
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
            if (!tracked) {
                player->AddObjectToContainer(item, nullptr, 1, nullptr);
                return true;
            }

            // 재검수 R-15: 추적에 실패해도 원래 소유한 지도는 플레이어에게 남긴다.
            bool untrackedLeft = false;
            if (auto* extra = UniqueIdUtil::CreateGameExtraDataList()) {
                const auto key = ItemTracker::AddObjectWithTrackedUniqueId(player, item, extra, nullptr,
                    ItemTracker::UntrackedAddPolicy::Keep, &untrackedLeft);
                if (key.IsValid()) {
                    auto record = *tracked;
                    record.baseFormId = item->GetFormID();
                    ItemTracker::Upsert(key, record, false);
                    return true;
                }
            }
            if (!untrackedLeft) {
                player->AddObjectToContainer(item, nullptr, 1, nullptr);
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
        void DisableMapSpawnOnMainThread(RE::FormID actorId, std::uint64_t runId, RE::FormID runCellId,
            std::span<const RE::FormID> templateIds)
        {
            if ((actorId & 0xFF000000u) != 0xFF000000u || runCellId == 0 || templateIds.empty()) {
                return;
            }
            std::uint64_t generation = 0;
            {
                std::scoped_lock lk(g_mutex);
                if (std::ranges::any_of(g_pendingActorDeletes, [actorId, runId](const PendingActorDelete& entry) {
                        return entry.actorId == actorId && entry.runId == runId;
                    })) {
                    return;
                }
                generation = g_pendingActorDeleteGeneration;
            }
            const RE::NiPointer<RE::Actor> actor{ RE::TESForm::LookupByID<RE::Actor>(actorId) };
            if (!actor || actor->IsPlayerRef() || actor->IsDeleted()) {
                return;
            }
            const auto* cell = actor->GetParentCell();
            const auto* baseObject = actor->GetBaseObject();
            const RE::FormID actorBaseId = baseObject ? baseObject->GetFormID() : 0;
            if (!cell || cell->GetFormID() != runCellId ||
                std::ranges::find(templateIds, actorBaseId) == templateIds.end()) {
                return;
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
                    return;
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
                spawnedIds = g_activeMap.spawnedRefFormIds;
                cellId = g_activeCellFormId.load(std::memory_order_relaxed);
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
            // 보스도 스폰 목록에 있다. 목록 밖의 ID는 손상됐거나 재사용됐을 수 있다.
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
            CleanupClosedRunOnMainThread(snap.spawnedRefFormIds, snap.runId, cellId);
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
                        g_mapActorBossId = actorId;
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
            auto* boss = RE::TESForm::LookupByID<RE::Actor>(bossFormId);
            if (!boss || boss->IsDead() || boss->IsDisabled()) {
                return;
            }
            {
                SpawnHarmlessVfxAtActor(boss, FormIds::kVfxFireBurst);
                const std::uint16_t phaseMods = (newPhase == 2)
                    ? (static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted)) |
                       static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::ExtraFire)))
                    : (static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted)) |
                       static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Deadly)) |
                       static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::ExtraLightning)));
                MonsterRuntime::ForcePromoteForTest(boss, Core::MonsterRarity::Rare, phaseMods, -1.0);
            }

            const auto* arena = MapPackRuntime::FindForms(&MapPackRuntime::Manifest().arena);
            if (!arena) {
                return;
            }
            std::size_t addedCount = 0;
            for (std::size_t i = 0; i < 2 && i < arena->packAnchors.size(); ++i) {
                const RE::NiPoint3 off{ (i == 0 ? 40.0f : -40.0f), 30.0f, 0.0f };
                if (SpawnMapNpcAtAnchor(player, arena->packAnchors[i], arena->addNpcs[i % arena->addNpcs.size()], off, runId)) {
                    ++addedCount;
                }
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
                        g_deathEscapeInFlight.store(false, std::memory_order_release);
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
                toClean.swap(g_activeMap.spawnedRefFormIds);
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
            g_deathEscapeInFlight.store(false, std::memory_order_release);
        }

        bool CompleteEntry(RE::PlayerCharacter* player, const Core::MapTravelRequest& request, const TravelContext& context)
        {
            const auto kind = request.kind;
            const auto& origin = context.entryOrigin;
            std::string message;
            if (kind == MapTravelKind::Reenter) {
                std::scoped_lock lk(g_mutex);
                if (g_activeMap.runId != context.runId || !Core::MapSystem::TryConsumePortalForReenter(g_activeMap)) {
                    return false;
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
                    return false;
                }

                bool committed = false;
                SaveLoad::RunSerializedTransaction([&]() {
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
                    auto next = arena
                        ? Core::MapSystem::CreateArena4Run(g_nextRunId++, origin.cell, origin.worldspace, origin.position, origin.angle)
                        : Core::MapSystem::CreateNormalMapRun(g_nextRunId++, *tier,
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
                    return false;
                }
            }

            RE::SendHUDMessage::ShowHUDMessage(message.c_str());
            return true;
        }

        void CompleteReturn(MapTravelKind kind, std::uint64_t runId)
        {
            if (runId != 0 && g_bossDeathPendingRunId.load(std::memory_order_acquire) == runId &&
                g_queue && !MapDeviceRuntime::IsUninstallCleanupArmed()) {
                // 보스 사망 처리(보상·bossKilled)가 큐에 먼저 들어가 있다. 그 뒤로 돌려 결정 175 판정이 그 결과를 보게 한다.
                g_queue->Enqueue([kind, runId]() { CompleteReturn(kind, runId); });
                return;
            }
            if (kind == MapTravelKind::SafeEscape || kind == MapTravelKind::CancelledRun) {
                RE::SendHUDMessage::ShowHUDMessage(kind == MapTravelKind::SafeEscape ? "안전 탈출을 완료했습니다." : "귀환을 완료했습니다.");
                return;
            }
            bool endRun = false;
            std::uint8_t portals = 0;
            bool arena = false;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_activeMap.runId != runId) {
                    return;
                }
                g_retryReturnRunId = 0;
                endRun = kind == MapTravelKind::Abandon || Core::MapSystem::ShouldEndOnReturn(g_activeMap);
                portals = g_activeMap.portalsRemaining;
                arena = g_activeMap.isArena4;
            }
            if (endRun) {
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
                g_deathEscapeExitElapsedSec = 0.0;
                g_deathEscapeInFlight.store(false, std::memory_order_release);
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
                } else if (!CompleteEntry(player, *request, context)) {
                    RE::SendHUDMessage::ShowHUDMessage("입장 비용을 소비하지 못했습니다.");
                    ShowMapMessage(RequestReturn(player, MapTravelKind::CancelledRun, context.entryOrigin, 0, "귀환을 요청했습니다."));
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

        void ExecuteDeathEscapeOnMainThread()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || MapDeviceRuntime::IsUninstallCleanupArmed()) {
                g_deathEscapeInFlight.store(false, std::memory_order_release);
                return;
            }

            Core::MapTravelTarget saved;
            std::uint64_t runId = 0;
            bool alreadyReturning = false;
            bool failedRun = false;
            bool arenaRun = false;
            {
                std::scoped_lock lk(g_mutex);
                const auto& pending = g_travel.Current();
                if (!g_activeMap.active || (pending && Core::IsMapEntry(pending->kind))) {
                    g_deathEscapeInFlight.store(false, std::memory_order_release);
                    return;
                }
                // 사망으로 생긴 포털 변화와 실패는 즉시 반영한다. 회차 종료(정리)는 도착 때 확정하므로
                // 실패한 회차도 귀환 좌표를 쓰도록 활성으로 두되, 보상·재입장은 failed로 막는다.
                auto death = g_activeMap;
                if (!Core::MapSystem::ConsumePortalOnDeathEscape(death)) {
                    g_activeMap.failed = true;
                    failedRun = true;
                    arenaRun = g_activeMap.isArena4;
                    RefreshMapActorsLocked();
                }
                g_activeMap.portalsRemaining = death.portalsRemaining;
                saved = SavedReturn(g_activeMap);
                runId = g_activeMap.runId;
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
                    g_deathEscapeExitElapsedSec = 0.0;
                    g_deathEscapeInFlight.store(false, std::memory_order_release);
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
                        g_queue->EnqueueForGeneration(generation, [cellId]() {
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
                if (g_travel.Current()) {
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
        const Core::MapDungeonDef* fallDungeon = nullptr;
        std::vector<std::pair<RE::FormID, std::uint64_t>> resetCandidates;
        {
            std::scoped_lock lk(g_mutex);
            const auto access = GetEntryAccessLocked(observation);
            g_permittedCell = access.permittedCell;
            accessChanged = g_playerInActiveMapCell.exchange(access.inActiveMap, std::memory_order_acq_rel) != access.inActiveMap;
            const auto& pending = g_travel.Current();
            resetCandidates = CollectCellResetCandidatesLocked(observation);
            if (!observation.loading) {
                if (!access.unauthorized) {
                    g_warnedUnauthorizedCell = 0;
                } else if (observation.cell != g_warnedUnauthorizedCell && !pending) {
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
            bool belowMap = false;
            if (!gamePaused && !observation.loading) {
                const bool validDelta = std::isfinite(deltaSec) && deltaSec > 0.0;
                if (validDelta) {
                    g_fallRecoveryCooldownSec = std::max(0.0, g_fallRecoveryCooldownSec - deltaSec);
                    AdvanceCellResetTimersLocked(deltaSec);
                }
                // 3D가 준비된 같은 셀을 두 틱 연속 본 뒤에만 높이를 믿는다(순간이동 직후 옛 좌표 제외).
                const bool observedReady = observation.loaded3D && observation.cell != 0;
                const bool cellStable = observedReady && observation.cell == g_lastObservedCell;
                g_lastObservedCell = observedReady ? observation.cell : 0;
                if (cellStable && observation.inPackCell) {
                    const auto* dungeon = MapPackRuntime::FindDungeonByCell(observation.cell);
                    belowMap = dungeon && Core::HasFallenBelowMap(observation, dungeon->navmeshMinZ, g_travelSettings.fallMargin);
                    if (!belowMap) {
                        g_fallRecoveryStreak = 0;
                    } else if (!pending && g_fallRecoveryCooldownSec == 0.0 &&
                               !g_deathEscapeInFlight.load(std::memory_order_acquire)) {
                        fallDungeon = dungeon;
                        fallAuthorized = access.inActiveMap;
                        fallUsePortal = ++g_fallRecoveryStreak >= 2;
                        g_fallRecoveryCooldownSec = kFallRecoveryCooldownSec;
                    }
                }
                if (g_deathEscapeInFlight.load(std::memory_order_acquire)) {
                    if (observation.inPackCell || !observation.loaded3D || observation.cell == 0) {
                        g_deathEscapeExitElapsedSec = 0.0;
                    } else if (validDelta) {
                        g_deathEscapeExitElapsedSec += deltaSec;
                        if (g_deathEscapeExitElapsedSec >= kDeathEscapeReleaseCooldownSec) {
                            g_deathEscapeExitElapsedSec = 0.0;
                            g_deathEscapeInFlight.store(false, std::memory_order_release);
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
            RecoverFromMapFall(player, *fallDungeon, fallAuthorized, fallUsePortal);
            return;  // 다음 틱에서 옮긴 위치를 다시 관측한 뒤 스폰한다.
        }
        if (shouldSpawnNow) {
            SpawnMapEncounterOnMainThread(player);
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
                return { false, "재입장 가능한 활성 지도가 없습니다." };
            }
            if (g_activeMap.portalsRemaining == 0) {
                return { false, "남은 포털이 없습니다." };
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
            if (g_travel.Current()) {
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
            if (g_travel.Current()) {
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
        const auto& pending = g_travel.Current();
        return { pending ? std::optional(pending->kind) : std::nullopt, observation.inPackCell,
            GetEntryAccessLocked(observation).unauthorized, g_retryReturnRunId != 0 };
    }

    double MapDeviceRuntime::GetTravelArrivalRadius() noexcept
    {
        return g_travelSettings.arrivalRadius;
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

    MapActionResult MapDeviceRuntime::RunSafeUninstallCleanup(RE::PlayerCharacter* player)
    {
        if (!player) {
            return { false, "플레이어를 찾을 수 없습니다." };
        }
        const auto travel = GetTravelStatus(player);
        if (travel.kind || travel.inPackCell || !player->GetParentCell()) {
            return { false, "이동을 마치고 지도 밖에서 실행하세요." };
        }

        // 회차 드랍은 액터 정리 전에 버리고, 나머지는 안전 제거 절차대로 각 시체에 돌려준다.
        CycloneRuntime::StopNowOnMainThread(player);
        if (g_activeMapRun.load(std::memory_order_acquire)) {
            EndRunOnMainThread();
        }
        WorldDropTracker::ReturnAllToCorpsesNow();
        (void)WorldDropTracker::SweepOrphanBeamsNearPlayer();

        std::size_t returnedGems = 0;
        std::size_t returnedJewels = 0;
        // 돌려주지 못해 소켓에 남긴 주얼 수. 0이 아니면 정리를 완료로 표시하지 않는다(모드를 지우면 잃기 때문).
        std::size_t keptJewels = 0;
        std::size_t returnedCharms = 0;
        double appliedHpDelta = 0.0;
        double appliedMpDelta = 0.0;

        SaveLoad::RunSerializedTransaction([&]() {
            PersistentState st = SaveLoad::GetStateCopy();
            appliedHpDelta = st.appliedHealthDelta;
            appliedMpDelta = st.appliedMagickaDelta;

            for (auto& slot : st.slots) {
                if (!slot.occupied) {
                    continue;
                }
                for (auto& sup : slot.gem.socketedGems) {
                    if (GemInventory::ReturnGemToPlayerInventory(player, sup).has_value()) {
                        ++returnedGems;
                    }
                }
                slot.gem.socketedGems.clear();
                if (GemInventory::ReturnGemToPlayerInventory(player, slot.gem).has_value()) {
                    ++returnedGems;
                }
                slot.occupied = false;
                slot.linkedAttackSlot = -1;
                slot.gem = Core::GemInstance{};
            }

            // 재검수 R-15와 같은 규칙(SaveLoad::PostLoadInit의 주얼 반환): 돌려준 것과 "실물은 들어갔지만 추적 실패"는 소켓에서 빼고,
            // 넣지 못한 것은 소켓에 남겨 다시 시도할 수 있게 한다(6단계 재작업 계획 D-10: 실패분은 보존).
            for (auto it = st.socketedJewels.begin(); it != st.socketedJewels.end();) {
                const auto& jRec = it->second;
                auto* baseObj = RE::TESForm::LookupByID<RE::TESBoundObject>(jRec.baseFormId);
                if (!baseObj) {
                    baseObj = FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(jRec.jewelColor));
                }
                auto* freshExtra = baseObj ? UniqueIdUtil::CreateGameExtraDataList() : nullptr;
                if (baseObj && freshExtra) {
                    bool untrackedLeft = false;
                    const auto newKey = ItemTracker::AddObjectWithTrackedUniqueId(
                        player, baseObj, freshExtra, nullptr, ItemTracker::UntrackedAddPolicy::RemoveAgain, &untrackedLeft);
                    if (newKey.IsValid()) {
                        ItemTracker::Upsert(newKey, jRec, false);
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
                ++keptJewels;
                ++it;
            }

            for (auto& cs : st.charmSlots) {
                if (cs.occupied) {
                    if (auto* charmObj = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CharmLocalFormId(cs.kind))) {
                        player->AddObjectToContainer(charmObj, nullptr, 1, nullptr);
                        ++returnedCharms;
                    }
                }
                cs = Core::CharmSlotState{};
            }
            if (g_charmSystem) {
                g_charmSystem->SetSlots(st.charmSlots, 0);
            }

            st.activeAuraMask = 0;
            st.virtualRing2Key = {};
            st.virtualRing2BaseFormId = 0;
            st.virtualBeltKey = {};
            st.virtualBeltBaseFormId = 0;
            st.appliedHealthDelta = 0.0;
            st.appliedMagickaDelta = 0.0;
            SaveLoad::SetState(st);
        });

        ProxySpellRuntime::RemoveAllProxySpells(player);

        if (auto* av = player->AsActorValueOwner()) {
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

        AilmentRuntime::ClearPlayerAilments();
        AilmentRuntime::RevertAllTrackedSpeedsNow();
        MonsterRuntime::RevertAllHasteSpeedsNow();
        SaveLoad::RevertAllPendingSpeedModsNow();

        if (g_esPool) {
            g_esPool->Clear();
        }
        if (g_statEngine) {
            g_statEngine->SetLayer1(Core::Layer1Input{});
            g_statEngine->SetLayer2(Core::Layer2Input{});
        }

        if (keptJewels > 0) {
            // 정리를 완료로 표시하지 않는다: 재오염 차단을 걸지 않고, 같은 세션에서 다시 시도할 수 있게 둔다(6단계 재작업 계획 D-10).
            const auto failMsg = std::format(
                "안전 제거를 마치지 못했습니다: 주얼 {}개를 돌려주지 못해 소켓에 남았습니다. 모드를 지우지 말고 다시 시도하세요(젬 {}개, 주얼 {}개, 참 {}개는 반환됨).",
                keptJewels, returnedGems, returnedJewels, returnedCharms);
            RE::SendHUDMessage::ShowHUDMessage(failMsg.c_str());
            SKSE::log::error("MapDeviceRuntime: SafeUninstallCleanup incomplete; {} jewel(s) kept in sockets.", keptJewels);
            return { false, failMsg };
        }

        g_uninstallCleanupArmed.store(true, std::memory_order_release);

        const auto msg = std::format(
            "안전 제거 정리 완료: 젬 {}개, 주얼 {}개, 참 {}개 반환 및 체력/매지카/속도 원복 완료. 저장 후 종료해 모드를 제거하세요.",
            returnedGems, returnedJewels, returnedCharms);
        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        SKSE::log::info("MapDeviceRuntime: SafeUninstallCleanup armed (gems={}, jewels={}, charms={}, hpDelta={:.1f}, mpDelta={:.1f}).",
            returnedGems, returnedJewels, returnedCharms, appliedHpDelta, appliedMpDelta);
        return { true, msg };
    }

    void MapDeviceRuntime::OnActorKilled(RE::FormID actorId, std::uint64_t runId)
    {
        if (runId != 0) {
            // 그 회차의 보스(또는 이미 끝난 회차)의 처리일 때만 대기 표시를 푼다. 일반 적의 처리로 풀면
            // 아직 큐에 있는 보스 처리보다 귀환 확정이 먼저 올 수 있다.
            bool clearPending = false;
            {
                std::scoped_lock lk(g_mutex);
                clearPending = !g_activeMap.active || g_activeMap.runId != runId || g_activeMap.bossRefFormId == actorId;
            }
            if (clearPending) {
                auto expected = runId;
                (void)g_bossDeathPendingRunId.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
            }
        }
        if (actorId == 0 || runId == 0 || g_uninstallCleanupArmed.load(std::memory_order_acquire)) {
            return;
        }
        bool isArena4 = false;
        std::uint8_t tier = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active || g_activeMap.failed || g_activeMap.runId != runId ||
                g_activeMap.bossRefFormId != actorId || g_activeMap.bossKilled) {
                return;
            }
            g_activeMap.bossKilled = true;
            isArena4 = g_activeMap.isArena4;
            tier = g_activeMap.tier;
        }
        if (isArena4) {
            bool unlocked4th = false;
            SaveLoad::UpdateState([&](PersistentState& st) {
                if ((st.tree.ascendancyMilestonesBitmask & 0x8u) == 0u) {
                    st.tree.ascendancyMilestonesBitmask |= 0x8u;
                    unlocked4th = true;
                    if (g_treeData && g_treeData->IsLoaded() && g_config) {
                        const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                            *g_treeData, st.tree, st.exileLevel, *g_config, st.atlas.completedTierMask);
                        st.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
                    }
                }
            });
            if (unlocked4th) {
                SaveLoad::SyncStateToStatEngine();
                RE::SendHUDMessage::ShowHUDMessage(
                    "[SkyrimPoE] 4차 전직 결투장 정복! 전직 8/8, 패시브 +2, 기본 최대 정신력 220 해금!");
                SKSE::log::info("MapDeviceRuntime: 4th Ascendancy Arena cleared -> bitmask |= 0x8 (8/8 asc, Spirit 220).");
            } else {
                RE::SendHUDMessage::ShowHUDMessage("[SkyrimPoE] 4차 전직 결투장 보스 처치 완료!");
            }
            return;
        }

        bool firstClear = false;
        std::uint32_t totalAtlasPts = 0;
        SaveLoad::UpdateState([&](PersistentState& st) {
            firstClear = Core::MapSystem::RecordMapClear(st.atlas, tier);
            totalAtlasPts = Core::MapSystem::CountAtlasTierPoints(st.atlas.completedTierMask);
            if (firstClear && g_treeData && g_treeData->IsLoaded() && g_config) {
                const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                    *g_treeData, st.tree, st.exileLevel, *g_config, st.atlas.completedTierMask);
                st.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
            }
        });

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
        double diffMult)
    {
        if (!boss || !std::isfinite(rawSkyrimDamage) || rawSkyrimDamage <= 0.0) {
            return std::max(0.0, std::isfinite(rawSkyrimDamage) ? rawSkyrimDamage : 0.0);
        }
        if (!g_activeIsArena4.load(std::memory_order_acquire) ||
            boss->GetFormID() != g_activeBossFormId.load(std::memory_order_acquire)) {
            return rawSkyrimDamage;
        }
        const double safeDiff = (std::isfinite(diffMult) && diffMult > 1e-4) ? diffMult : 1.0;
        const auto* av = boss->AsActorValueOwner();
        const double curHp = av ? std::max(0.0, static_cast<double>(av->GetActorValue(RE::ActorValue::kHealth))) : 100.0;
        const double maxHp = av ? std::max(1.0, static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth))) : 100.0;

        Core::ArenaDamageClampResult clampRes{};
        const RE::FormID bossId = boss->GetFormID();
        std::uint64_t runId = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active || g_activeMap.failed || !g_activeMap.isArena4 || g_activeMap.bossRefFormId != bossId) {
                return rawSkyrimDamage;
            }
            clampRes = Core::MapSystem::ClampArenaBossDamage(
                g_activeMap.arenaPhase,
                g_arenaPhaseInvulnSec > 0.0,
                curHp,
                maxHp,
                rawSkyrimDamage * safeDiff);
            if (clampRes.triggeredNextPhase != 0) {
                runId = g_activeMap.runId;
                g_activeMap.arenaPhase = clampRes.triggeredNextPhase;
                g_arenaPhaseInvulnSec = kArena4PhaseInvulnSec;
            }
        }
        if (clampRes.triggeredNextPhase != 0 && g_queue) {
            const std::uint8_t nextPhase = clampRes.triggeredNextPhase;
            g_queue->Enqueue([nextPhase, bossId, runId]() {
                ExecuteArena4PhaseTransitionOnMainThread(nextPhase, bossId, runId);
            });
        }
        return std::max(0.0, clampRes.clampedDamage / safeDiff);
    }

    bool MapDeviceRuntime::TryInterceptLethalHitFromHook(
        double curHp,
        double effectiveDamage)
    {
        if (!g_queue || !g_config || !g_config->mapDeathInterceptEnabled) {
            return false;
        }
        const auto generation = g_queue->CurrentGeneration();
        if (g_uninstallCleanupArmed.load(std::memory_order_acquire)) {
            return false;
        }
        if (!g_playerInActiveMapCell.load(std::memory_order_acquire)) {
            return false;
        }
        if (g_deathEscapeInFlight.load(std::memory_order_acquire)) {
            return true;
        }
        if (!std::isfinite(curHp) || !std::isfinite(effectiveDamage) || effectiveDamage <= 0.0) {
            return false;
        }
        if ((curHp - effectiveDamage) <= 1.0) {
            bool expected = false;
            if (g_deathEscapeInFlight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                g_queue->EnqueueForGeneration(generation, []() {
                    ExecuteDeathEscapeOnMainThread();
                });
            }
            return true;
        }
        return false;
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

    bool MapDeviceRuntime::IsDeathEscapeInFlight() noexcept
    {
        return g_deathEscapeInFlight.load(std::memory_order_acquire);
    }

    bool MapDeviceRuntime::IsUninstallCleanupArmed() noexcept
    {
        return g_uninstallCleanupArmed.load(std::memory_order_acquire);
    }

    void MapDeviceRuntime::DisarmUninstallCleanup() noexcept
    {
        g_uninstallCleanupArmed.store(false, std::memory_order_release);
    }

    Core::ActiveMapState MapDeviceRuntime::GetActiveMapStateCopy()
    {
        std::scoped_lock lk(g_mutex);
        return g_activeMap;
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

    MapKillContext MapDeviceRuntime::GetMapKillContext(RE::FormID actorId)
    {
        std::scoped_lock lk(g_mapActorsMutex);
        if (!g_mapActors.Find(actorId)) {
            return {};
        }
        auto context = g_mapKillContext;
        context.isBoss = actorId == g_mapActorBossId;
        // 결정 176 ④: 허가 없이 셀에 들어왔거나(재입장 포털 없이 처치) 실패한 회차의 처치는 지도 보상을 주지 않는다.
        context.rewardBlocked = context.runFailed || !g_playerInActiveMapCell.load(std::memory_order_acquire);
        return context;
    }

    void MapDeviceRuntime::NoteConfirmedMapKill(const MapKillContext& context)
    {
        if (context.runId == 0 || !context.isBoss) {
            return;
        }
        std::scoped_lock lk(g_mutex);
        if (!g_activeMap.active || g_activeMap.runId != context.runId) {
            return;
        }
        // 보상 자격이 있는 사망만 처치 큐가 끝날 때까지 귀환 확정을 미룬다(결정 175).
        if (!context.rewardBlocked) {
            g_bossDeathPendingRunId.store(context.runId, std::memory_order_release);
        }
        g_bossDeathConfirmedRunId.store(context.runId, std::memory_order_release);
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
        // 결정 176 ④: 도착 확인을 거친 허가만 저장한다. 허가 없이 셀에 있던 세이브는 불러와도 허가가 생기지 않는다.
        const auto activeCell = g_activeCellFormId.load(std::memory_order_acquire);
        snapshot.playerAuthorized = g_activeMap.active && activeCell != 0 && g_permittedCell == activeCell;
        return snapshot;
    }

    void MapDeviceRuntime::RestoreFromSave(Core::ActiveMapState saved, RE::FormID legacyCell)
    {
        std::scoped_lock lk(g_mutex);
        g_permittedCell = 0;
        g_playerInActiveMapCell.store(false, std::memory_order_release);
        g_incompatibleSavedRun.reset();
        if (saved.active && saved.runId == 0) {
            // 회차 번호 0은 "회차 밖"을 뜻하므로, 손상된 세이브의 활성 회차에는 새 번호를 준다.
            saved.runId = std::max<std::uint64_t>(g_nextRunId, 1);
            SKSE::log::warn("MapDeviceRuntime: active map run in save had runId 0; assigned {}.", saved.runId);
        }
        if (saved.runId == std::numeric_limits<std::uint64_t>::max()) {
            // 손상된 값이다. +1이 0으로 돌아가면 다음 회차의 스폰 대기 조건(!= 0)이 깨진다.
            saved.runId = std::max<std::uint64_t>(g_nextRunId, 1);
            SKSE::log::warn("MapDeviceRuntime: active map run in save had runId UINT64_MAX; assigned {}.", saved.runId);
        }
        if (saved.runId >= g_nextRunId) {
            g_nextRunId = saved.runId + 1;
        }
        if (saved.active && !MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(saved))) {
            // 결정 169, R4: 호환되지 않는 회차는 로드 중 등록부에 올리지 않고 주 스레드에서 정리한다.
            g_incompatibleSavedRun = IncompatibleSavedRun{ std::move(saved), legacyCell };
            SetActiveMapLocked({});
        } else {
            auto refund = RecreateSavedMapRefund(saved);
            SetActiveMapLocked(std::move(saved), std::move(refund));
        }
        g_spawnedForCurrentRun = g_activeMap.bossKilled || g_activeMap.bossRefFormId != 0 || !g_activeMap.spawnedRefFormIds.empty();
        g_pendingSpawnRunId = (g_activeMap.active && !g_spawnedForCurrentRun) ? g_activeMap.runId : 0;
        g_arenaPhaseInvulnSec = 0.0;
        g_deathEscapeExitElapsedSec = 0.0;
        g_deathEscapeInFlight.store(false, std::memory_order_release);
        RefreshAtomicsLocked();
    }

    void MapDeviceRuntime::OnPostLoadRebind()
    {
        g_uninstallCleanupArmed.store(false, std::memory_order_release);
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        const bool inPackCell = MapPackRuntime::OwnsCell(cell);
        {
            std::scoped_lock lk(g_mutex);
            // 저장하지 않는 요청과 허가는 불러온 세이브에서 새로 시작한다.
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
        }

        if (player) {
            std::optional<IncompatibleSavedRun> cancelled;
            std::optional<bool> refunded;
            SaveLoad::RunSerializedTransaction([&]() {
                {
                    std::scoped_lock lk(g_mutex);
                    cancelled = std::exchange(g_incompatibleSavedRun, std::nullopt);
                }
                // 보스를 처치한 회차는 보상(아틀라스·첫 공물·4차 해금)을 이미 받았으므로 소모품을 돌려주지 않는다.
                // 돌려주면 팩 판이 바뀔 때마다 보상을 받고 지도까지 돌려받는 복제가 생긴다. 실패한 회차도 이미 소모가 끝났다.
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

        bool isArena4 = false;
        RE::FormID bossId = 0;
        std::uint8_t arenaPhase = 1;
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
                // 끝나야 할 회차인데 플레이어가 이미 지도 팩 밖이면, 귀환 확정 전에 저장된 것이다.
                // 셀을 아직 못 읽었으면 Tick이 셀을 확인한 뒤 같은 판단을 한다.
                endRunAfterLoad = cell && !inPackCell && Core::MapSystem::ShouldEndOnReturn(g_activeMap);
                g_endCheckAfterLoad = !cell;
                isArena4 = g_activeMap.isArena4;
                bossId = g_activeMap.bossRefFormId;
                arenaPhase = g_activeMap.arenaPhase;
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

        if (endRunAfterLoad) {
            EndRunOnMainThread();
            SKSE::log::info("MapDeviceRuntime: ended a run saved outside the pack with a pending end condition.");
            return;
        }

        if (bossId != 0) {
            if (auto* boss = RE::TESForm::LookupByID<RE::Actor>(bossId); boss && !boss->IsDead() && !boss->IsDisabled()) {
                if (isArena4) {
                    const std::uint16_t mask = (arenaPhase <= 1)
                        ? static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Armoured))
                        : (arenaPhase == 2
                            ? static_cast<std::uint16_t>(
                                  (1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted)) |
                                  (1u << static_cast<std::uint8_t>(Core::MonsterModKind::ExtraFire)))
                            : static_cast<std::uint16_t>(
                                  (1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted)) |
                                  (1u << static_cast<std::uint8_t>(Core::MonsterModKind::Deadly)) |
                                  (1u << static_cast<std::uint8_t>(Core::MonsterModKind::ExtraLightning))));
                    MonsterRuntime::ForcePromoteForTest(boss, Core::MonsterRarity::Rare, mask, -1.0);
                } else {
                    std::uint16_t mask = static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Armoured));
                    if (summaryCopy.forceHasted) {
                        mask |= static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted));
                    }
                    if (summaryCopy.forceDeadly) {
                        mask |= static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Deadly));
                    }
                    MonsterRuntime::ForcePromoteForTest(boss, Core::MonsterRarity::Rare, mask, -1.0);
                }
            }
        }
        SyncMapLayer2ToStatEngine();
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
        g_incompatibleSavedRun.reset();
        SetActiveMapLocked({});
        g_spawnedForCurrentRun = false;
        g_pendingSpawnRunId = 0;
        g_arenaPhaseInvulnSec = 0.0;
        g_deathEscapeExitElapsedSec = 0.0;
        g_deathEscapeInFlight.store(false, std::memory_order_release);
        g_uninstallCleanupArmed.store(false, std::memory_order_release);
        RefreshAtomicsLocked();
    }
}
