#include "PCH.h"
#include "runtime/SaveLoad.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/AutoTest.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/ModState.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/ProxySpellRuntime.h"
#include "runtime/UniqueIdUtil.h"
#include "runtime/WorldDropTracker.h"

#include "core/ItemAffixParser.h"
#include "core/MapSave.h"
#include "core/PassiveStatParser.h"
#include "core/PassiveTree.h"

#include <REX/W32/KERNEL32.h>

#include <cstring>
#include <numeric>
#include <unordered_set>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        using namespace Core::SaveRecord;

        // 결정 166의 MAPS v1을 불러올 때 귀환 여부를 판정하는 바닐라 셀 전용 표다.
        constexpr std::array<RE::FormID, 5> kLegacyMapsV1Cells{
            0x000B6BE6, 0x00043FAB, 0x00015224, 0x000152B1, 0x0001521C
        };
        constexpr RE::FormID kLegacyMapsV1ArenaCell = 0x00015211;

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::Config* g_config = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;
        bool g_installed = false;

        // 저장·로드·리버트와 콘텐츠 검사 결과를 직렬화한다. g_txMutex보다 먼저 잡는다.
        std::mutex g_cosaveMutex;
        std::optional<Core::SaveGeneration> g_currentGeneration;
        std::optional<Core::SaveGeneration> g_testGeneration;
        std::optional<Core::SaveGeneration> g_worldGeneration;
        std::optional<Core::SaveGeneration> g_pendingLoadGeneration;
        AutoTestMarker::CleanupLoadRequest g_cleanupLoadRequest;
        bool g_thisLoadUsesTest = false;
        Core::SaveRecordTransition g_cosave;
        MigrationRuntime g_migration;
        std::shared_ptr<const MigrationDiagnostics> g_lastMigration;
        std::uint64_t g_loadWorld = 0;
        SaveLoad::LoadDiagnostics g_lastLoad;
        SaveLoad::SaveDiagnostics g_lastSave;
        bool g_notifyOnLoad = false;
        constexpr auto kIncompleteCosaveNotice =
            "모드 기록을 완전히 읽지 못했습니다. 이 슬롯에 저장하면 기록이 줄어들 수 있으니 다른 슬롯에 저장하세요.";

        // 잠금 순서 고정: g_txMutex -> g_stateMutex / GemInventory::g_mutex (결정 137(ㅁ))
        std::mutex g_txMutex;
        std::mutex g_stateMutex;
        PersistentState g_state;
        SaveLoad::LoadedMapRecordVersions g_loadedMapRecordVersions;
        std::mutex g_revertCbMutex;
        std::vector<std::function<void()>> g_revertCallbacks;
        // Q-1: 세이브 시점에 잔존하던 NPC kTemporary SpeedMult 델타(로드 후 주 갱신에서 안전 원복)
        std::mutex g_speedModMutex;
        std::vector<std::pair<RE::FormID, float>> g_pendingSpeedModReverts;
        double g_speedRevertRetryAccumSec = 0.0;
        constexpr double kSpeedRevertRetryIntervalSec = 2.0;
        constexpr std::size_t kMaxPendingSpeedReverts = 1024;

        // An absent static actor may return later. A vanished dynamic actor's ID may be reused.
        std::optional<MigrationSpeedResult> TryRevertSpeed(RE::FormID actorId, float delta)
        {
            using enum MigrationSpeedResult;
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
            if (!actor) {
                return (actorId >> 24) == 0xFF ? std::optional{ Lost } : std::nullopt;
            }
            if (actor->IsPlayerRef() || actor->IsDeleted()) {
                return Lost;
            }
            auto* av = actor->AsActorValueOwner();
            if (!av) {
                return std::nullopt;
            }
            constexpr auto temporary = RE::ACTOR_VALUE_MODIFIER::kTemporary;
            if (std::abs(delta) < 0.01f) {
                return AlreadyZero;
            }
            av->ModActorValue(temporary, RE::ActorValue::kSpeedMult, -delta);
            av->ModActorValue(temporary, RE::ActorValue::kCarryWeight, 0.01f);
            av->ModActorValue(temporary, RE::ActorValue::kCarryWeight, -0.01f);
            return Reverted;
        }

        bool QueueSpeedRevert(RE::FormID actorId, float delta, bool rejectAtCapacity)
        {
            if (actorId == 0 || !std::isfinite(delta) || std::abs(delta) < 0.01f) {
                return false;
            }
            if ((actorId >> 24) == 0xFF) {
                SKSE::log::info("SaveLoad: not deferring SpeedMult revert for vanished dynamic actor {:08X} (FormID may be reused).", actorId);
                return false;
            }
            std::scoped_lock lock(g_speedModMutex);
            if (g_pendingSpeedModReverts.size() >= kMaxPendingSpeedReverts) {
                if (rejectAtCapacity) {
                    return false;
                }
                SKSE::log::warn("SaveLoad: pending SpeedMult reverts exceeded {}; dropping the oldest.", kMaxPendingSpeedReverts);
                g_pendingSpeedModReverts.erase(g_pendingSpeedModReverts.begin());
            }
            g_pendingSpeedModReverts.emplace_back(actorId, delta);
            return true;
        }

        MigrationSpeedResult RevertLegacySpeed(const Core::MigrationSpeedMod& mod)
        {
            using enum MigrationSpeedResult;
            const auto delta = static_cast<float>(mod.delta);
            const auto result = TryRevertSpeed(mod.actorFormId, delta);
            if (result && *result != Lost) {
                return *result;
            }
            if (!result && QueueSpeedRevert(mod.actorFormId, delta, true)) {
                return Deferred;
            }
            SKSE::log::warn("SaveLoad: abandoned legacy SpeedMult revert for actor {:08X}.", mod.actorFormId);
            return Lost;
        }

        // 재검수 R-9: 맡겨 둔 SpeedMult 원복을 지금 찾을 수 있는 액터에 적용한다(주 갱신 스레드 전용).
        // 목록을 빼내지 않고 복사해서 처리한 뒤, 끝난 항목(원복했거나 버릴 항목)만 잠금 아래에서 지운다.
        // 그래서 처리 도중 저장(OnSave)이 끼어들어도 아직 원복하지 않은 항목이 SPDM에서 빠지지 않는다(Codex 지적 9).
        // 반환값: (원복한 액터 수, 아직 찾지 못해 남은 수)
        std::pair<std::size_t, std::size_t> ApplyPendingSpeedRevertsNow()
        {
            std::vector<std::pair<RE::FormID, float>> speedReverts;
            {
                std::scoped_lock lock(g_speedModMutex);
                speedReverts = g_pendingSpeedModReverts;
            }
            if (speedReverts.empty()) {
                return { 0, 0 };
            }
            std::vector<std::pair<RE::FormID, float>> resolved;
            std::size_t revertedActors = 0;
            std::size_t remaining = 0;
            for (const auto& entry : speedReverts) {
                const auto& [actorId, appliedDelta] = entry;
                const auto outcome = TryRevertSpeed(actorId, appliedDelta);
                if (!outcome) {
                    ++remaining;
                    continue;
                }
                resolved.push_back(entry);
                revertedActors += *outcome == MigrationSpeedResult::Reverted;
            }
            if (!resolved.empty()) {
                std::scoped_lock lock(g_speedModMutex);
                for (const auto& done : resolved) {
                    // 같은 (액터, 델타) 항목을 하나씩 지운다. 그사이 OnRevert가 목록을 비웠으면 지울 것이 없다.
                    const auto it = std::find(g_pendingSpeedModReverts.begin(), g_pendingSpeedModReverts.end(), done);
                    if (it != g_pendingSpeedModReverts.end()) {
                        g_pendingSpeedModReverts.erase(it);
                    }
                }
            }
            return { revertedActors, remaining };
        }

        struct RecordReader
        {
            SKSE::SerializationInterface* serialization;
            std::span<const std::uint8_t> remaining;

            bool ResolveFormID(std::uint32_t raw, std::uint32_t& resolved) const
            {
                return serialization->ResolveFormID(raw, resolved);
            }
        };

        bool ReadExact(RecordReader* reader, void* dst, std::uint32_t size)
        {
            if (size > reader->remaining.size()) {
                return false;
            }
            if (size != 0) {
                std::memcpy(dst, reader->remaining.data(), size);
                reader->remaining = reader->remaining.subspan(size);
            }
            return true;
        }

        template <class T>
        bool ReadExactPod(RecordReader* intfc, T& out)
        {
            return ReadExact(intfc, &out, static_cast<std::uint32_t>(sizeof(T)));
        }

        bool ReadString(RecordReader* intfc, std::string& out)
        {
            return Core::ReadSaveString(
                [intfc](void* data, std::uint32_t size) { return ReadExact(intfc, data, size); }, out);
        }

        void CollectMaxInstanceId(const Core::GemInstance& gem, std::uint64_t& maxId, std::uint32_t depth = 0)
        {
            if (depth > kMaxGemDepth) {
                return;
            }
            if (gem.instanceId > maxId) {
                maxId = gem.instanceId;
            }
            for (const auto& child : gem.socketedGems) {
                CollectMaxInstanceId(child, maxId, depth + 1);
            }
        }

        bool ReadAffixList(RecordReader* intfc, std::vector<Core::RolledAffix>& out)
        {
            return Core::ReadSaveAffixList(
                [intfc](void* data, std::uint32_t size) { return ReadExact(intfc, data, size); }, out);
        }

        struct ReadPoEItemResult
        {
            bool streamOk = false;
            bool baseResolved = false;
            RE::FormID rawBaseFormId = 0;
        };

        ReadPoEItemResult ReadPoEItemRecord(
            RecordReader* intfc,
            Core::PoEItemRecord& out,
            std::uint32_t itemVersion)
        {
            ReadPoEItemResult r{};
            std::uint8_t rarityByte = 0;
            std::uint32_t iLvl = 1;
            std::uint8_t slotGroupByte = 0;
            std::uint8_t defSubByte = 0;
            std::uint8_t amuletByte = 0;
            std::string weaponTag;

            bool ok = true;
            ok = ok && ReadExactPod(intfc, r.rawBaseFormId);
            ok = ok && ReadExactPod(intfc, rarityByte);
            ok = ok && ReadExactPod(intfc, iLvl);
            ok = ok && ReadExactPod(intfc, slotGroupByte);
            ok = ok && ReadExactPod(intfc, defSubByte);
            ok = ok && ReadExactPod(intfc, amuletByte);
            ok = ok && ReadString(intfc, weaponTag);
            ok = ok && ReadAffixList(intfc, out.affixes);

            if (ok && itemVersion >= 2) {
                std::uint8_t jColor = 0;
                ok = ok && ReadString(intfc, out.uniqueId);
                ok = ok && ReadExactPod(intfc, jColor);
                ok = ok && ReadAffixList(intfc, out.implicitAffixes);
                if (ok) {
                    out.jewelColor = std::min<std::uint8_t>(jColor, 2);
                }
            }
            if (!ok) {
                return r;
            }

            r.streamOk = true;
            out.rarity = rarityByte <= static_cast<std::uint8_t>(Core::ItemRarity::Unique)
                ? static_cast<Core::ItemRarity>(rarityByte)
                : Core::ItemRarity::Normal;
            out.itemLevel = std::clamp<std::uint32_t>(iLvl, 1, Core::kMaxExileLevel);
            out.slotGroup = slotGroupByte < Core::kTokenSlotGroupCount
                ? static_cast<Core::TokenSlotGroup>(slotGroupByte)
                : Core::TokenSlotGroup::OneHandWeapon;
            out.defenseSubtype = defSubByte <= static_cast<std::uint8_t>(Core::DefenseSubtype::EnergyShield)
                ? static_cast<Core::DefenseSubtype>(defSubByte)
                : Core::DefenseSubtype::None;
            out.isAmulet = (amuletByte != 0);
            out.weaponTag = std::move(weaponTag);

            RE::FormID resolvedBase = 0;
            if (intfc->ResolveFormID(r.rawBaseFormId, resolvedBase) && resolvedBase != 0) {
                out.baseFormId = resolvedBase;
                r.baseResolved = true;
            }
            return r;
        }

        Core::Layer1Input BuildLayer1FromState(const PersistentState& state)
        {
            Core::Layer1Input l1;
            l1.exileLevel = std::clamp<std::uint32_t>(state.exileLevel, 1, Core::kMaxExileLevel);
            if (state.tree.classSelected && g_treeData && g_treeData->IsLoaded()) {
                if (const auto* cls = g_treeData->FindClass(state.tree.classId)) {
                    l1.className = cls->name;
                } else {
                    l1.className = state.className.empty() ? "Scion" : state.className;
                }
                l1.passives = Core::PassiveStatParser::AggregateTree(*g_treeData, state.tree);
            } else {
                l1.className = state.className.empty() ? "Scion" : state.className;
            }
            l1.slots = state.slots;
            // 검수 C5: 단일 Layer1 조합 지점 — 장비 스냅샷을 항상 병합해 레벨업/패시브/젬 변경 시 덮어쓰기를 방지
            l1.equippedGear = EquipmentSync::GetCurrentGearSnapshot();

            // 오라 토글, 정신력 보상 이정표 비트(결정 203), 패시브 주얼 소켓 선합산
            l1.activeAuraMask = state.activeAuraMask;
            l1.achievementMask = state.tree.ascendancyMilestonesBitmask;
            if (g_itemData && g_itemData->IsLoaded() && !state.socketedJewels.empty()) {
                l1.socketedJewelStats.reserve(state.socketedJewels.size());
                for (const auto& [nodeId, rec] : state.socketedJewels) {
                    if (state.tree.IsAllocated(nodeId)) {
                        l1.socketedJewelStats.push_back(Core::ItemAffixParser::ParseItem(rec, *g_itemData));
                    }
                }
            }
            return l1;
        }

        void UpdateStatLayerFromState()
        {
            if (g_statEngine) {
                g_statEngine->SetLayer1(BuildLayer1FromState(SaveLoad::GetStateCopy()));
            }
        }

        void ResetMemoryState(bool afterCleanup = false)
        {
            if (g_queue) {
                (void)g_queue->BumpGenerationAndClear();
            }
            {
                std::scoped_lock lock(g_stateMutex);
                g_state = PersistentState{};
                g_state.starterGemsGiven = afterCleanup;
                g_state.starterStage5GemsGiven = afterCleanup;
                g_state.starterCurrencyGiven = afterCleanup;
                g_loadedMapRecordVersions = {};
            }
            GemInventory::Clear();
            ItemTracker::Clear();
            EquipmentSync::Clear();
            MapDeviceRuntime::ResetOnRevert(afterCleanup);
            if (g_charmSystem) {
                g_charmSystem->ResetOnRevert();
            }
            // The just-transferred WDRP/SPDM and map cleanup requests belong to the new state.
            if (afterCleanup) {
                return;
            }
            WorldDropTracker::Clear();
            {
                std::scoped_lock lock(g_speedModMutex);
                g_pendingSpeedModReverts.clear();
                g_speedRevertRetryAccumSec = 0.0;
            }
            if (g_statEngine) {
                g_statEngine->SetLayer1(Core::Layer1Input{});
                g_statEngine->SetLayer2(Core::Layer2Input{});
            }
            std::vector<std::function<void()>> cbs;
            {
                std::scoped_lock lock(g_revertCbMutex);
                cbs = g_revertCallbacks;
            }
            for (const auto& cb : cbs) {
                if (cb) {
                    try {
                        cb();
                    } catch (...) {
                    }
                }
            }
        }

        // SKSE 코세이브 콜백: 되돌리기(순수 메모리 정리만 수행, 엔진 호출 금지)
        void OnRevert(SKSE::SerializationInterface*)
        {
            std::scoped_lock lock(g_cosaveMutex);
            g_migration.Reset();
            g_lastMigration.reset();
            g_cosave.Revert();
            if (!g_currentGeneration) {
                return;
            }
            ResetMemoryState();
            if (!g_cosave.IsLoading()) {
                g_worldGeneration = g_currentGeneration;
                g_pendingLoadGeneration.reset();
                g_thisLoadUsesTest = false;
                g_notifyOnLoad = false;
                ModState::Set(ModMode::Normal, "새 세계에서 모드 상태를 초기화했습니다.");
            }
            SKSE::log::info("SaveLoad: OnRevert cleared persistent state, gem keys, and command queue.");
        }

        void ReportWriteWarning(std::string_view message)
        {
            SKSE::log::warn("SaveLoad: {}", message);
        }

        Snapshot CaptureSaveSnapshot()
        {
            // 결정 137(ㅁ), 검수 M4: g_txMutex 아래 상태·젬·아이템·바닥드랍 일관 스냅샷을 원자 캡처한다.
            PersistentState state;
            std::vector<TrackedGemEntry> tracked;
            std::vector<TrackedItemEntry> trackedItems;
            std::vector<SavedWorldDropEntry> savedWorldDrops;
            Core::ActiveMapState savedMapState;
            {
                std::scoped_lock txLock(g_txMutex);
                state = SaveLoad::GetStateCopy();
                if (g_charmSystem) {
                    state.charmSlots = g_charmSystem->Slots();
                }
                tracked = GemInventory::TakeSnapshotForSave();
                trackedItems = ItemTracker::TakeSnapshotForSave();
                savedWorldDrops = WorldDropTracker::TakeSnapshotForSave();
                savedMapState = MapDeviceRuntime::TakeSnapshotForSave();
            }
            // Q-1 + M-2: 세이브 시점에 적용 중인 NPC kTemporary SpeedMult 델타 + 아직 미원복 잔여(미로드 NPC) 합산 스냅샷
            std::unordered_map<RE::FormID, float> mergedSpeedMods;
            {
                std::scoped_lock lock(g_speedModMutex);
                for (const auto& [fid, d] : g_pendingSpeedModReverts) {
                    mergedSpeedMods[fid] += d;
                }
            }
            for (const auto& [fid, d] : AilmentRuntime::TakeActiveSpeedDeltasForSave()) {
                mergedSpeedMods[fid] += d;
            }
            for (const auto& [fid, d] : MonsterRuntime::TakeActiveSpeedDeltasForSave()) {
                mergedSpeedMods[fid] += d;
            }
            std::vector<std::pair<RE::FormID, float>> savedSpeedMods;
            savedSpeedMods.reserve(mergedSpeedMods.size());
            for (const auto& [fid, d] : mergedSpeedMods) {
                if (fid != 0 && std::isfinite(d) && std::abs(d) >= 0.01f) {
                    savedSpeedMods.emplace_back(fid, d);
                }
            }
            std::sort(savedSpeedMods.begin(), savedSpeedMods.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            return { std::move(state), std::move(tracked), std::move(trackedItems),
                std::move(savedWorldDrops), std::move(savedMapState), std::move(savedSpeedMods) };
        }

        // SKSE 코세이브 콜백: 저장(메모리 스냅샷만 기록, 예약 큐는 저장하지 않음).
        void OnSave(SKSE::SerializationInterface* intfc)
        {
            if (!intfc) {
                return;
            }
            bool complete = false;
            bool replay = false;
            {
                std::scoped_lock lock(g_cosaveMutex);
                replay = g_cosave.IsLoading() || !ModState::AllowsGameplay() || !g_worldGeneration;
                SaveLoad::SaveDiagnostics diagnostics{ .sequence = g_lastSave.sequence + 1 };
                diagnostics.threadId = REX::W32::GetCurrentThreadId();
                SKSE::log::info("SaveLoad: OnSave #{} thread={}, replay={}.", diagnostics.sequence, diagnostics.threadId, replay);
                const auto emit = [intfc, replay, &diagnostics](const Core::RawSaveRecord& record) {
                    const bool written = Core::WriteSaveRecord(*intfc, record);
                    if (written) {
                        diagnostics.wroteGeneration |= record.type == kRecordGeneration;
                        try {
                            diagnostics.recordTypes.push_back(record.type);
                            if (record.type == kRecordGeneration) {
                                auto decoded = Core::ReadGenerationRecord(record.version, record.bytes);
                                if (decoded.status == Core::GenerationRecordStatus::Valid) {
                                    diagnostics.generation = std::move(decoded.value);
                                }
                            }
                        } catch (...) {
                            // Diagnostic allocation must not interrupt writing the save.
                            diagnostics.generation.reset();
                        }
                    }
                    if (!written) {
                        if (replay) {
                            ++diagnostics.failedRecords;
                        }
                        SKSE::log::error("SaveLoad: failed to write record 0x{:08X} v{}.", record.type, record.version);
                    }
                    return written;
                };
                try {
                    if (replay) {
                        // Raw replay retains the old FormID indexes if the load order changed (contract 4.6).
                        complete = Core::ReplaySaveRecords(g_cosave.RecordsForSave(), emit);
                    } else {
                        complete = WriteSnapshot(CaptureSaveSnapshot(), *g_worldGeneration, emit,
                            [&](std::string_view warning) {
                                ++diagnostics.failedRecords;
                                ReportWriteWarning(warning);
                            });
                        // WriteSnapshot reports each failed record through the warning sink, except for the final GENR emit.
                        if (!complete && diagnostics.failedRecords == 0) {
                            ++diagnostics.failedRecords;
                        }
                    }
                } catch (const std::exception& e) {
                    SKSE::log::error("SaveLoad: co-save write exception: {}", e.what());
                } catch (...) {
                    SKSE::log::error("SaveLoad: unknown co-save write exception.");
                }
                diagnostics.complete = complete;
                g_lastSave = std::move(diagnostics);
            }
            if (!complete) {
                const auto* notice = replay ?
                    "SkyrimPoE 보존 기록을 모두 저장하지 못했습니다. 이전 세이브를 보관하세요." :
                    "SkyrimPoE 모드 기록을 모두 저장하지 못했습니다. 이전 세이브를 보관하세요.";
                SKSE::log::error("SaveLoad: co-save write incomplete (replay={}).", replay);
                if (const auto* tasks = SKSE::GetTaskInterface()) {
                    tasks->AddTask([notice] { RE::SendHUDMessage::ShowHUDMessage(notice); });
                }
            }
        }

        // 세대 판정을 통과한 뒤 기존 판별 순서대로 메모리를 복원한다.
        void RestoreLoadedRecords(SKSE::SerializationInterface* serialization)
        {
            PersistentState loadedState;
            bool hasCore = false;
            bool sawGemKeysRecord = false;  // Codex 지적 16: CORE만 손상되고 GKEY만 남은 세이브도 "이미 지급받은 세이브"로 본다
            Core::PassiveTreeState loadedTree;
            bool hasTree = false;
            bool hasItems = false;
            bool hasSpirit = false;
            // 재검수 R-14: 레코드가 있었지만 읽지 못한 경우(손상·미지원 판)를 "없던 옛 세이브"와 구별한다.
            bool sawItemsRecord = false;
            bool sawSpiritRecord = false;
            bool itemStarterFlagRead = false;  // Codex 지적 15: ITEM 머리의 시작 화폐 플래그만이라도 읽었는가
            bool hasGear = false;
            bool loadedStarterCurrencyGiven = false;
            std::uint16_t loadedActiveAuraMask = 0;
            bool loadedStarterStage5GemsGiven = false;
            Core::InventoryItemKey loadedRing2Key{};
            RE::FormID loadedRing2Base = 0;
            Core::InventoryItemKey loadedBeltKey{};
            RE::FormID loadedBeltBase = 0;
            std::array<Core::CharmSlotState, Core::kMaxCharmSlots> loadedCharms{};
            std::unordered_map<std::uint32_t, Core::PoEItemRecord> loadedJewels;
            std::vector<TrackedGemEntry> loadedGems;
            std::vector<TrackedItemEntry> loadedItems;
            std::vector<SavedWorldDropEntry> loadedWorldDrops;
            std::vector<std::pair<RE::FormID, float>> loadedSpeedMods;
            bool sawAtlasRecord = false;
            std::optional<Core::AtlasState> atlasRecord;
            std::optional<Core::MapSaveRecord> mapsRecord;
            SaveLoad::LoadedMapRecordVersions mapVersions;

            for (const auto& record : g_cosave.current.records) {
                const auto& [type, version, bytes] = record;
                const auto length = static_cast<std::uint32_t>(bytes.size());
                RecordReader reader{ serialization, bytes };
                auto* intfc = &reader;
                if (type == kRecordCore) {
                    if (version != kCoreVersion) {
                        SKSE::log::warn("SaveLoad: unsupported CORE record version {} (expected {}).", version, kCoreVersion);
                        continue;
                    }
                    PersistentState tmp;
                    if (ReadCoreV2(intfc->remaining, tmp)) {
                        loadedState = std::move(tmp);
                        hasCore = true;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse CORE record (truncated or corrupt); falling back to defaults.");
                    }
                } else if (type == kRecordGemKeys) {
                    sawGemKeysRecord = true;
                    if (version != kGemKeysVersion) {
                        SKSE::log::warn("SaveLoad: unsupported GKEY record version {} (expected {}).", version, kGemKeysVersion);
                        continue;
                    }
                    std::uint32_t count = 0;
                    if (!ReadExactPod(intfc, count) || count > kMaxListLen) {
                        SKSE::log::error("SaveLoad: invalid GKEY count.");
                        continue;
                    }
                    std::vector<TrackedGemEntry> tmp;
                    tmp.reserve(count);
                    bool ok = true;
                    for (std::uint32_t i = 0; i < count && ok; ++i) {
                        RE::FormID rawOwner = 0;
                        std::uint16_t uid = 0;
                        std::uint8_t worldByte = 0;
                        Core::GemInstance gem;
                        ok = ok && ReadExactPod(intfc, rawOwner);
                        ok = ok && ReadExactPod(intfc, uid);
                        ok = ok && ReadExactPod(intfc, worldByte);
                        ok = ok && ReadGemInstance(intfc->remaining, gem);
                        if (!ok) {
                            break;
                        }
                        RE::FormID resolvedOwner = 0;
                        if (intfc->ResolveFormID(rawOwner, resolvedOwner) && resolvedOwner != 0) {
                            tmp.push_back(TrackedGemEntry{
                                Core::InventoryItemKey{ resolvedOwner, uid },
                                worldByte != 0,
                                std::move(gem)
                            });
                        } else {
                            SKSE::log::warn("SaveLoad: skipping tracked gem '{}' with unresolvable owner FormID 0x{:08X}.",
                                gem.gemId, rawOwner);
                        }
                    }
                    if (ok) {
                        loadedGems = std::move(tmp);
                    } else {
                        // C-3: GKEY 뒷부분이 잘렸더라도 앞선 완전 파싱 항목은 살려 C-1 고아화를 최소화한다.
                        if (!tmp.empty()) {
                            loadedGems = std::move(tmp);
                        }
                        SKSE::log::error("SaveLoad: failed to parse full GKEY record (truncated or corrupt; recovered {} entries).",
                            loadedGems.size());
                    }
                } else if (type == kRecordTree) {
                    if (version != kTreeVersion) {
                        SKSE::log::warn("SaveLoad: unsupported TREE record version {} (expected {}).", version, kTreeVersion);
                        continue;
                    }
                    Core::PassiveTreeState tmpTree;
                    std::uint8_t clsSelByte = 0;
                    std::uint32_t nodeCount = 0;
                    std::uint32_t masteryCount = 0;
                    bool ok = true;
                    ok = ok && ReadExactPod(intfc, clsSelByte);
                    ok = ok && ReadExactPod(intfc, tmpTree.classId);
                    ok = ok && ReadString(intfc, tmpTree.ascendancyId);
                    ok = ok && ReadExactPod(intfc, nodeCount);
                    if (ok && nodeCount <= kMaxTreeNodes) {
                        tmpTree.allocatedNodes.resize(nodeCount);
                        for (std::uint32_t i = 0; i < nodeCount && ok; ++i) {
                            ok = ok && ReadExactPod(intfc, tmpTree.allocatedNodes[i]);
                        }
                    } else {
                        ok = false;
                    }
                    ok = ok && ReadExactPod(intfc, masteryCount);
                    if (ok && masteryCount <= kMaxListLen) {
                        for (std::uint32_t i = 0; i < masteryCount && ok; ++i) {
                            std::uint32_t mNode = 0;
                            std::uint32_t mEff = 0;
                            ok = ok && ReadExactPod(intfc, mNode);
                            ok = ok && ReadExactPod(intfc, mEff);
                            if (ok) {
                                tmpTree.masterySelections[mNode] = mEff;
                            }
                        }
                    } else {
                        ok = false;
                    }
                    ok = ok && ReadExactPod(intfc, tmpTree.bossKillCount);
                    ok = ok && ReadExactPod(intfc, tmpTree.dragonKillCount);
                    ok = ok && ReadExactPod(intfc, tmpTree.ascendancyMilestonesBitmask);
                    if (ok) {
                        tmpTree.classSelected = (clsSelByte != 0);
                        tmpTree.ascendancyMilestonesBitmask &= 0xFu;
                        loadedTree = std::move(tmpTree);
                        hasTree = true;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse TREE record (truncated or corrupt); falling back to empty tree.");
                    }
                } else if (type == kRecordItems) {
                    sawItemsRecord = true;
                    if (version != 1 && version != kItemsVersion) {
                        SKSE::log::warn("SaveLoad: unsupported ITEM record version {} (expected 1 or {}).", version, kItemsVersion);
                        continue;
                    }
                    std::uint8_t starterCurrByte = 0;
                    std::uint32_t itemCount = 0;
                    if (!ReadExactPod(intfc, starterCurrByte)) {
                        SKSE::log::error("SaveLoad: invalid ITEM header.");
                        continue;
                    }
                    // 플래그는 온전히 읽었으므로 개수가 잘려도 그 값은 쓴다.
                    loadedStarterCurrencyGiven = (starterCurrByte != 0);
                    itemStarterFlagRead = true;
                    if (!ReadExactPod(intfc, itemCount) || itemCount > kMaxItemRecords) {
                        SKSE::log::error("SaveLoad: invalid ITEM item count.");
                        continue;
                    }
                    std::vector<TrackedItemEntry> tmpItems;
                    tmpItems.reserve(itemCount);
                    bool ok = true;
                    for (std::uint32_t i = 0; i < itemCount && ok; ++i) {
                        RE::FormID rawOwner = 0;
                        std::uint16_t uid = 0;
                        std::uint8_t worldByte = 0;
                        ok = ok && ReadExactPod(intfc, rawOwner);
                        ok = ok && ReadExactPod(intfc, uid);
                        ok = ok && ReadExactPod(intfc, worldByte);
                        if (!ok) {
                            break;
                        }
                        Core::PoEItemRecord rec;
                        const auto rr = ReadPoEItemRecord(intfc, rec, version);
                        if (!rr.streamOk) {
                            ok = false;
                            break;
                        }
                        RE::FormID resolvedOwner = 0;
                        if (intfc->ResolveFormID(rawOwner, resolvedOwner) && resolvedOwner != 0 && rr.baseResolved) {
                            tmpItems.push_back(TrackedItemEntry{
                                Core::InventoryItemKey{ resolvedOwner, uid },
                                worldByte != 0,
                                std::move(rec)
                            });
                        } else {
                            SKSE::log::warn("SaveLoad: pruning stale item key owner=0x{:08X} base=0x{:08X}.",
                                rawOwner, rr.rawBaseFormId);
                        }
                    }
                    // 재검수 R-14: 중간에 끊겼으면 끊기기 전까지 읽은 항목은 살린다(항목마다 독립적이라 앞부분은 온전함).
                    // GKEY와 같은 처리. 시작 화폐 플래그는 위에서 이미 반영했다.
                    if (!ok) {
                        SKSE::log::error("SaveLoad: ITEM record truncated/corrupt after {} of {} entries; keeping the parsed prefix.",
                            tmpItems.size(), itemCount);
                    }
                    loadedItems = std::move(tmpItems);
                    hasItems = true;
                } else if (type == kRecordWorldDrops) {
                    if (version != kWorldDropsVersion) {
                        SKSE::log::warn("SaveLoad: unsupported WDRP record version {} (expected {}).", version, kWorldDropsVersion);
                        continue;
                    }
                    std::uint32_t count = 0;
                    if (!ReadExactPod(intfc, count) || count > kMaxWorldDropRecords) {
                        SKSE::log::error("SaveLoad: invalid WDRP count.");
                        continue;
                    }
                    std::vector<SavedWorldDropEntry> tmpWdrp;
                    tmpWdrp.reserve(count);
                    bool ok = true;
                    for (std::uint32_t i = 0; i < count && ok; ++i) {
                        RE::FormID rawDrop = 0, rawCorpse = 0, rawBeam = 0;
                        std::uint8_t tier = 2;
                        ok = ok && ReadExactPod(intfc, rawDrop);
                        ok = ok && ReadExactPod(intfc, rawCorpse);
                        ok = ok && ReadExactPod(intfc, rawBeam);
                        ok = ok && ReadExactPod(intfc, tier);
                        if (!ok) break;

                        RE::FormID resDrop = 0, resCorpse = 0, resBeam = 0;
                        const bool dropOk = intfc->ResolveFormID(rawDrop, resDrop) && resDrop != 0;
                        if (rawBeam != 0 && !intfc->ResolveFormID(rawBeam, resBeam)) {
                            resBeam = 0;  // 검수 m6: 빛기둥만 소실돼도 드랍 본체 회수 추적은 유지
                        }
                        if (rawCorpse != 0) {
                            (void)intfc->ResolveFormID(rawCorpse, resCorpse);
                        }
                        if (dropOk) {
                            tmpWdrp.push_back(SavedWorldDropEntry{ resDrop, resCorpse, resBeam, tier });
                        }
                    }
                    if (ok) {
                        loadedWorldDrops = std::move(tmpWdrp);
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse WDRP record.");
                    }
                } else if (type == kRecordSpirit) {
                    sawSpiritRecord = true;
                    if (version != kSpiritVersion) {
                        SKSE::log::warn("SaveLoad: unsupported SPRT record version {} (expected {}).", version, kSpiritVersion);
                        continue;
                    }
                    std::uint16_t auraMask = 0;
                    std::uint8_t s5GemsByte = 0;
                    if (ReadExactPod(intfc, auraMask)) {
                        loadedActiveAuraMask = static_cast<std::uint16_t>(auraMask & 0x01FFu);
                        hasSpirit = true;
                        if (ReadExactPod(intfc, s5GemsByte)) {
                            loadedStarterStage5GemsGiven = (s5GemsByte != 0);
                        } else {
                            // Codex 지적 15: 오라 마스크는 살리고, 읽지 못한 지급 플래그는 "이미 받음"으로 둔다(중복 지급 방지).
                            loadedStarterStage5GemsGiven = true;
                            SKSE::log::error("SaveLoad: SPRT record truncated after aura mask; keeping mask, suppressing stage-5 starter grant.");
                        }
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse SPRT record.");
                    }
                } else if (type == kRecordGear) {
                    if (version != kGearVersion) {
                        SKSE::log::warn("SaveLoad: unsupported GEAR record version {} (expected {}).", version, kGearVersion);
                        continue;
                    }
                    RE::FormID rawR2Owner = 0, rawR2Base = 0;
                    std::uint16_t r2Uid = 0;
                    RE::FormID rawBeltOwner = 0, rawBeltBase = 0;
                    std::uint16_t beltUid = 0;
                    std::uint32_t charmCount = 0;
                    std::uint32_t jewelCount = 0;
                    bool ok = true;
                    ok = ok && ReadExactPod(intfc, rawR2Owner);
                    ok = ok && ReadExactPod(intfc, r2Uid);
                    ok = ok && ReadExactPod(intfc, rawR2Base);
                    ok = ok && ReadExactPod(intfc, rawBeltOwner);
                    ok = ok && ReadExactPod(intfc, beltUid);
                    ok = ok && ReadExactPod(intfc, rawBeltBase);
                    ok = ok && ReadExactPod(intfc, charmCount);

                    std::array<Core::CharmSlotState, Core::kMaxCharmSlots> tmpCharms{};
                    if (ok && charmCount <= Core::kMaxCharmSlots) {
                        for (std::uint32_t c = 0; c < charmCount && ok; ++c) {
                            std::uint8_t occ = 0;
                            std::uint8_t kindByte = 0;
                            double charges = 0.0;
                            ok = ok && ReadExactPod(intfc, occ);
                            ok = ok && ReadExactPod(intfc, kindByte);
                            ok = ok && ReadExactPod(intfc, charges);
                            if (ok) {
                                tmpCharms[c].occupied = (occ != 0) && (kindByte < Core::kCharmKindCount);
                                tmpCharms[c].kind = (kindByte < Core::kCharmKindCount)
                                    ? static_cast<Core::CharmKind>(kindByte)
                                    : Core::CharmKind::Ruby;
                                tmpCharms[c].charges = std::isfinite(charges)
                                    ? std::clamp(charges, 0.0, Core::CharmSystem::kMaxCharges)
                                    : 0.0;
                            }
                        }
                    } else {
                        ok = false;
                    }

                    std::unordered_map<std::uint32_t, Core::PoEItemRecord> tmpJewels;
                    ok = ok && ReadExactPod(intfc, jewelCount);
                    if (ok && jewelCount <= kMaxSocketedJewels) {
                        for (std::uint32_t j = 0; j < jewelCount && ok; ++j) {
                            std::uint32_t nodeId = 0;
                            ok = ok && ReadExactPod(intfc, nodeId);
                            if (!ok) break;
                            Core::PoEItemRecord jRec;
                            const auto rr = ReadPoEItemRecord(intfc, jRec, 2);
                            if (!rr.streamOk) {
                                ok = false;
                                break;
                            }
                            if (nodeId != 0 && rr.baseResolved) {
                                tmpJewels[nodeId] = std::move(jRec);
                            }
                        }
                    } else {
                        ok = false;
                    }

                    if (ok) {
                        RE::FormID resR2Base = 0;
                        if (rawR2Base != 0 && intfc->ResolveFormID(rawR2Base, resR2Base) && resR2Base != 0) {
                            RE::FormID resR2Owner = 0;
                            if (rawR2Owner == 0 || (intfc->ResolveFormID(rawR2Owner, resR2Owner) && resR2Owner != 0)) {
                                loadedRing2Base = resR2Base;
                                loadedRing2Key = Core::InventoryItemKey{ resR2Owner, r2Uid };
                            }
                        }
                        RE::FormID resBeltBase = 0;
                        if (rawBeltBase != 0 && intfc->ResolveFormID(rawBeltBase, resBeltBase) && resBeltBase != 0) {
                            RE::FormID resBeltOwner = 0;
                            if (rawBeltOwner == 0 || (intfc->ResolveFormID(rawBeltOwner, resBeltOwner) && resBeltOwner != 0)) {
                                loadedBeltBase = resBeltBase;
                                loadedBeltKey = Core::InventoryItemKey{ resBeltOwner, beltUid };
                            }
                        }
                        loadedCharms = tmpCharms;
                        loadedJewels = std::move(tmpJewels);
                        hasGear = true;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse GEAR record.");
                    }
                } else if (type == kRecordSpeedMods) {
                    if (version != kSpeedModsVersion) {
                        SKSE::log::warn("SaveLoad: unsupported SPDM record version {} (expected {}).", version, kSpeedModsVersion);
                        continue;
                    }
                    std::uint32_t spdmCount = 0;
                    if (!ReadExactPod(intfc, spdmCount) || spdmCount > kMaxListLen) {
                        SKSE::log::error("SaveLoad: invalid SPDM count.");
                        continue;
                    }
                    std::vector<std::pair<RE::FormID, float>> tmpSpdm;
                    tmpSpdm.reserve(spdmCount);
                    bool ok = true;
                    for (std::uint32_t i = 0; i < spdmCount && ok; ++i) {
                        RE::FormID rawActor = 0;
                        float delta = 0.0f;
                        ok = ok && ReadExactPod(intfc, rawActor);
                        ok = ok && ReadExactPod(intfc, delta);
                        if (!ok) {
                            break;
                        }
                        RE::FormID resActor = 0;
                        if (rawActor != 0 && std::isfinite(delta) && std::abs(delta) >= 0.01f &&
                            intfc->ResolveFormID(rawActor, resActor) && resActor != 0) {
                            tmpSpdm.emplace_back(resActor, delta);
                        }
                    }
                    if (ok) {
                        loadedSpeedMods = std::move(tmpSpdm);
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse SPDM record.");
                    }
                } else if (type == kRecordAtlas) {
                    sawAtlasRecord = true;
                    const auto candidate = Core::ReadAtlasSave(version, length,
                        [intfc](void* data, std::uint32_t size) { return ReadExact(intfc, data, size); });
                    if (candidate) {
                        atlasRecord = candidate;
                        mapVersions.atlas = version;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse ATLS v{} ({} bytes).", version, length);
                    }
                } else if (type == kRecordMaps) {
                    auto candidate = Core::ReadMapSave(version, length,
                        [intfc](void* data, std::size_t size) { return ReadExact(intfc, data, static_cast<std::uint32_t>(size)); },
                        [intfc](std::uint32_t raw, std::uint32_t& resolved) {
                            return intfc->ResolveFormID(raw, resolved);
                        });
                    if (version == Core::kMapSaveVersion && candidate.run && candidate.run->active) {
                        if (!g_itemData || !g_itemData->IsLoaded()) {
                            SKSE::log::warn("SaveLoad: item data unavailable; skipped validation of saved map affix rolls.");
                        } else if (!Core::MapSystem::AreSavedMapRollsValid(*g_itemData, candidate.run->mods)) {
                            SKSE::log::error("SaveLoad: MAPS run {} has invalid saved affix rolls.", candidate.run->runId);
                            candidate.run.reset();
                        }
                    }
                    if (candidate.run) {
                        mapVersions.maps = version;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse MAPS v{} run ({} bytes).", version, length);
                    }
                    Core::KeepLatestMapSave(mapsRecord, std::move(candidate));
                } else if (type != kRecordGeneration) {
                    SKSE::log::warn("SaveLoad: ignoring unknown record type 0x{:08X}.", type);
                }
            }

            const auto [loadedAtlas, atlasDamaged, runDamaged] = Core::GetMapSaveStatus(sawAtlasRecord, atlasRecord, mapsRecord);
            loadedState.atlas = loadedAtlas;
            if (atlasDamaged) {
                SKSE::log::error("SaveLoad: atlas record missing from map save or unreadable; suppressing duplicate first tribute.");
            }
            if (sawAtlasRecord && !mapsRecord) {
                SKSE::log::error("SaveLoad: MAPS run record missing from map save.");
            }
            if (hasTree) {
                if (g_treeData && g_treeData->IsLoaded()) {
                    Core::PassiveTree::SanitizeLoadedState(*g_treeData, loadedTree);
                }
                loadedState.tree = std::move(loadedTree);
                if (loadedState.tree.classSelected && g_treeData && g_treeData->IsLoaded()) {
                    if (const auto* cls = g_treeData->FindClass(loadedState.tree.classId)) {
                        loadedState.className = cls->name;
                    }
                }
                if (g_treeData && g_treeData->IsLoaded() && g_config) {
                    const auto avail = Core::PassiveTree::ComputeAvailableNormalPoints(
                        *g_treeData, loadedState.tree, loadedState.exileLevel, *g_config,
                        loadedState.atlas.completedTierMask);
                    loadedState.unallocatedPassivePoints = static_cast<std::uint32_t>(std::max<std::int32_t>(0, avail));
                }
            }

            // 복원된 모든 슬롯·추적 젬의 instanceId 최댓값보다 nextGemInstanceId가 항상 크도록 보정한다.
            std::uint64_t maxInstanceId = 0;
            if (hasCore) {
                for (const auto& slot : loadedState.slots) {
                    if (slot.occupied) {
                        CollectMaxInstanceId(slot.gem, maxInstanceId);
                    }
                }
            }
            for (const auto& entry : loadedGems) {
                CollectMaxInstanceId(entry.gem, maxInstanceId);
            }
            if (hasItems) {
                loadedState.starterCurrencyGiven = loadedStarterCurrencyGiven;
            }
            if (hasSpirit) {
                loadedState.activeAuraMask = loadedActiveAuraMask;
                loadedState.starterStage5GemsGiven = loadedStarterStage5GemsGiven;
            }
            // 재검수 R-14: 레코드는 있었는데 읽지 못했다면 그 세이브는 이미 지급을 받은 세이브다. 다시 지급하지 않는다.
            if (sawItemsRecord && !hasItems) {
                // 머리 플래그를 읽었으면 그 값을, 못 읽었으면 "이미 받음"을 쓴다(Codex 지적 15).
                loadedState.starterCurrencyGiven = itemStarterFlagRead ? loadedStarterCurrencyGiven : true;
                SKSE::log::error("SaveLoad: ITEM record present but unreadable; starter currency flag = {} ({}).",
                    loadedState.starterCurrencyGiven, itemStarterFlagRead ? "from header" : "suppressed");
            }
            if (sawSpiritRecord && !hasSpirit) {
                loadedState.starterStage5GemsGiven = true;
                SKSE::log::error("SaveLoad: SPRT record present but unreadable; suppressing duplicate stage-5 starter gem grant.");
            }
            if (hasGear) {
                loadedState.virtualRing2Key = loadedRing2Key;
                loadedState.virtualRing2BaseFormId = loadedRing2Base;
                loadedState.virtualBeltKey = loadedBeltKey;
                loadedState.virtualBeltBaseFormId = loadedBeltBase;
                loadedState.charmSlots = loadedCharms;
                loadedState.socketedJewels = std::move(loadedJewels);
            }
            if (hasCore || hasTree || hasItems || hasSpirit || hasGear || sawAtlasRecord || mapsRecord ||
                sawItemsRecord || sawSpiritRecord || sawGemKeysRecord) {
                // C-3: CORE만 손상되고 다른 SPOE 레코드가 살아 있는 세이브에서 시작 젬·화폐 중복 지급을 막는다.
                if (!hasCore) {
                    loadedState.starterGemsGiven = true;
                    loadedState.starterStage5GemsGiven = true;
                    loadedState.starterCurrencyGiven = true;
                    SKSE::log::error("SaveLoad: CORE record missing/corrupt while sibling SPOE records exist; suppressing duplicate starter grants.");
                }
                if (loadedState.nextGemInstanceId <= maxInstanceId) {
                    loadedState.nextGemInstanceId = maxInstanceId + 1;
                }
                SaveLoad::SetState(loadedState);
            } else if (maxInstanceId > 0) {
                std::scoped_lock lock(g_stateMutex);
                if (g_state.nextGemInstanceId <= maxInstanceId) {
                    g_state.nextGemInstanceId = maxInstanceId + 1;
                }
            }

            if (g_charmSystem) {
                g_charmSystem->SetSlots(loadedState.charmSlots, 0);
            }
            GemInventory::RestoreFromSave(std::move(loadedGems));
            ItemTracker::RestoreFromSave(std::move(loadedItems));
            WorldDropTracker::RestoreFromSave(std::move(loadedWorldDrops));
            if (sawAtlasRecord || mapsRecord) {
                auto loadedRun = mapsRecord && mapsRecord->run ? std::move(*mapsRecord->run) : Core::ActiveMapState{};
                RE::FormID legacyCell = 0;
                if (mapVersions.maps == Core::kLegacyIndexMapSaveVersion && loadedRun.active) {
                    legacyCell = loadedRun.isArena4 ? kLegacyMapsV1ArenaCell :
                        kLegacyMapsV1Cells[mapsRecord->legacyDungeonIndex % kLegacyMapsV1Cells.size()];
                }
                MapDeviceRuntime::RestoreFromSave(std::move(loadedRun), legacyCell, atlasDamaged, runDamaged);
            } else {
                MapDeviceRuntime::ResetOnRevert();
            }
            {
                std::scoped_lock lock(g_stateMutex);
                g_loadedMapRecordVersions = mapVersions;
            }
            {
                std::scoped_lock lock(g_speedModMutex);
                g_pendingSpeedModReverts = std::move(loadedSpeedMods);
            }
            UpdateStatLayerFromState();

            SKSE::log::info(
                "SaveLoad: OnLoad restored CORE({}), TREE(nodes={}), GKEY({}), ITEM({}), WDRP({}), SPRT({}), GEAR(jewels={}), atlas(0x{:04X}), ATLS(v={}), MAPS(v={}).",
                hasCore, loadedState.tree.allocatedNodes.size(),
                GemInventory::CountTracked(), ItemTracker::CountTracked(), WorldDropTracker::ActiveCount(),
                hasSpirit, loadedState.socketedJewels.size(), loadedState.atlas.completedTierMask,
                mapVersions.atlas, mapVersions.maps);
        }

        bool IsPlayable(Core::SaveDisposition disposition)
        {
            return disposition == Core::SaveDisposition::Normal || disposition == Core::SaveDisposition::FirstInstall;
        }

        // Call under g_cosaveMutex, before a playable load discards the raw records.
        void RecordLoadDiagnostics(const Core::SaveDecision& decision) noexcept
        {
            g_lastLoad = { .receivedLoad = g_cosave.receivedLoad,
                .collectionComplete = g_cosave.receivedLoad && g_cosave.current.complete };
            try {
                g_lastLoad.decision = decision;
                Core::GenerationRecord generation;
                bool found = false;
                for (const auto& record : g_cosave.current.records) {
                    if (record.type != kRecordGeneration) {
                        continue;
                    }
                    if (found) {
                        generation.status = Core::GenerationRecordStatus::Damaged;
                        break;
                    }
                    found = true;
                    generation = Core::ReadGenerationRecord(record.version, record.bytes);
                }
                g_lastLoad.generation = std::move(generation);
            } catch (...) {
                // Diagnostic decoding must not change the load decision or restoration.
            }
        }

        void SetSaveDecision(const Core::SaveDecision& decision)
        {
            g_notifyOnLoad = decision.disposition != Core::SaveDisposition::Normal;
            if (IsPlayable(decision.disposition)) {
                ModState::Set(ModMode::Normal, std::string(decision.reason));
                if (decision.disposition == Core::SaveDisposition::FirstInstall) {
                    SKSE::log::info("SaveLoad: {}", decision.reason);
                }
            } else {
                const auto reason = decision.disposition == Core::SaveDisposition::ResumeMigration
                    ? "진행 중인 정리·이전 기록(MIGR)이 있습니다." : decision.reason;
                ModState::Set(ModMode::Preserve, reason);
                SKSE::log::warn("SaveLoad: preserving co-save. {}", reason);
            }
            RecordLoadDiagnostics(decision);
        }

        // SKSE 코세이브 콜백: 버퍼를 수집하고 판정한 뒤에만 기존 로더가 팩을 참조한다.
        void OnLoad(SKSE::SerializationInterface* intfc)
        {
            if (!intfc) {
                return;
            }
            std::scoped_lock lock(g_cosaveMutex);
            g_migration.Reset();
            g_lastMigration.reset();
            g_pendingLoadGeneration.reset();
            if (!g_cosave.IsLoading() && g_currentGeneration) {
                ++g_loadWorld;
                g_thisLoadUsesTest = false;
                ModState::Set(ModMode::Preserve, "불러오는 중");
            }
            g_cosave.receivedLoad = true;
            g_cosave.current = Core::CollectSaveRecords(
                [intfc](auto& type, auto& version, auto& length) { return intfc->GetNextRecordInfo(type, version, length); },
                [intfc](void* data, std::uint32_t size) { return intfc->ReadRecordData(data, size); });
            if (!g_cosave.current.complete) {
                SKSE::log::warn("SaveLoad: co-save collection incomplete, retaining {} complete record(s).",
                    g_cosave.current.records.size());
            }
            const auto* target = Core::SelectSaveTarget(g_currentGeneration, g_testGeneration, g_thisLoadUsesTest);
            if (!target) {
                return;
            }
            try {
                const auto decision = DecideSave(g_cosave.current, false, *target);
                if (decision.disposition == Core::SaveDisposition::CleanupFromLegacy) {
                    auto reason = g_migration.Prepare(g_cosave.current, *target,
                        [intfc](auto raw, auto& resolved) { return intfc->ResolveFormID(raw, resolved); }, g_loadWorld);
                    if (reason.empty()) {
                        g_pendingLoadGeneration = *target;
                        RecordLoadDiagnostics(decision);
                        g_notifyOnLoad = false;
                        ModState::Set(ModMode::CleanupPending, "PoE1 세이브 정리 준비");
                    } else {
                        SetSaveDecision({ Core::SaveDisposition::Preserve, std::move(reason) });
                    }
                    return;
                }
                const bool playable = IsPlayable(decision.disposition);
                if (playable) {
                    RestoreLoadedRecords(intfc);
                    g_pendingLoadGeneration = *target;
                }
                SetSaveDecision(decision);
                if (playable) {
                    g_cosave.current = {};
                }
            } catch (...) {
                g_migration.Reset();
                g_pendingLoadGeneration.reset();
                SKSE::log::error("SaveLoad: failed to read or restore the entire co-save.");
                SetSaveDecision({ Core::SaveDisposition::Preserve, "코세이브를 끝까지 읽거나 복원하지 못했습니다." });
            }
        }
    }

    bool SaveLoad::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        const Core::DataPack* dataPack,
        const Core::PassiveTreeData* treeData,
        const Core::Config* config,
        const Core::ItemData* itemData,
        Core::CharmSystem* charmSystem)
    {
        if (g_installed) {
            return true;
        }
        g_queue = queue;
        g_statEngine = statEngine;
        g_dataPack = dataPack;
        g_treeData = treeData;
        g_config = config;
        g_itemData = itemData;
        g_charmSystem = charmSystem;

        const auto* serialization = SKSE::GetSerializationInterface();
        if (!serialization) {
            SKSE::log::error("SaveLoad: SKSE SerializationInterface unavailable.");
            return false;
        }
        serialization->SetUniqueID(kCosaveId);
        serialization->SetRevertCallback(OnRevert);
        serialization->SetSaveCallback(OnSave);
        serialization->SetLoadCallback(OnLoad);

        g_installed = true;
        SKSE::log::info("SaveLoad: SKSE co-save callbacks registered (ID='SPOE').");
        return true;
    }

    void SaveLoad::SetContentCheck(std::optional<Core::SaveGeneration> generation, std::string reason)
    {
        std::scoped_lock lock(g_cosaveMutex);
        if (!g_worldGeneration) {
            g_worldGeneration = generation;
        }
        g_currentGeneration = std::move(generation);
        g_notifyOnLoad = !g_currentGeneration;
        ModState::Set(g_currentGeneration ? ModMode::Normal : ModMode::Preserve, std::move(reason));
    }

    void SaveLoad::SetCleanupTestTarget(bool enabled)
    {
        std::scoped_lock lock(g_cosaveMutex);
        if (enabled) {
            g_testGeneration = Core::SaveGeneration{ "poe2", 1, "autotest", "autotest-cleanup",
                SKSE::GetPluginVersion().string(".") };
        } else if (g_testGeneration) {
            g_cleanupLoadRequest.Clear();
            g_testGeneration.reset();
            if (g_thisLoadUsesTest && ModState::Get()->mode == ModMode::CleanupPending) {
                g_migration.Reset();
                g_pendingLoadGeneration.reset();
                ModState::Set(ModMode::Preserve, "AutoTest가 정리를 마치기 전에 종료되었습니다.");
            }
            g_thisLoadUsesTest = false;
        }
    }

    std::optional<Core::SaveGeneration> SaveLoad::GetCleanupTestTarget()
    {
        std::scoped_lock lock(g_cosaveMutex);
        return g_testGeneration;
    }

    void SaveLoad::ExpectCleanupTestLoad(std::string_view stem)
    {
        std::scoped_lock lock(g_cosaveMutex);
        if (g_testGeneration) {
            g_cleanupLoadRequest.Expect(stem);
        }
    }

    bool SaveLoad::MatchCleanupTestLoad(std::string_view name) noexcept
    {
        std::scoped_lock lock(g_cosaveMutex);
        g_thisLoadUsesTest = g_testGeneration && g_cosave.IsLoading() && g_cleanupLoadRequest.Match(name);
        return g_thisLoadUsesTest;
    }

    std::optional<MigrationCandidateSnapshot> SaveLoad::GetMigrationCandidate(RE::PlayerCharacter* player)
    {
        std::scoped_lock lock(g_cosaveMutex);
        if (ModState::Get()->mode != ModMode::CleanupPending || g_migration.GetPhase() == MigrationRuntime::Phase::None) {
            return std::nullopt;
        }
        return g_migration.Snapshot(player);
    }

    std::optional<MigrationDiagnostics> SaveLoad::GetLastMigrationDiagnostics()
    {
        std::scoped_lock lock(g_cosaveMutex);
        return g_lastMigration ? std::optional{ *g_lastMigration } : std::nullopt;
    }

    void SaveLoad::ReleaseForTest()
    {
        std::scoped_lock lock(g_cosaveMutex);
        if (g_thisLoadUsesTest && g_migration.IsForWorld(g_loadWorld)) {
            g_migration.ReleaseForTest();
        }
    }

    void SaveLoad::TickMigration()
    {
        using enum MigrationRuntime::Phase;
        std::string_view reason;
        std::shared_ptr<const MigrationDiagnostics> completed;
        std::optional<std::uint64_t> initGeneration;
        {
            std::scoped_lock cosaveLock(g_cosaveMutex);
            if (g_cosave.IsLoading() || ModState::Get()->mode != ModMode::CleanupPending || !g_migration.IsForWorld(g_loadWorld) ||
                (g_migration.GetPhase() != Escaping && g_migration.GetPhase() != Ready)) {
                return;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            const bool eligible = MigrationRuntime::CanTick(player);
            reason = g_migration.TickEscape(player, eligible);
            if (reason.empty()) {
                if (!eligible || g_migration.GetPhase() != Ready || (g_thisLoadUsesTest && !g_migration.IsReleasedForTest())) {
                    return;
                }
                auto* values = player->AsActorValueOwner();
                reason = g_migration.CheckCommitGate(player);
                if (reason.empty()) {
                    SKSE::log::info("SaveLoad: migration tick thread={}, world={}.", REX::W32::GetCurrentThreadId(), g_loadWorld);
                    std::scoped_lock txLock(g_txMutex);
                    std::optional<Core::SaveRecordBatch> original;
                    try {
                        auto result = std::make_shared<MigrationDiagnostics>();
                        *result = g_migration.Execute(player, *values, RevertLegacySpeed, [&] {
                            ResetMemoryState(true);
                            original = std::exchange(g_cosave.current, {});
                        });
                        ModState::Set(ModMode::Normal, "PoE1 세이브 정리를 마쳤습니다.");
                        g_worldGeneration = std::move(g_pendingLoadGeneration);
                        completed = g_lastMigration = std::move(result);
                        initGeneration = g_queue ? g_queue->CurrentGeneration() : 0;
                    } catch (...) {
                        if (original) {
                            g_cosave.current = std::move(*original);
                        }
                        g_lastMigration.reset();
                        reason = "정리 중 오류가 발생했습니다. 상태 일부가 바뀌었을 수 있으니 저장하지 말고 다시 불러오세요.";
                    }
                }
            }
            g_migration.Reset();
            g_pendingLoadGeneration.reset();
            g_thisLoadUsesTest = false;
            if (!initGeneration) {
                ModState::Set(ModMode::Preserve, std::string(reason));
            }
        }
        if (initGeneration) {
            ScheduleEngineInit(false, true, *initGeneration);
        }
        if (!completed) {
            try {
                RE::SendHUDMessage::ShowHUDMessage(reason.data());
                SKSE::log::warn("SaveLoad: legacy cleanup refused. {}", reason);
            } catch (...) {
                SKSE::log::error("SaveLoad: could not show the legacy cleanup refusal.");
            }
            return;
        }
        const auto& result = *completed;
        try {
            SKSE::log::info("SaveLoad: migration completed. {}", result.Describe());
        } catch (...) {
            SKSE::log::error("SaveLoad: could not report the legacy cleanup result.");
        }
        try {
            const auto lostItems = std::accumulate(result.lostReturns.begin(), result.lostReturns.end(), std::size_t{});
            const auto lostSpeed = result.speedResults[static_cast<std::size_t>(MigrationSpeedResult::Lost)] +
                result.speedModsUnresolved;
            const auto lostDrops = result.dropResults[static_cast<std::size_t>(WorldDropTracker::LegacyDropReturn::Lost)];
            const auto refund = result.mapClose.refund;
            using Refund = LegacyRunCloseResult::Refund;
            std::string notice = "PoE1 세이브를 PoE2 판에 맞게 정리했습니다. "
                "레벨·트리·스킬은 초기화했습니다. 되돌리려면 저장하지 말고 다시 불러오세요.";
            notice += std::format(" 돌려주지 못한 물건 {}개. 지도 {}.", lostItems, result.MapResultName());
            if (result.map == MigrationDiagnostics::MapResult::Partial) {
                notice += std::format(" 스폰 정리 실패 {}개.", result.mapClose.spawnsFailed);
                if (refund == Refund::Lost) {
                    notice += " 입장 비용을 돌려주지 못했습니다.";
                }
            }
            notice += std::format(" 원복하지 못한 NPC 속도 {}건·월드 드랍 {}개. 손상 레코드 {}개.",
                lostSpeed, lostDrops, result.damagedRecords);
            if (result.engineRevertMismatch) {
                notice += " 원복 불일치.";
            }
            RE::SendHUDMessage::ShowHUDMessage(notice.c_str());
        } catch (...) {
            SKSE::log::error("SaveLoad: could not show the legacy cleanup notice.");
        }
    }

    void SaveLoad::OnGameLoadStarted()
    {
        std::scoped_lock lock(g_cosaveMutex);
        g_thisLoadUsesTest = false;
        g_pendingLoadGeneration.reset();
        if (g_cosave.IsLoading()) {
            ++g_loadWorld;
            g_migration.Reset();
            g_lastMigration.reset();
            g_lastLoad = {};
            g_cosave.Revert();
            ModState::Set(ModMode::Preserve, "불러오는 중");
            return;
        }
        g_lastLoad = {};
        const auto previousMode = ModState::Get()->mode;
        const bool saveNormalState = previousMode == ModMode::Normal && g_worldGeneration;
        if (g_currentGeneration) {
            g_notifyOnLoad = false;
            ModState::Set(ModMode::Preserve, "불러오는 중");
        }
        Core::SaveRecordBatch snapshot;
        try {
            if (saveNormalState) {
                snapshot.complete = WriteSnapshot(CaptureSaveSnapshot(), *g_worldGeneration,
                    [&](Core::RawSaveRecord&& record) {
                        snapshot.records.push_back(std::move(record));
                        return true;
                    }, ReportWriteWarning);
            } else {
                snapshot = g_cosave.current;
            }
        } catch (...) {
            snapshot.complete = false;
        }
        if (!snapshot.complete) {
            SKSE::log::warn("SaveLoad: pre-load snapshot incomplete, retaining {} complete record(s).",
                snapshot.records.size());
        }
        g_cosave.BeginLoad(std::move(snapshot));
        ++g_loadWorld;
        g_migration.Reset();
        g_lastMigration.reset();
    }

    void SaveLoad::OnGameLoadFinished(bool succeeded)
    {
        bool incomplete = false;
        std::uint64_t world = 0;
        {
            std::scoped_lock lock(g_cosaveMutex);
            world = g_loadWorld;
            g_cosave.FinishLoad(succeeded);
            if (succeeded) {
                if (ModState::AllowsGameplay() && g_pendingLoadGeneration) {
                    g_worldGeneration = std::exchange(g_pendingLoadGeneration, std::nullopt);
                }
            } else {
                g_migration.Reset();
                g_lastMigration.reset();
                g_pendingLoadGeneration.reset();
                g_thisLoadUsesTest = false;
                g_cleanupLoadRequest.Clear();
                incomplete = !g_cosave.current.complete;
                ModState::Set(ModMode::Preserve, "불러오기 실패");
            }
        }
        if (succeeded) {
            ScheduleLoadInit(false, world);
        } else {
            SKSE::log::warn("SaveLoad: game load failed, restoring the pre-load co-save snapshot.");
            RE::SendHUDMessage::ShowHUDMessage(
                "불러오기에 실패했습니다. 모드는 멈췄고 저장하면 이전 기록을 그대로 씁니다. 다시 불러오세요.");
            if (incomplete) {
                RE::SendHUDMessage::ShowHUDMessage(kIncompleteCosaveNotice);
            }
        }
    }

    void SaveLoad::SchedulePostLoadOrNewGameInit(bool isNewGame)
    {
        ScheduleLoadInit(isNewGame, std::nullopt);
    }

    void SaveLoad::ScheduleLoadInit(bool isNewGame, std::optional<std::uint64_t> expectedWorld)
    {
        MapDeviceRuntime::DisarmUninstallCleanup();
        std::string notice;
        bool playable = false;
        bool incomplete = false;
        std::uint64_t queueGeneration = 0;
        {
            std::scoped_lock lock(g_cosaveMutex);
            if (expectedWorld && *expectedWorld != g_loadWorld) {
                return;
            }
            if (isNewGame) {
                g_cosave.FinishLoad(true);
                ++g_loadWorld;
                g_migration.Reset();
                g_lastMigration.reset();
                g_pendingLoadGeneration.reset();
                g_thisLoadUsesTest = false;
                g_cleanupLoadRequest.Clear();
            }
            if (isNewGame || !g_cosave.receivedLoad) {
                // A new game can arrive without OnLoad. SKSE also skips OnLoad if the .skse file cannot be opened.
                g_cosave.Revert();
                if (const auto* target = Core::SelectSaveTarget(g_currentGeneration, g_testGeneration, g_thisLoadUsesTest)) {
                    if (isNewGame) {
                        ResetMemoryState();
                    }
                    const auto decision = DecideSave(g_cosave.current, isNewGame, *target);
                    if (IsPlayable(decision.disposition)) {
                        g_pendingLoadGeneration = *target;
                    }
                    SetSaveDecision(decision);
                }
            }
            const auto status = ModState::Get();
            playable = status->mode == ModMode::Normal;
            queueGeneration = g_queue ? g_queue->CurrentGeneration() : 0;
            if (status->mode == ModMode::CleanupPending) {
                g_migration.Confirm(g_loadWorld, RE::PlayerCharacter::GetSingleton());
            } else {
                if (playable && g_pendingLoadGeneration) {
                    g_worldGeneration = std::move(g_pendingLoadGeneration);
                }
                g_pendingLoadGeneration.reset();
                g_thisLoadUsesTest = false;
            }
            incomplete = !g_cosave.current.complete;
            if (std::exchange(g_notifyOnLoad, false)) {
                notice = status->reason;
            }
        }
        if (!notice.empty()) {
            if (!playable) {
                notice = "SkyrimPoE 보존 모드. " + notice + " 호환되는 모드 판으로 돌아가거나 이 저장 전의 세이브를 쓰세요.";
            }
            RE::SendHUDMessage::ShowHUDMessage(notice.c_str());
        }
        if (incomplete) {
            RE::SendHUDMessage::ShowHUDMessage(kIncompleteCosaveNotice);
        }
        if (playable) {
            ScheduleEngineInit(isNewGame, false, queueGeneration);
        }
    }

    void SaveLoad::ScheduleEngineInit(bool isNewGame, bool afterCleanup, std::uint64_t queueGeneration)
    {
        if (!g_queue) {
            return;
        }

        // 결정 119, 150 ④: 세이브 로드·새 게임 직후 엔진 반영은 반드시 주 갱신 스레드에서만 실행한다.
        g_queue->EnqueueForGeneration(queueGeneration, [isNewGame, afterCleanup]() {
            if (!ModState::AllowsGameplay()) {
                return;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                SKSE::log::warn("SaveLoad::PostLoadInit: PlayerCharacter unavailable.");
                return;
            }

            // 결정 150 ④: 로드 직후 OAR 게이트(Variable09)를 0으로 강제 초기화해 직전 세이브의 채널링 잔상을 제거한다.
            if (auto* avOwner = player->AsActorValueOwner()) {
                avOwner->SetActorValue(RE::ActorValue::kVariable09, 0.0f);
            }

            bool needStarterGems = false;
            bool needStage5Gems = false;
            bool needStarterCurrency = false;
            {
                std::scoped_lock lock(g_stateMutex);
                if (g_config && g_config->giveStarterGems) {
                    if (!g_state.starterGemsGiven) {
                        needStarterGems = true;
                    } else if (!g_state.starterStage5GemsGiven) {
                        needStage5Gems = true;
                    }
                }
                if (g_config && g_config->giveStarterCurrency && !g_state.starterCurrencyGiven) {
                    needStarterCurrency = true;
                }
            }

            if (needStarterGems) {
                const auto added = GemInventory::GiveStarterGems(player);
                if (added > 0) {
                    if (added != FormIds::kAllGemForms.size()) {
                        SKSE::log::warn("SaveLoad::PostLoadInit: only {}/{} starter gems found (check SkyrimPoE.esp).",
                            added, FormIds::kAllGemForms.size());
                    }
                    // C-2: 일부 폼 누락으로 불러올 때마다 5단계 19종을 무한 재지급하지 않도록 두 플래그를 함께 확정한다.
                    std::scoped_lock lock(g_stateMutex);
                    g_state.starterGemsGiven = true;
                    g_state.starterStage5GemsGiven = true;
                }
            } else if (needStage5Gems) {
                const auto added = GemInventory::GiveStage5StarterGems(player);
                if (added > 0) {
                    if (added < FormIds::kStage5GemCount) {
                        SKSE::log::warn("SaveLoad::PostLoadInit: only {}/{} stage5 starter gems found (check SkyrimPoE.esp).",
                            added, FormIds::kStage5GemCount);
                    }
                    std::scoped_lock lock(g_stateMutex);
                    g_state.starterStage5GemsGiven = true;
                }
            }

            // 결정 59·123·C-1·C-34: 줍기 예약 실행 전 저장·로드된 세이브의 바닥 스탬프 젬·아이템을 플레이어 번호로 보완 복구
            // 실게임 AE 1.6.1170 확인: .ess 로더가 삭제된 FF 바닥 참조의 ExtraUniqueID::baseID를 소유자(0x14)로
            // 정규화하면서 스탬프 대역 uniqueID는 보존하므로 PostLoadInit에 한해 postLoadOwnerNormalized=true를 허용한다.
            if (!isNewGame) {
                (void)GemInventory::RelinkPickedUpGems(player, true);
                (void)ItemTracker::RelinkPickedUpItems(player, true);

                // Q-1 + M-2: .ess에 남은 NPC kTemporary SpeedMult(냉각/빙결/가속)를 로드 직후 원복한다.
                // 찾지 못한 액터는 버리지 않고 남겨 두며, 재검수 R-9에 따라 주 갱신에서 주기적으로 다시 시도한다.
                const auto [revertedActors, stillPending] = ApplyPendingSpeedRevertsNow();
                if (revertedActors > 0 || stillPending > 0) {
                    SKSE::log::info("SaveLoad::PostLoadInit: reverted persisted kTemporary SpeedMult on {} actor(s) from SPDM (deferred={}).",
                        revertedActors, stillPending);
                }
            }

            if (needStarterCurrency) {
                const auto granted = CraftingRuntime::GiveStarterCurrency(player);
                if (granted > 0) {
                    std::scoped_lock lock(g_stateMutex);
                    g_state.starterCurrencyGiven = true;
                }
            }

            // 5단계(결정 164) + 검수 C4: 트리 정규화로 해제된 소켓 주얼을 RunSerializedTransaction 안에서 원자 반환
            if (g_treeData && g_treeData->IsLoaded()) {
                SaveLoad::RunSerializedTransaction([&]() {
                    PersistentState working = SaveLoad::GetStateCopy();
                    bool changed = false;
                    for (auto it = working.socketedJewels.begin(); it != working.socketedJewels.end(); ) {
                        const auto* node = g_treeData->FindNode(it->first);
                        const bool validSocket = node && node->kind == Core::PassiveNodeKind::Socket &&
                            working.tree.IsAllocated(it->first);
                        if (validSocket) {
                            ++it;
                            continue;
                        }
                        const auto& jRec = it->second;
                        auto* baseObj = RE::TESForm::LookupByID<RE::TESBoundObject>(jRec.baseFormId);
                        if (!baseObj) {
                            baseObj = FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(jRec.jewelColor));
                        }
                        auto* freshExtra = baseObj ? UniqueIdUtil::CreateGameExtraDataList() : nullptr;
                        if (baseObj && freshExtra) {
                            bool untrackedLeft = false;
                            const auto newKey = ItemTracker::AddObjectWithTrackedUniqueId(
                                player, baseObj, freshExtra, nullptr,
                                ItemTracker::UntrackedAddPolicy::RemoveAgain, &untrackedLeft);
                            if (newKey.IsValid()) {
                                ItemTracker::Upsert(newKey, jRec, false);
                                it = working.socketedJewels.erase(it);
                                changed = true;
                                continue;
                            }
                            if (untrackedLeft) {
                                // 재검수 R-15: 실물은 가방에 들어갔다. 남겨 두면 로드할 때마다 1개씩 더 지급되므로 소켓에서 뺀다.
                                SKSE::log::error("SaveLoad::PostLoadInit: orphaned jewel from node {} returned without tracking; affixes lost.",
                                    it->first);
                                RE::SendHUDMessage::ShowHUDMessage("주얼을 가방으로 돌려받았지만 추적에 실패해 어픽스가 사라졌습니다.");
                                it = working.socketedJewels.erase(it);
                                changed = true;
                                continue;
                            }
                        }
                        SKSE::log::warn("SaveLoad::PostLoadInit: failed to return orphaned jewel from node {}; keeping for retry.", it->first);
                        ++it;
                    }
                    if (changed) {
                        SaveLoad::UpdateState([&](PersistentState& st) {
                            st.socketedJewels = working.socketedJewels;
                        });
                    }
                });
            }

            // 검수 M3 & C4 & 6단계 C-3: 로드 직후 착용 장비 스캔, 고아 빛기둥 스윕, 활성 지도/결투장 보스 재바인딩
            EquipmentSync::RefreshFromPlayerNow();
            (void)WorldDropTracker::SweepOrphanBeamsNearPlayer();
            MapDeviceRuntime::OnPostLoadRebind(afterCleanup);

            SaveLoad::SyncStateToStatEngine();
            if (g_statEngine && g_dataPack && g_config && g_dataPack->IsLoaded()) {
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            }
            // 5단계(결정 163): 로드된 스킬 칸에 맞춰 대리 주문·파워 18종 동기화
            ProxySpellRuntime::SyncProxySpellsFromState();

            const auto state = SaveLoad::GetStateCopy();
            const auto snap = g_statEngine ? g_statEngine->GetSnapshot() : nullptr;
            SKSE::log::info(
                "SaveLoad::PostLoadInit({}): exileLevel={}, class={}, trackedGems={}, poeLife={:.1f}, poeMana={:.1f}.",
                isNewGame ? "NewGame" : "PostLoad",
                state.exileLevel,
                state.className,
                GemInventory::CountTracked(),
                snap ? snap->poeMaxLife : 0.0,
                snap ? snap->poeMaxMana : 0.0);
        });
    }

    PersistentState SaveLoad::GetStateCopy()
    {
        std::scoped_lock lock(g_stateMutex);
        return g_state;
    }

    std::optional<Core::SaveGeneration> SaveLoad::GetContentGeneration()
    {
        std::scoped_lock lock(g_cosaveMutex);
        return g_currentGeneration;
    }

    SaveLoad::LoadDiagnostics SaveLoad::GetLastLoadDiagnostics()
    {
        std::scoped_lock lock(g_cosaveMutex);
        return g_lastLoad;
    }

    SaveLoad::SaveDiagnostics SaveLoad::GetLastSaveDiagnostics()
    {
        std::scoped_lock lock(g_cosaveMutex);
        return g_lastSave;
    }

    std::size_t SaveLoad::GetPreservedRecordCount()
    {
        std::scoped_lock lock(g_cosaveMutex);
        return g_cosave.RecordsForSave().records.size();
    }

    SaveLoad::LoadedMapRecordVersions SaveLoad::GetLoadedMapRecordVersions()
    {
        std::scoped_lock lock(g_stateMutex);
        return g_loadedMapRecordVersions;
    }

    void SaveLoad::SetState(const PersistentState& state)
    {
        std::scoped_lock lock(g_stateMutex);
        g_state = state;
    }

    void SaveLoad::UpdateState(const std::function<void(PersistentState&)>& mutator)
    {
        if (!mutator) {
            return;
        }
        std::scoped_lock lock(g_stateMutex);
        mutator(g_state);
    }

    void SaveLoad::RunSerializedTransaction(const std::function<void()>& txFn)
    {
        if (!txFn) {
            return;
        }
        std::scoped_lock txLock(g_txMutex);
        txFn();
    }

    void SaveLoad::AddRevertCallback(std::function<void()> cb)
    {
        if (!cb) {
            return;
        }
        std::scoped_lock lock(g_revertCbMutex);
        g_revertCallbacks.push_back(std::move(cb));
    }

    void SaveLoad::SyncStateToStatEngine()
    {
        if (ModState::AllowsGameplay()) {
            UpdateStatLayerFromState();
        }
    }

    void SaveLoad::DeferSpeedRevert(RE::FormID actorId, float appliedDelta)
    {
        (void)QueueSpeedRevert(actorId, appliedDelta, false);
    }

    void SaveLoad::ForgetSpeedRevert(RE::FormID actorId)
    {
        std::scoped_lock lock(g_speedModMutex);
        std::erase_if(g_pendingSpeedModReverts, [actorId](const auto& e) { return e.first == actorId; });
    }

    void SaveLoad::RetryPendingSpeedReverts(double deltaSec, bool gamePaused)
    {
        if (gamePaused || !std::isfinite(deltaSec) || deltaSec <= 0.0) {
            return;
        }
        {
            // OnRevert와 같은 잠금 아래에서 누적 시간을 읽고 쓴다(Codex 지적 9의 데이터 경합).
            std::scoped_lock lock(g_speedModMutex);
            g_speedRevertRetryAccumSec += deltaSec;
            if (g_speedRevertRetryAccumSec < kSpeedRevertRetryIntervalSec) {
                return;
            }
            g_speedRevertRetryAccumSec = 0.0;
        }
        const auto [reverted, remaining] = ApplyPendingSpeedRevertsNow();
        if (reverted > 0) {
            SKSE::log::info("SaveLoad: reverted deferred kTemporary SpeedMult on {} actor(s) (still pending={}).", reverted, remaining);
        }
    }

    void SaveLoad::PersistCharmSlotsFromSystem()
    {
        if (!g_charmSystem) {
            return;
        }
        const auto slots = g_charmSystem->Slots();
        std::scoped_lock lock(g_stateMutex);
        g_state.charmSlots = slots;
    }

    // 6단계 안전 제거: 맡겨 둔 SpeedMult 원복을 지금 시도한다.
    // 재검수 R-9의 ApplyPendingSpeedRevertsNow와 같은 규칙을 쓴다. 지금 찾지 못한 액터의 항목은 버리지 않고 남겨,
    // 주 갱신의 재시도와 세이브(SPDM)에 그대로 둔다(6단계 재작업 계획 D-10: 실패분은 보존).
    std::size_t SaveLoad::RevertAllPendingSpeedModsNow()
    {
        const auto reverted = ApplyPendingSpeedRevertsNow().first;
        std::size_t remaining = 0;
        {
            std::scoped_lock lock(g_speedModMutex);
            std::unordered_set<RE::FormID> actors;
            for (const auto& entry : g_pendingSpeedModReverts) {
                actors.insert(entry.first);
            }
            remaining = actors.size();
        }
        if (remaining > 0) {
            SKSE::log::warn("SaveLoad: uninstall cleanup reverted {} SpeedMult entr(ies); {} actor(s) not found now and kept for retry.",
                reverted, remaining);
        } else {
            SKSE::log::info("SaveLoad: uninstall cleanup reverted {} SpeedMult entr(ies).", reverted);
        }
        return remaining;
    }
}
