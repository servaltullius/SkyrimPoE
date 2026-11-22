#include "PCH.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/CycloneRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
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
        const Core::DataPack* g_dataPack = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::Config* g_config = nullptr;
        Core::EnergyShieldPool* g_esPool = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;

        std::mutex g_mutex;
        Core::ActiveMapState g_activeMap{};
        std::optional<Core::PoEItemRecord> g_consumedMapRecord;
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
                g_activeCellFormId.store(forms ? forms->cell->GetFormID() : 0, std::memory_order_release);
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
            const MapPackRuntime::DungeonForms& dungeon)
        {
            if (!player) {
                return false;
            }
            if (auto* marker = dungeon.entryMarker) {
                auto* markerCell = marker->GetParentCell() ? marker->GetParentCell() : dungeon.cell;
                auto* markerWs = marker->GetWorldspace();
                if (markerCell || markerWs) {
                    return InvokeMoveToImpl(player, marker->GetHandle(), markerCell, markerWs,
                        marker->GetPosition(), marker->GetAngle());
                }
            }
            if (dungeon.cell) {
                return InvokeMoveToImpl(player, player->CreateRefHandle(), dungeon.cell, nullptr, {}, {});
            }
            return false;
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

        void CleanupSpawnedRefOnMainThread(RE::FormID fid)
        {
            if ((fid & 0xFF000000u) != 0xFF000000u) {
                return;
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

        void CleanupSpawnedRefsOnMainThread()
        {
            std::vector<std::uint32_t> toClean;
            {
                std::scoped_lock lk(g_mutex);
                // 보스는 스폰할 때 목록에도 들어가므로 따로 더하지 않는다(목록 밖 ID를 지우는 범위를 넓히지 않음).
                toClean.swap(g_activeMap.spawnedRefFormIds);
                g_activeMap.bossRefFormId = 0;
                g_activeBossFormId.store(0u, std::memory_order_release);
                RefreshMapActorsLocked();
            }
            for (const auto fid : toClean) {
                CleanupSpawnedRefOnMainThread(fid);
            }
        }

        void FinishCancelledRunOnMainThread(
            RE::PlayerCharacter* player, const Core::ActiveMapState& snap, bool returnPlayer,
            std::optional<bool> refunded, std::string_view reason)
        {
            // refunded: nullopt = 돌려줄 대상이 아님(보스를 처치한 회차), true/false = 환불 성공 여부
            // 보스는 스폰할 때 목록에도 들어가므로 목록만 지운다. 목록 밖의 보스 ID는 손상됐거나 재사용된 ID일 수 있다.
            for (const auto id : snap.spawnedRefFormIds) {
                CleanupSpawnedRefOnMainThread(id);
            }
            const bool returned = !returnPlayer || TeleportPlayerToSavedReturn(player, snap);
            const std::string_view refundText = !refunded.has_value()
                ? "보스를 처치한 회차라 소모품은 돌려주지 않습니다."
                : (*refunded ? (snap.isArena4 ? "공물을 돌려받았습니다." : "지도를 돌려받았습니다.")
                             : "소모품을 돌려주지 못했습니다.");
            const auto msg = std::format("{} {}{}", reason, refundText,
                returned ? "" : " 귀환 위치로 이동하지 못했습니다.");
            SKSE::log::warn("MapDeviceRuntime: cancelled run {}. {}", snap.runId, msg);
            RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
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
                if (ptr) {
                    CleanupSpawnedRefOnMainThread(ptr->GetFormID());
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
                CleanupSpawnedRefOnMainThread(actorId);
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
                if (!g_activeMap.active || !g_activeMap.isArena4 || g_activeMap.runId != runId ||
                    g_activeMap.bossRefFormId != bossFormId || g_activeMap.bossKilled) {
                    return;
                }
            }
            auto* boss = RE::TESForm::LookupByID<RE::Actor>(bossFormId);
            if (!boss || boss->IsDead()) {
                // 사망 큐가 bossKilled를 세우기 전에 보스가 이미 죽었으면 페이즈 전환과 증원을 하지 않는다.
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
                if (!g_activeMap.active || g_spawnedForCurrentRun) {
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

                const std::uint32_t basePacks = g_config ? std::clamp<std::uint32_t>(g_config->mapBasePackCount, 1u, 6u) : 3u;
                const std::uint32_t extraPacks = (snap.summary.packSizePct >= 15.0) ? 1u : 0u;
                const std::uint32_t totalPacks = std::min<std::uint32_t>(6u, basePacks + extraPacks);
                constexpr std::array<RE::NiPoint3, 3> kPackOffsets = {{
                    { 0.0f, 0.0f, 0.0f },
                    { 85.0f, 45.0f, 0.0f },
                    { -85.0f, -45.0f, 0.0f }
                }};
                const std::size_t anchorCount = dungeon->packAnchors.size();
                for (std::uint32_t p = 0; p < totalPacks; ++p) {
                    auto* anchor = dungeon->packAnchors[p % anchorCount];
                    const float wrapShift = static_cast<float>((p / anchorCount) * 110);
                    for (std::size_t m = 0; m < kPackOffsets.size(); ++m) {
                        RE::NiPoint3 off = kPackOffsets[m];
                        off.x += wrapShift;
                        auto* npc = dungeon->packNpcs[(p * kPackOffsets.size() + m) % dungeon->packNpcs.size()];
                        if (SpawnMapNpcAtAnchor(player, anchor, npc, off, snap.runId)) {
                            packSpawned = true;
                        }
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
                        g_deathEscapeExitElapsedSec = 0.0;
                        g_deathEscapeInFlight.store(false, std::memory_order_release);
                        RefreshAtomicsLocked();
                    }
                    refunded = RefundEntryOnMainThread(player, snap.tier, snap.isArena4, tracked);
                    cancelled = true;
                });
                if (cancelled) {
                    FinishCancelledRunOnMainThread(player, snap, true, refunded,
                        boss ? "무리 생성에 실패해 회차를 취소했습니다." : "보스 생성에 실패해 회차를 취소했습니다.");
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
                if (!survivedMap) {
                    RefreshMapActorsLocked();
                }
                snap = g_activeMap;
                g_deathEscapeExitElapsedSec = 0.0;
            }

            (void)TeleportPlayerToSavedReturn(player, snap);

            if (!survivedMap) {
                CleanupSpawnedRefsOnMainThread();
                {
                    std::scoped_lock lk(g_mutex);
                    SetActiveMapLocked({});
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
        if (const auto& reason = MapPackRuntime::UnavailableReason(); !reason.empty()) {
            return { false, reason };
        }
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
        const auto& pack = MapPackRuntime::Manifest();
        const auto* dungeon = pack.ForTier(tier);
        const auto* forms = MapPackRuntime::FindForms(dungeon);
        if (!forms) {
            return { false, "지도 티어에 해당하는 복제 던전을 찾을 수 없습니다." };
        }
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
            next.dungeonKey = dungeon->key;
            next.packVersion = pack.version;
            SetActiveMapLocked(next, consumedTrackedRecord);
            g_spawnedForCurrentRun = false;
            g_pendingSpawnRunId = next.runId;
            g_arenaPhaseInvulnSec = 0.0;
            g_deathEscapeExitElapsedSec = 0.0;
            g_deathEscapeInFlight.store(false, std::memory_order_release);
            RefreshAtomicsLocked();
        }

        if (!TeleportPlayerToDungeonEntry(player, *forms)) {
            {
                std::scoped_lock lk(g_mutex);
                SetActiveMapLocked({});
                g_spawnedForCurrentRun = false;
                g_pendingSpawnRunId = 0;
                RefreshAtomicsLocked();
            }
            if (!RefundEntryOnMainThread(player, tier, false, consumedTrackedRecord)) {
                return { false, "지도 던전 입구로 이동하지 못했고 소모한 지도를 돌려주지 못했습니다." };
            }
            return { false, "지도 던전 입구 마커로 이동하지 못했습니다." };
        }

        SyncMapLayer2ToStatEngine();
        const auto msg = std::format("T{} {} 입장 (Lv.{}, 남은 포털 {}/{})",
            tier, dungeon->name, Core::MapSystem::RewardMonsterLevel(next), next.portalsRemaining, Core::kNormalMapMaxPortals);
        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        SKSE::log::info("MapDeviceRuntime: opened T{} map '{}' (cell=0x{:08X}, portals={}/{}).",
            tier, dungeon->key, forms->cell->GetFormID(), next.portalsRemaining, Core::kNormalMapMaxPortals);
        return { true, msg };
    }

    MapActionResult MapDeviceRuntime::OpenArena4(RE::PlayerCharacter* player)
    {
        if (const auto& reason = MapPackRuntime::UnavailableReason(); !reason.empty()) {
            return { false, reason };
        }
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

        const auto& pack = MapPackRuntime::Manifest();
        const auto* arena = MapPackRuntime::FindForms(&pack.arena);
        if (!arena) {
            return { false, "복제 결투장을 찾을 수 없습니다." };
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
            next.dungeonKey = pack.arena.key;
            next.packVersion = pack.version;
            SetActiveMapLocked(next);
            g_spawnedForCurrentRun = false;
            g_pendingSpawnRunId = next.runId;
            g_arenaPhaseInvulnSec = 0.0;
            g_deathEscapeExitElapsedSec = 0.0;
            g_deathEscapeInFlight.store(false, std::memory_order_release);
            RefreshAtomicsLocked();
        }

        if (!TeleportPlayerToDungeonEntry(player, *arena)) {
            {
                std::scoped_lock lk(g_mutex);
                SetActiveMapLocked({});
                g_spawnedForCurrentRun = false;
                g_pendingSpawnRunId = 0;
                RefreshAtomicsLocked();
            }
            if (!RefundEntryOnMainThread(player, next.tier, true, std::nullopt)) {
                return { false, "4차 전직 결투장 입구로 이동하지 못했고 공물을 돌려주지 못했습니다." };
            }
            return { false, "4차 전직 결투장 입구로 이동하지 못했습니다." };
        }

        SyncMapLayer2ToStatEngine();
        const auto msg = std::format("4차 전직 결투장 {} 입장 (단판 1포털 · 3페이즈 보스전)", pack.arena.name);
        RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
        SKSE::log::info("MapDeviceRuntime: opened Arena4 '{}' (cell=0x{:08X}, rewardLv={}).",
            pack.arena.key, arena->cell->GetFormID(), Core::kArena4MonsterLevel);
        return { true, msg };
    }

    MapActionResult MapDeviceRuntime::ReenterActiveMap(RE::PlayerCharacter* player)
    {
        if (const auto& reason = MapPackRuntime::UnavailableReason(); !reason.empty()) {
            return { false, reason };
        }
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
        const MapPackRuntime::DungeonForms* forms = nullptr;
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active || g_activeMap.failed) {
                return { false, "재입장 가능한 활성 지도가 없습니다." };
            }
            forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(g_activeMap));
            if (!forms) {
                return { false, "저장된 회차와 일치하는 복제 던전을 찾을 수 없습니다." };
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

        if (!TeleportPlayerToDungeonEntry(player, *forms)) {
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
            SetActiveMapLocked({});
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

    void MapDeviceRuntime::OnActorKilled(RE::FormID actorId, RE::Actor* victim, std::uint64_t runId)
    {
        if (actorId == 0 || runId == 0 || g_uninstallCleanupArmed.load(std::memory_order_acquire)) {
            return;
        }
        bool isArena4 = false;
        std::uint8_t tier = 0;
        {
            std::scoped_lock lk(g_mutex);
            if (!g_activeMap.active || g_activeMap.runId != runId ||
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
        std::uint64_t runId = 0;
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
        return g_activeMap;
    }

    void MapDeviceRuntime::RestoreFromSave(Core::ActiveMapState saved, RE::FormID legacyCell)
    {
        std::scoped_lock lk(g_mutex);
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

        if (player) {
            std::optional<IncompatibleSavedRun> cancelled;
            std::optional<bool> refunded;
            SaveLoad::RunSerializedTransaction([&]() {
                {
                    std::scoped_lock lk(g_mutex);
                    cancelled = std::exchange(g_incompatibleSavedRun, std::nullopt);
                }
                // 보스를 처치한 회차는 보상(아틀라스·첫 공물·4차 해금)을 이미 받았으므로 소모품을 돌려주지 않는다.
                // 돌려주면 팩 판이 바뀔 때마다 보상을 받고 지도까지 돌려받는 복제가 생긴다.
                if (cancelled && !cancelled->state.bossKilled) {
                    const auto& snap = cancelled->state;
                    refunded = RefundEntryOnMainThread(player, snap.tier, snap.isArena4, RecreateSavedMapRefund(snap));
                }
            });
            if (cancelled) {
                const auto& snap = cancelled->state;
                const auto& pack = MapPackRuntime::Manifest();
                const auto* dungeon = pack.Find(snap.dungeonKey);
                const bool inLegacyCell = cell && cancelled->legacyCell != 0 && cell->GetFormID() == cancelled->legacyCell;
                const bool inSavedDungeon = cell && dungeon && cell == MapPackRuntime::ResolveCell(*dungeon);
                // 키를 찾았어도 그 키의 셀이 바뀌었을 수 있으므로 팩 소속 셀 검사를 함께 한다.
                // 현재 셀을 모르면 출구 없는 셀에 남지 않도록 귀환시킨다.
                const bool returnPlayer = !cell || inSavedDungeon || inLegacyCell || MapPackRuntime::OwnsCell(cell);
                const auto& packError = MapPackRuntime::UnavailableReason();
                const auto reason = packError.empty()
                    ? std::string("저장된 던전 키나 팩 판이 현재 지도 팩과 달라 회차를 종료했습니다.")
                    : std::format("{} 저장된 회차를 종료했습니다.", packError);
                FinishCancelledRunOnMainThread(player, snap, returnPlayer, refunded, reason);
            }
        }

        bool isArena4 = false;
        RE::FormID bossId = 0;
        std::uint8_t arenaPhase = 1;
        Core::MapModSummary summaryCopy{};
        {
            std::scoped_lock lk(g_mutex);
            if (g_activeMap.active) {
                const auto* forms = MapPackRuntime::FindForms(MapPackRuntime::Manifest().ForRun(g_activeMap));
                const bool inCell = forms && cell == forms->cell;
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
