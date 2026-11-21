#include "PCH.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/CycloneRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
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
#include <cmath>
#include <format>
#include <mutex>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        constexpr RE::FormID kCocMarkerBaseId = 0x00000032;
        constexpr RE::FormID kTamrielWorldspaceId = 0x0000003C;
        constexpr double kDeathEscapeReleaseCooldownSec = 1.5;
        constexpr double kArena4PhaseInvulnSec = 0.8;

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::Config* g_config = nullptr;
        Core::EnergyShieldPool* g_esPool = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;

        std::mutex g_mutex;
        Core::ActiveMapState g_activeMap{};
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
        std::atomic<std::uint32_t> g_activeCombatAreaLevel{ 0 };
        std::atomic<RE::FormID> g_activeBossFormId{ 0 };
        std::atomic_bool g_activeIsArena4{ false };
        std::atomic_bool g_deathEscapeInFlight{ false };
        std::atomic_bool g_uninstallCleanupArmed{ false };

        [[nodiscard]] std::uint32_t ComputeCombatAreaLevel(const Core::ActiveMapState& st) noexcept
        {
            if (!st.active) {
                return 0u;
            }
            return st.isArena4 ? Core::kArena4MonsterLevel : Core::MapSystem::MonsterLevelForTier(st.tier);
        }

        void RefreshAtomicsLocked() noexcept
        {
            const bool active = g_activeMap.active;
            g_activeMapRun.store(active, std::memory_order_release);
            if (active) {
                const auto& dg = Core::MapSystem::GetDungeonForState(g_activeMap);
                g_activeCellFormId.store(dg.cellFormId, std::memory_order_release);
                g_activeMapTier.store(g_activeMap.tier, std::memory_order_release);
                g_activeCombatAreaLevel.store(ComputeCombatAreaLevel(g_activeMap), std::memory_order_release);
                g_activeBossFormId.store(g_activeMap.bossRefFormId, std::memory_order_release);
                g_activeIsArena4.store(g_activeMap.isArena4, std::memory_order_release);
            } else {
                g_activeCellFormId.store(0u, std::memory_order_release);
                g_activeMapTier.store(0u, std::memory_order_release);
                g_activeCombatAreaLevel.store(0u, std::memory_order_release);
                g_activeBossFormId.store(0u, std::memory_order_release);
                g_activeIsArena4.store(false, std::memory_order_release);
                g_playerInActiveMapCell.store(false, std::memory_order_release);
            }
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
            return true;
        }

        bool MovePlayerToFallbackSafeLocation(RE::PlayerCharacter* player)
        {
            if (!player) {
                return false;
            }
            if (auto* drCell = RE::TESForm::LookupByEditorID<RE::TESObjectCELL>("WhiterunDragonsreach")) {
                RE::TESObjectREFR* cocMarker = nullptr;
                drCell->ForEachReference([&](RE::TESObjectREFR* ref) {
                    if (ref && ref->GetBaseObject() && ref->GetBaseObject()->GetFormID() == kCocMarkerBaseId) {
                        cocMarker = ref;
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                if (cocMarker) {
                    return InvokeMoveToImpl(
                        player, cocMarker->GetHandle(), drCell, nullptr,
                        cocMarker->GetPosition(), cocMarker->GetAngle());
                }
                return InvokeMoveToImpl(
                    player, player->CreateRefHandle(), drCell, nullptr,
                    RE::NiPoint3{ 0.0f, 0.0f, 0.0f }, RE::NiPoint3{ 0.0f, 0.0f, 0.0f });
            }
            if (auto* tamriel = RE::TESForm::LookupByID<RE::TESWorldSpace>(kTamrielWorldspaceId)) {
                return InvokeMoveToImpl(
                    player, player->CreateRefHandle(), nullptr, tamriel,
                    RE::NiPoint3{ 22200.0f, -7300.0f, -3400.0f }, RE::NiPoint3{ 0.0f, 0.0f, 0.0f });
            }
            return false;
        }

        bool TeleportPlayerToSavedReturn(RE::PlayerCharacter* player, const Core::ActiveMapState& snap)
        {
            if (!player) {
                return false;
            }
            auto* retCell = (snap.returnCellFormId != 0)
                ? RE::TESForm::LookupByID<RE::TESObjectCELL>(snap.returnCellFormId)
                : nullptr;
            auto* retWs = (snap.returnWorldspaceFormId != 0)
                ? RE::TESForm::LookupByID<RE::TESWorldSpace>(snap.returnWorldspaceFormId)
                : nullptr;
            const RE::NiPoint3 pos{ snap.returnPos[0], snap.returnPos[1], snap.returnPos[2] };
            const RE::NiPoint3 ang{ snap.returnAngle[0], snap.returnAngle[1], snap.returnAngle[2] };

            // 검수 C-1: 야외(Tamriel 등)는 언로드되어 retCell==nullptr일 수 있으므로 retWs!=nullptr이면 그대로 이동
            if (retWs != nullptr) {
                return MapDeviceRuntime::MovePlayerToSpace(player, retCell, retWs, pos, ang);
            }
            if (retCell != nullptr) {
                return MapDeviceRuntime::MovePlayerToSpace(player, retCell, nullptr, pos, ang);
            }
            SKSE::log::warn("MapDeviceRuntime: return cell 0x{:08X} / ws 0x{:08X} unavailable; using safe fallback.",
                snap.returnCellFormId, snap.returnWorldspaceFormId);
            return MovePlayerToFallbackSafeLocation(player);
        }

        bool TeleportPlayerToDungeonEntry(
            RE::PlayerCharacter* player,
            const Core::MapDungeonDef& dungeon)
        {
            if (!player) {
                return false;
            }
            auto* targetCell = RE::TESForm::LookupByID<RE::TESObjectCELL>(dungeon.cellFormId);
            auto* marker = RE::TESForm::LookupByID<RE::TESObjectREFR>(dungeon.entryMarkerFormId);
            if (marker) {
                auto* markerCell = marker->GetParentCell() ? marker->GetParentCell() : targetCell;
                auto* markerWs = marker->GetWorldspace();
                if (markerCell || markerWs) {
                    return InvokeMoveToImpl(
                        player,
                        marker->GetHandle(),
                        markerCell,
                        markerWs,
                        marker->GetPosition(),
                        marker->GetAngle());
                }
            }
            if (targetCell) {
                return InvokeMoveToImpl(
                    player,
                    player->CreateRefHandle(),
                    targetCell,
                    nullptr,
                    RE::NiPoint3{ 0.0f, 0.0f, 0.0f },
                    RE::NiPoint3{ 0.0f, 0.0f, 0.0f });
            }
            return false;
        }

        void CaptureReturnLocation(
            RE::PlayerCharacter* player,
            RE::FormID& outCellId,
            RE::FormID& outWsId,
            std::array<float, 3>& outPos,
            std::array<float, 3>& outAngle)
        {
            if (!player) {
                return;
            }
            auto* cell = player->GetParentCell();
            auto* ws = player->GetWorldspace();
            outCellId = cell ? cell->GetFormID() : 0u;
            outWsId = ws ? ws->GetFormID() : 0u;
            const auto pos = player->GetPosition();
            const auto ang = player->GetAngle();
            outPos = { pos.x, pos.y, pos.z };
            outAngle = { ang.x, ang.y, ang.z };
        }

        void CleanupSpawnedRefsOnMainThread()
        {
            std::vector<std::uint32_t> toClean;
            {
                std::scoped_lock lk(g_mutex);
                toClean.swap(g_activeMap.spawnedRefFormIds);
                g_activeMap.bossRefFormId = 0;
                g_activeBossFormId.store(0u, std::memory_order_release);
            }
            for (const auto fid : toClean) {
                if ((fid & 0xFF000000u) != 0xFF000000u) {
                    continue;
                }
                AilmentRuntime::OnActorReset(fid);
                MonsterRuntime::OnActorReset(fid);
                auto* form = RE::TESForm::LookupByID(fid);
                auto* refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
                if (refr && !refr->IsPlayerRef() && !refr->IsDeleted()) {
                    refr->Disable();
                    refr->SetDelete(true);
                }
            }
        }

        RE::Actor* SpawnNpcAtAnchor(
            RE::PlayerCharacter* player,
            RE::FormID anchorFormId,
            RE::FormID npcTemplateFormId,
            const RE::NiPoint3& offset)
        {
            if (!player || npcTemplateFormId == 0) {
                return nullptr;
            }
            auto* npcBase = RE::TESForm::LookupByID<RE::TESNPC>(npcTemplateFormId);
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!npcBase || !dataHandler) {
                return nullptr;
            }
            auto* anchor = (anchorFormId != 0)
                ? RE::TESForm::LookupByID<RE::TESObjectREFR>(anchorFormId)
                : nullptr;
            auto* cell = (anchor && anchor->GetParentCell()) ? anchor->GetParentCell() : player->GetParentCell();
            auto* ws = anchor ? anchor->GetWorldspace() : player->GetWorldspace();
            if (!cell && !ws) {
                return nullptr;
            }
            RE::NiPoint3 spawnPos = anchor ? anchor->GetPosition() : player->GetPosition();
            spawnPos.x += offset.x;
            spawnPos.y += offset.y;
            spawnPos.z += offset.z;
            const RE::NiPoint3 spawnAng = anchor ? anchor->GetAngle() : RE::NiPoint3{};

            const auto handle = dataHandler->CreateReferenceAtLocation(
                npcBase,
                spawnPos,
                spawnAng,
                cell,
                ws,
                nullptr,
                nullptr,
                RE::ObjectRefHandle{},
                false,
                true);
            auto ptr = handle.get();
            auto* actor = ptr ? ptr->As<RE::Actor>() : nullptr;
            if (actor) {
                actor->StartCombat(player);
            }
            return actor;
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

        void ExecuteArena4PhaseTransitionOnMainThread(std::uint8_t newPhase, RE::FormID bossFormId)
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || MapDeviceRuntime::IsUninstallCleanupArmed()) {
                return;
            }
            if (auto* boss = RE::TESForm::LookupByID<RE::Actor>(bossFormId); boss && !boss->IsDead()) {
                SpawnHarmlessVfxAtActor(boss, FormIds::kVfxFireBurst);
                const std::uint16_t phaseMods = (newPhase == 2)
                    ? (static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted)) |
                       static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::ExtraFire)))
                    : (static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted)) |
                       static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Deadly)) |
                       static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::ExtraLightning)));
                MonsterRuntime::ForcePromoteForTest(boss, Core::MonsterRarity::Rare, phaseMods, -1.0);
            }

            const auto& arena = Core::MapSystem::GetArena4Dungeon();
            std::vector<RE::FormID> addedIds;
            const RE::FormID addNpc = arena.extraAddNpcFormId != 0 ? arena.extraAddNpcFormId : arena.packNpcFormId;
            for (std::size_t i = 0; i < 2 && i < arena.packAnchorCount; ++i) {
                const RE::NiPoint3 off{ (i == 0 ? 40.0f : -40.0f), 30.0f, 0.0f };
                if (auto* add = SpawnNpcAtAnchor(player, arena.packAnchorFormIds[i], addNpc, off)) {
                    addedIds.push_back(add->GetFormID());
                }
            }
            if (!addedIds.empty()) {
                std::scoped_lock lk(g_mutex);
                for (const auto fid : addedIds) {
                    if (g_activeMap.spawnedRefFormIds.size() < Core::kMaxMapSpawnedActors) {
                        g_activeMap.spawnedRefFormIds.push_back(fid);
                    }
                }
            }
            const auto msg = std::format("[4차 전직 결투장] 보스 {}페이즈 돌입! 증원군 출현!", newPhase);
            RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        }

        void SpawnMapEncounterOnMainThread(RE::PlayerCharacter* player)
        {
            Core::ActiveMapState snap;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_spawnedForCurrentRun) {
                    return;
                }
                snap = g_activeMap;
            }

            std::vector<std::uint32_t> spawned;
            RE::FormID bossId = 0;
            const auto& dg = Core::MapSystem::GetDungeonForState(snap);

            if (snap.isArena4) {
                if (auto* boss = SpawnNpcAtAnchor(player, dg.bossAnchorFormId, dg.bossNpcFormId, {})) {
                    bossId = boss->GetFormID();
                    spawned.push_back(bossId);
                    const std::uint16_t p1Mask =
                        static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Armoured));
                    MonsterRuntime::ForcePromoteForTest(boss, Core::MonsterRarity::Rare, p1Mask, -1.0);
                }
                const RE::FormID addNpc = dg.extraAddNpcFormId != 0 ? dg.extraAddNpcFormId : dg.packNpcFormId;
                for (std::size_t i = 0; i < 2 && i < dg.packAnchorCount; ++i) {
                    if (auto* add = SpawnNpcAtAnchor(player, dg.packAnchorFormIds[i], addNpc, {})) {
                        spawned.push_back(add->GetFormID());
                    }
                }
            } else {
                if (auto* boss = SpawnNpcAtAnchor(player, dg.bossAnchorFormId, dg.bossNpcFormId, {})) {
                    bossId = boss->GetFormID();
                    spawned.push_back(bossId);
                    std::uint16_t bossMods =
                        static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Armoured));
                    if (snap.summary.forceHasted) {
                        bossMods |= static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Hasted));
                    }
                    if (snap.summary.forceDeadly) {
                        bossMods |= static_cast<std::uint16_t>(1u << static_cast<std::uint8_t>(Core::MonsterModKind::Deadly));
                    }
                    MonsterRuntime::ForcePromoteForTest(boss, Core::MonsterRarity::Rare, bossMods, -1.0);
                }

                const std::uint32_t basePacks = g_config ? std::clamp<std::uint32_t>(g_config->mapBasePackCount, 1u, 6u) : 3u;
                const std::uint32_t extraPacks = (snap.summary.packSizePct >= 15.0) ? 1u : 0u;
                const std::uint32_t totalPacks = std::min<std::uint32_t>(6u, basePacks + extraPacks);
                constexpr std::array<RE::NiPoint3, 3> kPackOffsets = {{
                    { 0.0f, 0.0f, 0.0f },
                    { 85.0f, 45.0f, 0.0f },
                    { -85.0f, -45.0f, 0.0f }
                }};
                const std::size_t anchorCount = dg.packAnchorCount;
                for (std::uint32_t p = 0; p < totalPacks && anchorCount > 0; ++p) {
                    const RE::FormID anchorId = dg.packAnchorFormIds[p % anchorCount];
                    const float wrapShift = static_cast<float>((p / anchorCount) * 110);
                    for (std::size_t m = 0; m < kPackOffsets.size(); ++m) {
                        if (spawned.size() >= Core::kMaxMapSpawnedActors) {
                            break;
                        }
                        RE::NiPoint3 off = kPackOffsets[m];
                        off.x += wrapShift;
                        if (auto* mob = SpawnNpcAtAnchor(player, anchorId, dg.packNpcFormId, off)) {
                            spawned.push_back(mob->GetFormID());
                        }
                    }
                }
            }

            bool staleRun = false;
            std::size_t spawnedCount = 0;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active || g_activeMap.runId != snap.runId) {
                    staleRun = true;
                } else {
                    g_activeMap.bossRefFormId = bossId;
                    spawnedCount = spawned.size();
                    g_activeMap.spawnedRefFormIds = std::move(spawned);
                    g_spawnedForCurrentRun = true;
                    g_pendingSpawnRunId = 0;
                    RefreshAtomicsLocked();
                }
            }
            if (staleRun) {
                for (const auto fid : spawned) {
                    if ((fid & 0xFF000000u) != 0xFF000000u) {
                        continue;
                    }
                    AilmentRuntime::OnActorReset(fid);
                    MonsterRuntime::OnActorReset(fid);
                    if (auto* f = RE::TESForm::LookupByID(fid)) {
                        if (auto* r = f->As<RE::TESObjectREFR>(); r && !r->IsPlayerRef() && !r->IsDeleted()) {
                            r->Disable();
                            r->SetDelete(true);
                        }
                    }
                }
                return;
            }

            SKSE::log::info("MapDeviceRuntime: spawned {} actor(s) (boss=0x{:08X}, arena4={}, T{}, areaLv={}).",
                spawnedCount, bossId, snap.isArena4, snap.tier, ComputeCombatAreaLevel(snap));
        }

        void ExecuteDeathEscapeOnMainThread()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player || MapDeviceRuntime::IsUninstallCleanupArmed()) {
                g_deathEscapeInFlight.store(false, std::memory_order_release);
                return;
            }

            // 1) 플레이어 상태이상 해제 및 체력·매지카·스태미나 최소 안전선(50%) 복구
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

            Core::ActiveMapState snap;
            bool survivedMap = false;
            {
                std::scoped_lock lk(g_mutex);
                if (!g_activeMap.active) {
                    g_deathEscapeInFlight.store(false, std::memory_order_release);
                    return;
                }
                survivedMap = Core::MapSystem::ConsumePortalOnDeathEscape(g_activeMap);
                snap = g_activeMap;
                g_deathEscapeExitElapsedSec = 0.0;
            }

            (void)TeleportPlayerToSavedReturn(player, snap);

            if (!survivedMap) {
                CleanupSpawnedRefsOnMainThread();
                {
                    std::scoped_lock lk(g_mutex);
                    g_activeMap = Core::ActiveMapState{};
                    g_spawnedForCurrentRun = false;
                    g_pendingSpawnRunId = 0;
                    g_arenaPhaseInvulnSec = 0.0;
                    RefreshAtomicsLocked();
                }
                MapDeviceRuntime::SyncMapLayer2ToStatEngine();
                const char* msg = snap.isArena4
                    ? "[4차 전직 결투장] 치명타로 도전에 실패했습니다. 귀환합니다."
                    : "[지도 장치] 남은 포털이 모두 소진되어 지도가 닫혔습니다.";
                RE::SendHUDMessage::ShowHUDMessage(msg);
            } else {
                g_playerInActiveMapCell.store(false, std::memory_order_release);
                MapDeviceRuntime::SyncMapLayer2ToStatEngine();
                const auto msg = std::format("[지도 장치] 치명타 가로채기 발동! 안전 귀환 (남은 포털: {}/{})",
                    snap.portalsRemaining, Core::kNormalMapMaxPortals);
                RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
            }
            SKSE::log::info("MapDeviceRuntime: death escape executed (arena4={}, survivedMap={}, portalsRemaining={}).",
                snap.isArena4, survivedMap, snap.portalsRemaining);
        }
    }

    void MapDeviceRuntime::Initialize(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        const Core::ItemData* itemData,
        const Core::PassiveTreeData* treeData,
        const Core::Config* config,
        Core::EnergyShieldPool* esPool,
        Core::CharmSystem* charmSystem)
    {
        g_queue = queue;
        g_statEngine = statEngine;
        g_itemData = itemData;
        g_treeData = treeData;
        g_config = config;
        g_esPool = esPool;
        g_charmSystem = charmSystem;
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
        const bool inMapCell = g_playerInActiveMapCell.load(std::memory_order_acquire);
        std::uint32_t areaLv = 0;
        Core::MapModSummary summary{};
        {
            std::scoped_lock lk(g_mutex);
            if (g_activeMap.active && inMapCell) {
                areaLv = ComputeCombatAreaLevel(g_activeMap);
                summary = g_activeMap.summary;
            }
        }
        g_statEngine->UpdateLayer2([inActive = (areaLv > 0), areaLv, summary](Core::Layer2Input& l2) {
            l2.inActiveMap = inActive;
            l2.combatAreaLevel = areaLv;
            l2.activeMapMods = summary;
        });
    }

    void MapDeviceRuntime::Tick(double deltaSec, bool gamePaused)
    {
        if (g_uninstallCleanupArmed.load(std::memory_order_acquire)) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        const RE::FormID curCellId = cell ? cell->GetFormID() : 0u;
        const RE::FormID activeCellId = g_activeCellFormId.load(std::memory_order_acquire);
        const bool isActiveRun = g_activeMapRun.load(std::memory_order_acquire);
        const bool nowInMapCell = isActiveRun && activeCellId != 0u && curCellId == activeCellId;
        const bool prevInMapCell = g_playerInActiveMapCell.exchange(nowInMapCell, std::memory_order_acq_rel);

        if (nowInMapCell != prevInMapCell) {
            SyncMapLayer2ToStatEngine();
        }

        if (gamePaused || !player) {
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        const bool loading = ui && (
            ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ||
            ui->IsMenuOpen(RE::FaderMenu::MENU_NAME));

        // 검수 C-2 + Major #2: 사망 탈출 무적/중복 방지 플래그는 로딩·일시정지 해제 후
        // 플레이어 3D가 지도 셀 밖에서 실제로 로드된 상태로 1.5초 경과해야 해제한다.
        if (g_deathEscapeInFlight.load(std::memory_order_acquire)) {
            if (!nowInMapCell && !loading && player->Is3DLoaded() &&
                std::isfinite(deltaSec) && deltaSec > 0.0) {
                bool releaseNow = false;
                {
                    std::scoped_lock lk(g_mutex);
                    g_deathEscapeExitElapsedSec += deltaSec;
                    if (g_deathEscapeExitElapsedSec >= kDeathEscapeReleaseCooldownSec) {
                        g_deathEscapeExitElapsedSec = 0.0;
                        releaseNow = true;
                    }
                }
                if (releaseNow) {
                    g_deathEscapeInFlight.store(false, std::memory_order_release);
                }
            }
        }

        bool shouldSpawnNow = false;
        {
            std::scoped_lock lk(g_mutex);
            if (g_activeMap.active && g_arenaPhaseInvulnSec > 0.0 &&
                std::isfinite(deltaSec) && deltaSec > 0.0) {
                g_arenaPhaseInvulnSec = std::max(0.0, g_arenaPhaseInvulnSec - deltaSec);
            }
            if (g_activeMap.active && !g_spawnedForCurrentRun &&
                g_pendingSpawnRunId != 0 && g_pendingSpawnRunId == g_activeMap.runId &&
                nowInMapCell) {
                if (!loading && player->Is3DLoaded()) {
                    shouldSpawnNow = true;
                }
            }
        }

        if (shouldSpawnNow) {
            SpawnMapEncounterOnMainThread(player);
        }
    }

    MapActionResult MapDeviceRuntime::OpenNormalMap(
        RE::PlayerCharacter* player,
        const Core::InventoryItemKey& mapKey,
        RE::FormID baseFormId)
    {
        if (!player) {
            return { false, "플레이어를 찾을 수 없습니다." };
        }
        if (g_uninstallCleanupArmed.load(std::memory_order_acquire)) {
            return { false, "안전 제거 정리 완료 상태입니다. 세이브 후 종료하거나 다시 불러오세요." };
        }
        if (g_playerInActiveMapCell.load(std::memory_order_acquire)) {
            return { false, "지도 내부에서는 새 지도를 열 수 없습니다. 먼저 포털로 귀환하세요." };
        }
        if (player->IsInCombat()) {
            return { false, "전투 중에는 지도를 열 수 없습니다." };
        }

        const auto localId = FormIds::ResolveLocalFormIdIfOurs(baseFormId);
        const auto tierOpt = FormIds::MapTierFromLocalFormId(localId);
        if (!tierOpt) {
            return { false, "유효한 지도 아이템이 아닙니다." };
        }
        const std::uint8_t tier = *tierOpt;
        auto* miscObj = RE::TESForm::LookupByID<RE::TESBoundObject>(baseFormId);
        if (!miscObj) {
            return { false, "지도 베이스 폼을 찾을 수 없습니다." };
        }

        std::optional<Core::PoEItemRecord> consumedTrackedRecord;
        std::vector<Core::RolledAffix> rolledMods;
        bool consumed = false;
        SaveLoad::RunSerializedTransaction([&]() {
            if (mapKey.IsValid()) {
                // 배치2 검수 Major #3: 추적 제작 지도가 지정됐는데 찾지 못하면 일반 지도로 폴백 오소비하지 않고 실패 반환
                const auto tracked = ItemTracker::FindByKey(mapKey);
                if (!tracked) {
                    return;
                }
                auto* extra = ItemTracker::FindExtraListByKey(player, miscObj, mapKey);
                if (!extra) {
                    return;
                }
                consumedTrackedRecord = *tracked;
                rolledMods = tracked->affixes;
                (void)player->RemoveItem(miscObj, 1, RE::ITEM_REMOVE_REASON::kRemove, extra, nullptr);
                ItemTracker::RemoveByKey(mapKey);
                consumed = true;
                return;
            }
            if (UniqueIdUtil::RemovePlainItem(player, miscObj, 1)) {
                consumed = true;
            }
        });
        if (!consumed) {
            return { false, "인벤토리에서 해당 지도를 소비하지 못했습니다." };
        }

        // 소비 확정 이후에만 기존 활성 지도 잔여물을 정리한다(소비 실패 시 기존 지도 파괴 방지)
        if (g_activeMapRun.load(std::memory_order_acquire)) {
            CleanupSpawnedRefsOnMainThread();
        }

        RE::FormID retCell = 0;
        RE::FormID retWs = 0;
        std::array<float, 3> retPos{};
        std::array<float, 3> retAng{};
        CaptureReturnLocation(player, retCell, retWs, retPos, retAng);

        Core::ActiveMapState next{};
        {
            std::scoped_lock lk(g_mutex);
            next = Core::MapSystem::CreateNormalMapRun(
                g_nextRunId++, tier, rolledMods, retCell, retWs, retPos, retAng);
            g_activeMap = next;
            g_spawnedForCurrentRun = false;
            g_pendingSpawnRunId = next.runId;
            g_arenaPhaseInvulnSec = 0.0;
            g_deathEscapeExitElapsedSec = 0.0;
            g_deathEscapeInFlight.store(false, std::memory_order_release);
            RefreshAtomicsLocked();
        }

        const auto& dungeon = Core::MapSystem::GetDungeonForState(next);
        if (!TeleportPlayerToDungeonEntry(player, dungeon)) {
            {
                std::scoped_lock lk(g_mutex);
                g_activeMap = Core::ActiveMapState{};
                g_spawnedForCurrentRun = false;
                g_pendingSpawnRunId = 0;
                RefreshAtomicsLocked();
            }
            if (consumedTrackedRecord.has_value()) {
                if (auto* freshExtra = UniqueIdUtil::CreateGameExtraDataList()) {
                    const auto newKey = ItemTracker::AddObjectWithTrackedUniqueId(player, miscObj, freshExtra);
                    if (newKey.IsValid()) {
                        ItemTracker::Upsert(newKey, *consumedTrackedRecord, false);
                    }
                }
            } else {
                player->AddObjectToContainer(miscObj, nullptr, 1, nullptr);
            }
            return { false, "지도 던전 입구 마커로 이동하지 못했습니다." };
        }

        SyncMapLayer2ToStatEngine();
        const auto msg = std::format("T{} {} 입장 (Lv.{}, 남은 포털 {}/{})",
            tier, dungeon.koName, ComputeCombatAreaLevel(next), next.portalsRemaining, Core::kNormalMapMaxPortals);
        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        SKSE::log::info("MapDeviceRuntime: opened T{} map '{}' (cell=0x{:08X}, portals={}/{}).",
            tier, dungeon.id, dungeon.cellFormId, next.portalsRemaining, Core::kNormalMapMaxPortals);
        return { true, msg };
    }

    MapActionResult MapDeviceRuntime::OpenArena4(RE::PlayerCharacter* player)
    {
        if (!player) {
            return { false, "플레이어를 찾을 수 없습니다." };
        }
        if (g_uninstallCleanupArmed.load(std::memory_order_acquire)) {
            return { false, "안전 제거 정리 완료 상태입니다." };
        }
        if (g_playerInActiveMapCell.load(std::memory_order_acquire)) {
            return { false, "지도 내부에서는 4차 전직 결투장을 열 수 없습니다. 먼저 귀환하세요." };
        }
        if (player->IsInCombat()) {
            return { false, "전투 중에는 4차 전직 결투장을 열 수 없습니다." };
        }

        auto* tributeObj = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
        if (!tributeObj) {
            return { false, "4차 전직의 공물 폼을 찾을 수 없습니다." };
        }
        if (CountPlayerTributes(player) <= 0) {
            return { false, "4차 전직의 공물이 필요합니다 (T6+ 지도 보스 처치로 획득)." };
        }

        bool consumed = false;
        SaveLoad::RunSerializedTransaction([&]() {
            consumed = UniqueIdUtil::RemovePlainItem(player, tributeObj, 1);
            if (!consumed && CountPlayerTributes(player) > 0) {
                (void)player->RemoveItem(tributeObj, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                consumed = true;
            }
        });
        if (!consumed) {
            return { false, "4차 전직의 공물을 소비하지 못했습니다." };
        }

        if (g_activeMapRun.load(std::memory_order_acquire)) {
            CleanupSpawnedRefsOnMainThread();
        }

        RE::FormID retCell = 0;
        RE::FormID retWs = 0;
        std::array<float, 3> retPos{};
        std::array<float, 3> retAng{};
        CaptureReturnLocation(player, retCell, retWs, retPos, retAng);

        Core::ActiveMapState next{};
        {
            std::scoped_lock lk(g_mutex);
            next = Core::MapSystem::CreateArena4Run(g_nextRunId++, retCell, retWs, retPos, retAng);
            g_activeMap = next;
            g_spawnedForCurrentRun = false;
            g_pendingSpawnRunId = next.runId;
            g_arenaPhaseInvulnSec = 0.0;
            g_deathEscapeExitElapsedSec = 0.0;
            g_deathEscapeInFlight.store(false, std::memory_order_release);
            RefreshAtomicsLocked();
        }

        const auto& arena = Core::MapSystem::GetArena4Dungeon();
        if (!TeleportPlayerToDungeonEntry(player, arena)) {
            {
                std::scoped_lock lk(g_mutex);
                g_activeMap = Core::ActiveMapState{};
                g_spawnedForCurrentRun = false;
                g_pendingSpawnRunId = 0;
                RefreshAtomicsLocked();
            }
            player->AddObjectToContainer(tributeObj, nullptr, 1, nullptr);
            return { false, "4차 전직 결투장 입구로 이동하지 못했습니다." };
        }

        SyncMapLayer2ToStatEngine();
        const std::string msg = "4차 전직 결투장 입장! (단판 1포털 · 3페이즈 보스전)";
        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        SKSE::log::info("MapDeviceRuntime: opened Arena4 '{}' (cell=0x{:08X}, areaLv={}).",
            arena.id, arena.cellFormId, Core::kArena4MonsterLevel);
        return { true, msg };
    }

    MapActionResult MapDeviceRuntime::ReenterActiveMap(RE::PlayerCharacter* player)
    {
        if (!player) {
            return { false, "플레이어를 찾을 수 없습니다." };
        }
        if (g_uninstallCleanupArmed.load(std::memory_order_acquire)) {
            return { false, "안전 제거 정리 완료 상태입니다." };
        }
        if (g_playerInActiveMapCell.load(std::memory_order_acquire)) {
            return { false, "이미 활성 지도 내부에 있습니다." };
        }

        // 배치2 검수 Major #3: 엔진 API(CaptureReturnLocation)는 g_mutex 밖에서 호출
        RE::FormID retCell = 0;
        RE::FormID retWs = 0;
        std::array<float, 3> retPos{};
        std::array<float, 3> retAng{};
        CaptureReturnLocation(player, retCell, retWs, retPos, retAng);

        Core::ActiveMapState snap;
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active || g_activeMap.failed) {
                return { false, "재입장 가능한 활성 지도가 없습니다." };
            }
            if (!Core::MapSystem::TryConsumePortalForReenter(g_activeMap)) {
                return { false, "남은 포털이 없습니다." };
            }
            g_activeMap.returnCellFormId = retCell;
            g_activeMap.returnWorldspaceFormId = retWs;
            g_activeMap.returnPos = retPos;
            g_activeMap.returnAngle = retAng;
            if (!g_spawnedForCurrentRun) {
                g_pendingSpawnRunId = g_activeMap.runId;
            }
            // 배치2 검수 Critical #2: 1.5초 이내 재입장 시 g_deathEscapeInFlight 영구 무적 고착 방지
            g_deathEscapeExitElapsedSec = 0.0;
            g_deathEscapeInFlight.store(false, std::memory_order_release);
            snap = g_activeMap;
            RefreshAtomicsLocked();
        }

        const auto& dg = Core::MapSystem::GetDungeonForState(snap);
        if (!TeleportPlayerToDungeonEntry(player, dg)) {
            // 배치2 검수 Minor #6: 텔레포트 실패 시 차감한 포털 1개 환급
            std::scoped_lock lk(g_mutex);
            if (g_activeMap.active && g_activeMap.runId == snap.runId &&
                g_activeMap.portalsRemaining < Core::kNormalMapMaxPortals) {
                ++g_activeMap.portalsRemaining;
            }
            return { false, "활성 지도 재입장에 실패했습니다." };
        }
        SyncMapLayer2ToStatEngine();
        const auto msg = std::format("지도 재입장 완료 (남은 포털: {}/{})",
            snap.portalsRemaining, snap.isArena4 ? Core::kArena4MaxPortals : Core::kNormalMapMaxPortals);
        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        return { true, msg };
    }

    MapActionResult MapDeviceRuntime::ReturnPortal(RE::PlayerCharacter* player)
    {
        if (!player) {
            return { false, "플레이어를 찾을 수 없습니다." };
        }
        Core::ActiveMapState snap;
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active) {
                return { false, "활성 지도가 없습니다." };
            }
            snap = g_activeMap;
        }
        if (!TeleportPlayerToSavedReturn(player, snap)) {
            return { false, "원래 위치로 귀환하지 못했습니다." };
        }
        g_playerInActiveMapCell.store(false, std::memory_order_release);
        SyncMapLayer2ToStatEngine();
        const auto msg = std::format("포털 귀환 완료 (남은 포털: {}/{})",
            snap.portalsRemaining, snap.isArena4 ? Core::kArena4MaxPortals : Core::kNormalMapMaxPortals);
        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        return { true, msg };
    }

    MapActionResult MapDeviceRuntime::AbandonActiveMap(RE::PlayerCharacter* player)
    {
        Core::ActiveMapState snap;
        bool wasInCell = false;
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active) {
                return { false, "종료할 활성 지도가 없습니다." };
            }
            snap = g_activeMap;
            wasInCell = g_playerInActiveMapCell.load(std::memory_order_acquire);
        }

        if (player && wasInCell) {
            (void)TeleportPlayerToSavedReturn(player, snap);
        }
        CleanupSpawnedRefsOnMainThread();
        {
            std::scoped_lock lk(g_mutex);
            g_activeMap = Core::ActiveMapState{};
            g_spawnedForCurrentRun = false;
            g_pendingSpawnRunId = 0;
            g_arenaPhaseInvulnSec = 0.0;
            g_deathEscapeExitElapsedSec = 0.0;
            g_deathEscapeInFlight.store(false, std::memory_order_release);
            RefreshAtomicsLocked();
        }
        SyncMapLayer2ToStatEngine();
        RE::SendHUDMessage::ShowHUDMessage("활성 지도를 닫았습니다.");
        return { true, "활성 지도를 닫았습니다." };
    }

    MapActionResult MapDeviceRuntime::RunSafeUninstallCleanup(RE::PlayerCharacter* player)
    {
        if (!player) {
            return { false, "플레이어를 찾을 수 없습니다." };
        }

        // 배치2 검수 Major #1: 사이클론 즉시 중단 + 바닥 드랍/빛기둥을 활성 지도 시체 삭제·이탈 전에 먼저 정리
        CycloneRuntime::StopNowOnMainThread(player);
        WorldDropTracker::ReturnAllToCorpsesNow();
        (void)WorldDropTracker::SweepOrphanBeamsNearPlayer();

        if (g_activeMapRun.load(std::memory_order_acquire)) {
            (void)AbandonActiveMap(player);
            (void)WorldDropTracker::SweepOrphanBeamsNearPlayer();
        }

        std::size_t returnedGems = 0;
        std::size_t returnedJewels = 0;
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

            for (const auto& [nodeId, jRec] : st.socketedJewels) {
                auto* baseObj = RE::TESForm::LookupByID<RE::TESBoundObject>(jRec.baseFormId);
                if (!baseObj) {
                    baseObj = FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(jRec.jewelColor));
                }
                auto* freshExtra = baseObj ? UniqueIdUtil::CreateGameExtraDataList() : nullptr;
                if (baseObj && freshExtra) {
                    const auto newKey = ItemTracker::AddObjectWithTrackedUniqueId(player, baseObj, freshExtra);
                    if (newKey.IsValid()) {
                        ItemTracker::Upsert(newKey, jRec, false);
                        ++returnedJewels;
                    }
                }
            }
            st.socketedJewels.clear();

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

        g_uninstallCleanupArmed.store(true, std::memory_order_release);

        const auto msg = std::format(
            "안전 제거 정리 완료: 젬 {}개, 주얼 {}개, 참 {}개 반환 및 체력/매지카/속도 원복 완료. 저장 후 종료해 모드를 제거하세요.",
            returnedGems, returnedJewels, returnedCharms);
        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        SKSE::log::info("MapDeviceRuntime: SafeUninstallCleanup armed (gems={}, jewels={}, charms={}, hpDelta={:.1f}, mpDelta={:.1f}).",
            returnedGems, returnedJewels, returnedCharms, appliedHpDelta, appliedMpDelta);
        return { true, msg };
    }

    void MapDeviceRuntime::OnActorKilled(RE::FormID actorId, RE::Actor* victim)
    {
        if (actorId == 0 || g_uninstallCleanupArmed.load(std::memory_order_acquire)) {
            return;
        }
        bool isBossKill = false;
        bool isArena4 = false;
        std::uint8_t tier = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active || g_activeMap.bossRefFormId != actorId || g_activeMap.bossKilled) {
                return;
            }
            g_activeMap.bossKilled = true;
            isBossKill = true;
            isArena4 = g_activeMap.isArena4;
            tier = g_activeMap.tier;
        }
        if (!isBossKill) {
            return;
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
        bool grantFirstTribute = false;
        SaveLoad::UpdateState([&](PersistentState& st) {
            firstClear = Core::MapSystem::RecordMapClear(st.atlas, tier);
            totalAtlasPts = Core::MapSystem::CountAtlasTierPoints(st.atlas.completedTierMask);
            if (tier >= 6u && !st.atlas.firstTributeGiven) {
                st.atlas.firstTributeGiven = true;
                grantFirstTribute = true;
            }
            if (firstClear && g_treeData && g_treeData->IsLoaded() && g_config) {
                const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                    *g_treeData, st.tree, st.exileLevel, *g_config, st.atlas.completedTierMask);
                st.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
            }
        });

        if (grantFirstTribute) {
            if (auto* tributeObj = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute)) {
                if (victim && !victim->IsDeleted() && g_config && g_config->worldDropEnabled) {
                    (void)WorldDropTracker::SpawnPhysicalDropFromContainer(victim, tributeObj, 3, false, true);
                } else if (victim && !victim->IsDeleted()) {
                    victim->AddObjectToContainer(tributeObj, nullptr, 1, nullptr);
                } else if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                    player->AddObjectToContainer(tributeObj, nullptr, 1, nullptr);
                }
                RE::SendHUDMessage::ShowHUDMessage("[SkyrimPoE] 4차 전직의 공물을 획득했습니다!");
            }
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
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active || !g_activeMap.isArena4 || g_activeMap.bossRefFormId != bossId) {
                return rawSkyrimDamage;
            }
            clampRes = Core::MapSystem::ClampArenaBossDamage(
                g_activeMap.arenaPhase,
                g_arenaPhaseInvulnSec > 0.0,
                curHp,
                maxHp,
                rawSkyrimDamage * safeDiff);
            if (clampRes.triggeredNextPhase != 0) {
                g_activeMap.arenaPhase = clampRes.triggeredNextPhase;
                g_arenaPhaseInvulnSec = kArena4PhaseInvulnSec;
            }
        }
        if (clampRes.triggeredNextPhase != 0 && g_queue) {
            const std::uint8_t nextPhase = clampRes.triggeredNextPhase;
            g_queue->Enqueue([nextPhase, bossId]() {
                ExecuteArena4PhaseTransitionOnMainThread(nextPhase, bossId);
            });
        }
        return std::max(0.0, clampRes.clampedDamage / safeDiff);
    }

    bool MapDeviceRuntime::TryInterceptLethalHitFromHook(
        double curHp,
        double effectiveDamage)
    {
        if (!g_config || !g_config->mapDeathInterceptEnabled) {
            return false;
        }
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
                if (g_queue) {
                    g_queue->Enqueue([]() {
                        ExecuteDeathEscapeOnMainThread();
                    });
                } else {
                    g_deathEscapeInFlight.store(false, std::memory_order_release);
                    return false;
                }
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

    std::uint32_t MapDeviceRuntime::GetActiveCombatAreaLevel() noexcept
    {
        return g_activeCombatAreaLevel.load(std::memory_order_acquire);
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

    Core::MapModSummary MapDeviceRuntime::GetActiveMapModSummary()
    {
        std::scoped_lock lk(g_mutex);
        return g_activeMap.active ? g_activeMap.summary : Core::MapModSummary{};
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
            const auto& dg = Core::MapSystem::GetMapDungeon(Core::MapSystem::DungeonIndexForTier(tier));
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
                    item.displayName = std::format("T{} {} (Lv.{})",
                        tier, dg.koName, Core::MapSystem::MonsterLevelForTier(tier));
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
                item.displayName = std::format("T{} {} (Lv.{})",
                    tier, dg.koName, Core::MapSystem::MonsterLevelForTier(tier));
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
        return g_activeMap;
    }

    void MapDeviceRuntime::RestoreFromSave(Core::ActiveMapState saved)
    {
        std::scoped_lock lk(g_mutex);
        g_activeMap = std::move(saved);
        if (g_activeMap.runId >= g_nextRunId) {
            g_nextRunId = g_activeMap.runId + 1;
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
        const RE::FormID curCellId = cell ? cell->GetFormID() : 0u;

        bool isArena4 = false;
        RE::FormID bossId = 0;
        std::uint8_t arenaPhase = 1;
        Core::MapModSummary summaryCopy{};
        {
            std::scoped_lock lk(g_mutex);
            if (g_activeMap.active) {
                const auto& dg = Core::MapSystem::GetDungeonForState(g_activeMap);
                const bool inCell = (curCellId != 0 && curCellId == dg.cellFormId);
                g_playerInActiveMapCell.store(inCell, std::memory_order_release);
                isArena4 = g_activeMap.isArena4;
                bossId = g_activeMap.bossRefFormId;
                arenaPhase = g_activeMap.arenaPhase;
                summaryCopy = g_activeMap.summary;
            } else {
                g_playerInActiveMapCell.store(false, std::memory_order_release);
            }
            RefreshAtomicsLocked();
        }

        if (bossId != 0) {
            if (auto* boss = RE::TESForm::LookupByID<RE::Actor>(bossId); boss && !boss->IsDead()) {
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
        g_activeMap = Core::ActiveMapState{};
        g_spawnedForCurrentRun = false;
        g_pendingSpawnRunId = 0;
        g_arenaPhaseInvulnSec = 0.0;
        g_deathEscapeExitElapsedSec = 0.0;
        g_deathEscapeInFlight.store(false, std::memory_order_release);
        g_uninstallCleanupArmed.store(false, std::memory_order_release);
        RefreshAtomicsLocked();
    }
}
