#include "PCH.h"
#include "runtime/SaveLoad.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
#include "runtime/WorldDropTracker.h"

#include "core/PassiveStatParser.h"
#include "core/PassiveTree.h"

namespace SkyrimPoE::Runtime
{
    namespace
    {
        constexpr std::uint32_t kMaxStringLen = 256;
        constexpr std::uint32_t kMaxListLen = 1024;
        constexpr std::uint32_t kMaxTreeNodes = 4096;
        constexpr std::uint32_t kMaxItemRecords = 8192;
        constexpr std::uint32_t kMaxWorldDropRecords = 64;
        constexpr std::uint32_t kMaxAffixesPerItem = 16;
        constexpr std::uint32_t kMaxValuesPerAffix = 16;
        constexpr std::uint32_t kMaxGemDepth = 4;

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::Config* g_config = nullptr;
        bool g_installed = false;

        // 잠금 순서 고정: g_txMutex -> g_stateMutex / GemInventory::g_mutex (결정 137(ㅁ))
        std::mutex g_txMutex;
        std::mutex g_stateMutex;
        PersistentState g_state;
        std::mutex g_revertCbMutex;
        std::vector<std::function<void()>> g_revertCallbacks;

        // SKSE::SerializationInterface::ReadRecordData는 bool이 아니라 실제 읽은 바이트 수(uint32_t)를 반환한다.
        // 절단·손상 레코드에서 부분 읽기가 성공으로 오인되지 않도록 정확 길이를 비교한다.
        bool ReadExact(SKSE::SerializationInterface* intfc, void* dst, std::uint32_t size)
        {
            if (!intfc) {
                return false;
            }
            if (size == 0) {
                return true;
            }
            return intfc->ReadRecordData(dst, size) == size;
        }

        template <class T>
        bool ReadExactPod(SKSE::SerializationInterface* intfc, T& out)
        {
            return ReadExact(intfc, &out, static_cast<std::uint32_t>(sizeof(T)));
        }

        bool WriteString(SKSE::SerializationInterface* intfc, const std::string& str)
        {
            const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(str.size(), kMaxStringLen));
            if (!intfc->WriteRecordData(&len, sizeof(len))) {
                return false;
            }
            if (len > 0 && !intfc->WriteRecordData(str.data(), len)) {
                return false;
            }
            return true;
        }

        bool ReadString(SKSE::SerializationInterface* intfc, std::string& out)
        {
            std::uint32_t len = 0;
            if (!ReadExactPod(intfc, len) || len > kMaxStringLen) {
                return false;
            }
            out.resize(len);
            if (len > 0 && !ReadExact(intfc, out.data(), len)) {
                out.clear();
                return false;
            }
            return true;
        }

        bool WriteGemInstance(SKSE::SerializationInterface* intfc, const Core::GemInstance& gem, std::uint32_t depth = 0)
        {
            if (depth > kMaxGemDepth) {
                return false;
            }
            if (!intfc->WriteRecordData(&gem.instanceId, sizeof(gem.instanceId))) return false;
            if (!WriteString(intfc, gem.gemId)) return false;
            if (!intfc->WriteRecordData(&gem.level, sizeof(gem.level))) return false;
            if (!intfc->WriteRecordData(&gem.quality, sizeof(gem.quality))) return false;
            if (!intfc->WriteRecordData(&gem.xp, sizeof(gem.xp))) return false;
            if (!intfc->WriteRecordData(&gem.socketCount, sizeof(gem.socketCount))) return false;

            const auto childCount = static_cast<std::uint32_t>(std::min<std::size_t>(gem.socketedGems.size(), kMaxListLen));
            if (!intfc->WriteRecordData(&childCount, sizeof(childCount))) return false;
            for (std::uint32_t i = 0; i < childCount; ++i) {
                if (!WriteGemInstance(intfc, gem.socketedGems[i], depth + 1)) {
                    return false;
                }
            }
            return true;
        }

        bool ReadGemInstance(SKSE::SerializationInterface* intfc, Core::GemInstance& out, std::uint32_t depth = 0)
        {
            if (depth > kMaxGemDepth) {
                return false;
            }
            if (!ReadExactPod(intfc, out.instanceId)) return false;
            if (!ReadString(intfc, out.gemId)) return false;
            if (!ReadExactPod(intfc, out.level)) return false;
            if (!ReadExactPod(intfc, out.quality)) return false;
            if (!ReadExactPod(intfc, out.xp)) return false;
            if (!ReadExactPod(intfc, out.socketCount)) return false;

            out.level = static_cast<std::uint8_t>(std::clamp<std::uint32_t>(out.level, 1, Core::kMaxGemLevel));
            out.socketCount = static_cast<std::uint8_t>(std::min<std::uint32_t>(out.socketCount, Core::kMaxGemSockets));

            std::uint32_t childCount = 0;
            if (!ReadExactPod(intfc, childCount) || childCount > kMaxListLen) {
                return false;
            }
            out.socketedGems.clear();
            out.socketedGems.reserve(childCount);
            for (std::uint32_t i = 0; i < childCount; ++i) {
                Core::GemInstance child;
                if (!ReadGemInstance(intfc, child, depth + 1)) {
                    return false;
                }
                out.socketedGems.push_back(std::move(child));
            }
            return true;
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
            return l1;
        }

        void ResetMemoryState()
        {
            if (g_queue) {
                (void)g_queue->BumpGenerationAndClear();
            }
            {
                std::scoped_lock lock(g_stateMutex);
                g_state = PersistentState{};
            }
            GemInventory::Clear();
            ItemTracker::Clear();
            EquipmentSync::Clear();
            WorldDropTracker::Clear();
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
            ResetMemoryState();
            SKSE::log::info("SaveLoad: OnRevert cleared persistent state, gem keys, and command queue.");
        }

        // SKSE 코세이브 콜백: 저장(메모리 스냅샷만 기록, 예약 큐는 저장하지 않음).
        // 결정 137(ㅁ): g_txMutex로 젬 장착/탈착 거래와 직렬화해 일관 스냅샷을 확보한다.
        void OnSave(SKSE::SerializationInterface* intfc)
        {
            if (!intfc) {
                return;
            }

            // 결정 137(ㅁ), 검수 M4: g_txMutex 아래 상태·젬·아이템·바닥드랍 일관 스냅샷을 원자 캡처한다.
            PersistentState state;
            std::vector<TrackedGemEntry> tracked;
            std::vector<TrackedItemEntry> trackedItems;
            std::vector<SavedWorldDropEntry> savedWorldDrops;
            {
                std::scoped_lock txLock(g_txMutex);
                state = SaveLoad::GetStateCopy();
                tracked = GemInventory::TakeSnapshotForSave();
                trackedItems = ItemTracker::TakeSnapshotForSave();
                savedWorldDrops = WorldDropTracker::TakeSnapshotForSave();
            }

            if (!intfc->OpenRecord(SaveLoad::kRecordCore, SaveLoad::kCoreVersion)) {
                SKSE::log::error("SaveLoad: failed to open CORE record for writing.");
                return;
            }

            bool ok = true;
            ok = ok && intfc->WriteRecordData(&state.exileLevel, sizeof(state.exileLevel));
            ok = ok && intfc->WriteRecordData(&state.exileXp, sizeof(state.exileXp));
            ok = ok && intfc->WriteRecordData(&state.unallocatedPassivePoints, sizeof(state.unallocatedPassivePoints));
            ok = ok && WriteString(intfc, state.className);
            const std::uint8_t starterByte = state.starterGemsGiven ? 1 : 0;
            ok = ok && intfc->WriteRecordData(&starterByte, sizeof(starterByte));
            ok = ok && intfc->WriteRecordData(&state.nextGemInstanceId, sizeof(state.nextGemInstanceId));
            ok = ok && intfc->WriteRecordData(&state.appliedHealthDelta, sizeof(state.appliedHealthDelta));
            ok = ok && intfc->WriteRecordData(&state.appliedMagickaDelta, sizeof(state.appliedMagickaDelta));

            const auto slotCount = static_cast<std::uint32_t>(state.slots.size());
            ok = ok && intfc->WriteRecordData(&slotCount, sizeof(slotCount));
            for (const auto& slot : state.slots) {
                const std::uint8_t occupiedByte = slot.occupied ? 1 : 0;
                ok = ok && intfc->WriteRecordData(&occupiedByte, sizeof(occupiedByte));
                ok = ok && intfc->WriteRecordData(&slot.linkedAttackSlot, sizeof(slot.linkedAttackSlot));
                if (slot.occupied) {
                    ok = ok && WriteGemInstance(intfc, slot.gem);
                }
            }
            if (!ok) {
                SKSE::log::error("SaveLoad: error while writing CORE record.");
                return;
            }

            if (!intfc->OpenRecord(SaveLoad::kRecordGemKeys, SaveLoad::kGemKeysVersion)) {
                SKSE::log::error("SaveLoad: failed to open GKEY record for writing.");
                return;
            }

            const auto trackedCount = static_cast<std::uint32_t>(std::min<std::size_t>(tracked.size(), kMaxListLen));
            ok = intfc->WriteRecordData(&trackedCount, sizeof(trackedCount));
            for (std::uint32_t i = 0; i < trackedCount && ok; ++i) {
                const auto& entry = tracked[i];
                const std::uint8_t worldByte = entry.isWorld ? 1 : 0;
                ok = ok && intfc->WriteRecordData(&entry.key.ownerFormId, sizeof(entry.key.ownerFormId));
                ok = ok && intfc->WriteRecordData(&entry.key.uniqueId, sizeof(entry.key.uniqueId));
                ok = ok && intfc->WriteRecordData(&worldByte, sizeof(worldByte));
                ok = ok && WriteGemInstance(intfc, entry.gem);
            }
            if (!ok) {
                SKSE::log::error("SaveLoad: error while writing GKEY record.");
                return;
            }

            // 3단계: 패시브 트리·전직·보스 이정표 영구 상태('TREE' v1)
            if (!intfc->OpenRecord(SaveLoad::kRecordTree, SaveLoad::kTreeVersion)) {
                SKSE::log::error("SaveLoad: failed to open TREE record for writing.");
                return;
            }
            const auto& tr = state.tree;
            const std::uint8_t clsSelByte = tr.classSelected ? 1 : 0;
            ok = intfc->WriteRecordData(&clsSelByte, sizeof(clsSelByte));
            ok = ok && intfc->WriteRecordData(&tr.classId, sizeof(tr.classId));
            ok = ok && WriteString(intfc, tr.ascendancyId);

            const auto nodeCount = static_cast<std::uint32_t>(std::min<std::size_t>(tr.allocatedNodes.size(), kMaxTreeNodes));
            ok = ok && intfc->WriteRecordData(&nodeCount, sizeof(nodeCount));
            for (std::uint32_t i = 0; i < nodeCount && ok; ++i) {
                ok = ok && intfc->WriteRecordData(&tr.allocatedNodes[i], sizeof(std::uint32_t));
            }

            std::vector<std::pair<std::uint32_t, std::uint32_t>> sortedMasteries(
                tr.masterySelections.begin(), tr.masterySelections.end());
            std::sort(sortedMasteries.begin(), sortedMasteries.end());
            const auto masteryCount = static_cast<std::uint32_t>(std::min<std::size_t>(sortedMasteries.size(), kMaxListLen));
            ok = ok && intfc->WriteRecordData(&masteryCount, sizeof(masteryCount));
            for (std::uint32_t i = 0; i < masteryCount && ok; ++i) {
                ok = ok && intfc->WriteRecordData(&sortedMasteries[i].first, sizeof(std::uint32_t));
                ok = ok && intfc->WriteRecordData(&sortedMasteries[i].second, sizeof(std::uint32_t));
            }

            ok = ok && intfc->WriteRecordData(&tr.bossKillCount, sizeof(tr.bossKillCount));
            ok = ok && intfc->WriteRecordData(&tr.dragonKillCount, sizeof(tr.dragonKillCount));
            ok = ok && intfc->WriteRecordData(&tr.ascendancyMilestonesBitmask, sizeof(tr.ascendancyMilestonesBitmask));
            if (!ok) {
                SKSE::log::error("SaveLoad: error while writing TREE record.");
                return;
            }

            // 4단계: 제작/감정 PoE 아이템('ITEM' v1) + starterCurrencyGiven 헤더
            if (!intfc->OpenRecord(SaveLoad::kRecordItems, SaveLoad::kItemsVersion)) {
                SKSE::log::error("SaveLoad: failed to open ITEM record for writing.");
                return;
            }
            const std::uint8_t starterCurrByte = state.starterCurrencyGiven ? 1 : 0;
            const auto itemCount = static_cast<std::uint32_t>(std::min<std::size_t>(trackedItems.size(), kMaxItemRecords));
            ok = intfc->WriteRecordData(&starterCurrByte, sizeof(starterCurrByte));
            ok = ok && intfc->WriteRecordData(&itemCount, sizeof(itemCount));
            for (std::uint32_t i = 0; i < itemCount && ok; ++i) {
                const auto& entry = trackedItems[i];
                const auto& rec = entry.record;
                const std::uint8_t worldByte = entry.isWorld ? 1 : 0;
                const auto rarityByte = static_cast<std::uint8_t>(rec.rarity);
                const auto slotGroupByte = static_cast<std::uint8_t>(rec.slotGroup);
                const auto defSubByte = static_cast<std::uint8_t>(rec.defenseSubtype);
                const std::uint8_t amuletByte = rec.isAmulet ? 1 : 0;
                ok = ok && intfc->WriteRecordData(&entry.key.ownerFormId, sizeof(entry.key.ownerFormId));
                ok = ok && intfc->WriteRecordData(&entry.key.uniqueId, sizeof(entry.key.uniqueId));
                ok = ok && intfc->WriteRecordData(&worldByte, sizeof(worldByte));
                ok = ok && intfc->WriteRecordData(&rec.baseFormId, sizeof(rec.baseFormId));
                ok = ok && intfc->WriteRecordData(&rarityByte, sizeof(rarityByte));
                ok = ok && intfc->WriteRecordData(&rec.itemLevel, sizeof(rec.itemLevel));
                ok = ok && intfc->WriteRecordData(&slotGroupByte, sizeof(slotGroupByte));
                ok = ok && intfc->WriteRecordData(&defSubByte, sizeof(defSubByte));
                ok = ok && intfc->WriteRecordData(&amuletByte, sizeof(amuletByte));
                ok = ok && WriteString(intfc, rec.weaponTag);
                const auto affixCount = static_cast<std::uint32_t>(std::min<std::size_t>(rec.affixes.size(), kMaxAffixesPerItem));
                ok = ok && intfc->WriteRecordData(&affixCount, sizeof(affixCount));
                for (std::uint32_t a = 0; a < affixCount && ok; ++a) {
                    ok = ok && WriteString(intfc, rec.affixes[a].affixId);
                    const auto valCount = static_cast<std::uint32_t>(
                        std::min<std::size_t>(rec.affixes[a].values.size(), kMaxValuesPerAffix));
                    ok = ok && intfc->WriteRecordData(&valCount, sizeof(valCount));
                    for (std::uint32_t v = 0; v < valCount && ok; ++v) {
                        ok = ok && intfc->WriteRecordData(&rec.affixes[a].values[v], sizeof(double));
                    }
                }
            }
            if (!ok) {
                SKSE::log::error("SaveLoad: error while writing ITEM record.");
                return;
            }

            // 4단계(검수 C4): 활성 바닥 드랍·빛기둥('WDRP' v1)
            if (!intfc->OpenRecord(SaveLoad::kRecordWorldDrops, SaveLoad::kWorldDropsVersion)) {
                SKSE::log::error("SaveLoad: failed to open WDRP record for writing.");
                return;
            }
            const auto wdrpCount = static_cast<std::uint32_t>(
                std::min<std::size_t>(savedWorldDrops.size(), kMaxWorldDropRecords));
            ok = intfc->WriteRecordData(&wdrpCount, sizeof(wdrpCount));
            for (std::uint32_t i = 0; i < wdrpCount && ok; ++i) {
                const auto& w = savedWorldDrops[i];
                ok = ok && intfc->WriteRecordData(&w.dropRefFormId, sizeof(w.dropRefFormId));
                ok = ok && intfc->WriteRecordData(&w.corpseRefFormId, sizeof(w.corpseRefFormId));
                ok = ok && intfc->WriteRecordData(&w.beamRefFormId, sizeof(w.beamRefFormId));
                ok = ok && intfc->WriteRecordData(&w.worldDropTier, sizeof(w.worldDropTier));
            }
            if (!ok) {
                SKSE::log::error("SaveLoad: error while writing WDRP record.");
                return;
            }

            SKSE::log::info("SaveLoad: saved CORE(Lv {}), GKEY({}), TREE(nodes={}), ITEM({}), WDRP({}).",
                state.exileLevel, trackedCount, nodeCount, itemCount, wdrpCount);
        }

        // SKSE 코세이브 콜백: 불러오기(순수 메모리 복원만 수행, 엔진 조작은 주 갱신으로 이연)
        void OnLoad(SKSE::SerializationInterface* intfc)
        {
            if (!intfc) {
                return;
            }

            PersistentState loadedState;
            bool hasCore = false;
            Core::PassiveTreeState loadedTree;
            bool hasTree = false;
            bool hasItems = false;
            bool loadedStarterCurrencyGiven = false;
            std::vector<TrackedGemEntry> loadedGems;
            std::vector<TrackedItemEntry> loadedItems;
            std::vector<SavedWorldDropEntry> loadedWorldDrops;

            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t length = 0;

            while (intfc->GetNextRecordInfo(type, version, length)) {
                if (type == SaveLoad::kRecordCore) {
                    if (version != SaveLoad::kCoreVersion) {
                        SKSE::log::warn("SaveLoad: unsupported CORE record version {} (expected {}).", version, SaveLoad::kCoreVersion);
                        continue;
                    }
                    PersistentState tmp;
                    std::uint8_t starterByte = 0;
                    std::uint32_t slotCount = 0;
                    bool ok = true;
                    ok = ok && ReadExactPod(intfc, tmp.exileLevel);
                    ok = ok && ReadExactPod(intfc, tmp.exileXp);
                    ok = ok && ReadExactPod(intfc, tmp.unallocatedPassivePoints);
                    ok = ok && ReadString(intfc, tmp.className);
                    ok = ok && ReadExactPod(intfc, starterByte);
                    ok = ok && ReadExactPod(intfc, tmp.nextGemInstanceId);
                    ok = ok && ReadExactPod(intfc, tmp.appliedHealthDelta);
                    ok = ok && ReadExactPod(intfc, tmp.appliedMagickaDelta);
                    ok = ok && ReadExactPod(intfc, slotCount);

                    if (ok && slotCount <= Core::kSkillSlotCount) {
                        tmp.exileLevel = std::clamp<std::uint32_t>(tmp.exileLevel, 1, Core::kMaxExileLevel);
                        tmp.starterGemsGiven = (starterByte != 0);
                        if (!std::isfinite(tmp.appliedHealthDelta)) tmp.appliedHealthDelta = 0.0;
                        if (!std::isfinite(tmp.appliedMagickaDelta)) tmp.appliedMagickaDelta = 0.0;
                        if (tmp.nextGemInstanceId == 0) {
                            tmp.nextGemInstanceId = 1;
                        }
                        for (std::uint32_t i = 0; i < slotCount && ok; ++i) {
                            std::uint8_t occupiedByte = 0;
                            std::int8_t linked = -1;
                            ok = ok && ReadExactPod(intfc, occupiedByte);
                            ok = ok && ReadExactPod(intfc, linked);
                            tmp.slots[i].slotIndex = static_cast<std::uint8_t>(i);
                            tmp.slots[i].occupied = (occupiedByte != 0);
                            tmp.slots[i].linkedAttackSlot =
                                (linked >= 0 && linked < static_cast<std::int8_t>(Core::kSkillSlotCount)) ? linked : -1;
                            if (tmp.slots[i].occupied) {
                                ok = ok && ReadGemInstance(intfc, tmp.slots[i].gem);
                            }
                        }
                    } else {
                        ok = false;
                    }

                    if (ok) {
                        loadedState = std::move(tmp);
                        hasCore = true;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse CORE record (truncated or corrupt); falling back to defaults.");
                    }
                } else if (type == SaveLoad::kRecordGemKeys) {
                    if (version != SaveLoad::kGemKeysVersion) {
                        SKSE::log::warn("SaveLoad: unsupported GKEY record version {} (expected {}).", version, SaveLoad::kGemKeysVersion);
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
                        ok = ok && ReadGemInstance(intfc, gem);
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
                        SKSE::log::error("SaveLoad: failed to parse GKEY record (truncated or corrupt).");
                    }
                } else if (type == SaveLoad::kRecordTree) {
                    if (version != SaveLoad::kTreeVersion) {
                        SKSE::log::warn("SaveLoad: unsupported TREE record version {} (expected {}).", version, SaveLoad::kTreeVersion);
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
                        tmpTree.ascendancyMilestonesBitmask &= 0x7u;
                        loadedTree = std::move(tmpTree);
                        hasTree = true;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse TREE record (truncated or corrupt); falling back to empty tree.");
                    }
                } else if (type == SaveLoad::kRecordItems) {
                    if (version != SaveLoad::kItemsVersion) {
                        SKSE::log::warn("SaveLoad: unsupported ITEM record version {} (expected {}).", version, SaveLoad::kItemsVersion);
                        continue;
                    }
                    std::uint8_t starterCurrByte = 0;
                    std::uint32_t itemCount = 0;
                    if (!ReadExactPod(intfc, starterCurrByte) || !ReadExactPod(intfc, itemCount) || itemCount > kMaxItemRecords) {
                        SKSE::log::error("SaveLoad: invalid ITEM header.");
                        continue;
                    }
                    std::vector<TrackedItemEntry> tmpItems;
                    tmpItems.reserve(itemCount);
                    bool ok = true;
                    for (std::uint32_t i = 0; i < itemCount && ok; ++i) {
                        RE::FormID rawOwner = 0;
                        std::uint16_t uid = 0;
                        std::uint8_t worldByte = 0;
                        RE::FormID rawBase = 0;
                        std::uint8_t rarityByte = 0;
                        std::uint32_t iLvl = 1;
                        std::uint8_t slotGroupByte = 0;
                        std::uint8_t defSubByte = 0;
                        std::uint8_t amuletByte = 0;
                        std::string weaponTag;
                        std::uint32_t affixCount = 0;

                        ok = ok && ReadExactPod(intfc, rawOwner);
                        ok = ok && ReadExactPod(intfc, uid);
                        ok = ok && ReadExactPod(intfc, worldByte);
                        ok = ok && ReadExactPod(intfc, rawBase);
                        ok = ok && ReadExactPod(intfc, rarityByte);
                        ok = ok && ReadExactPod(intfc, iLvl);
                        ok = ok && ReadExactPod(intfc, slotGroupByte);
                        ok = ok && ReadExactPod(intfc, defSubByte);
                        ok = ok && ReadExactPod(intfc, amuletByte);
                        ok = ok && ReadString(intfc, weaponTag);
                        ok = ok && ReadExactPod(intfc, affixCount);
                        if (!ok || affixCount > kMaxAffixesPerItem) {
                            ok = false;
                            break;
                        }

                        Core::PoEItemRecord rec;
                        rec.rarity = rarityByte <= static_cast<std::uint8_t>(Core::ItemRarity::Rare)
                            ? static_cast<Core::ItemRarity>(rarityByte)
                            : Core::ItemRarity::Normal;
                        rec.itemLevel = std::clamp<std::uint32_t>(iLvl, 1, Core::kMaxExileLevel);
                        rec.slotGroup = slotGroupByte < Core::kTokenSlotGroupCount
                            ? static_cast<Core::TokenSlotGroup>(slotGroupByte)
                            : Core::TokenSlotGroup::OneHandWeapon;
                        rec.defenseSubtype = defSubByte <= static_cast<std::uint8_t>(Core::DefenseSubtype::EnergyShield)
                            ? static_cast<Core::DefenseSubtype>(defSubByte)
                            : Core::DefenseSubtype::None;
                        rec.isAmulet = (amuletByte != 0);
                        rec.weaponTag = std::move(weaponTag);
                        rec.affixes.reserve(affixCount);

                        for (std::uint32_t a = 0; a < affixCount && ok; ++a) {
                            Core::RolledAffix af;
                            std::uint32_t valCount = 0;
                            ok = ok && ReadString(intfc, af.affixId);
                            ok = ok && ReadExactPod(intfc, valCount);
                            if (!ok || valCount > kMaxValuesPerAffix) {
                                ok = false;
                                break;
                            }
                            af.values.resize(valCount);
                            for (std::uint32_t v = 0; v < valCount && ok; ++v) {
                                ok = ok && ReadExactPod(intfc, af.values[v]);
                                if (ok && !std::isfinite(af.values[v])) {
                                    af.values[v] = 0.0;
                                }
                            }
                            if (ok) {
                                rec.affixes.push_back(std::move(af));
                            }
                        }
                        if (!ok) {
                            break;
                        }

                        RE::FormID resolvedOwner = 0;
                        RE::FormID resolvedBase = 0;
                        if (intfc->ResolveFormID(rawOwner, resolvedOwner) && resolvedOwner != 0 &&
                            intfc->ResolveFormID(rawBase, resolvedBase) && resolvedBase != 0) {
                            rec.baseFormId = resolvedBase;
                            tmpItems.push_back(TrackedItemEntry{
                                Core::InventoryItemKey{ resolvedOwner, uid },
                                worldByte != 0,
                                std::move(rec)
                            });
                        } else {
                            SKSE::log::warn("SaveLoad: pruning stale item key owner=0x{:08X} base=0x{:08X}.", rawOwner, rawBase);
                        }
                    }
                    if (ok) {
                        loadedStarterCurrencyGiven = (starterCurrByte != 0);
                        loadedItems = std::move(tmpItems);
                        hasItems = true;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse ITEM record.");
                    }
                } else if (type == SaveLoad::kRecordWorldDrops) {
                    if (version != SaveLoad::kWorldDropsVersion) {
                        SKSE::log::warn("SaveLoad: unsupported WDRP record version {} (expected {}).", version, SaveLoad::kWorldDropsVersion);
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
                } else {
                    SKSE::log::warn("SaveLoad: ignoring unknown record type 0x{:08X}.", type);
                }
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
                        *g_treeData, loadedState.tree, loadedState.exileLevel, *g_config);
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
            if (hasCore || hasTree || hasItems) {
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

            GemInventory::RestoreFromSave(std::move(loadedGems));
            ItemTracker::RestoreFromSave(std::move(loadedItems));
            WorldDropTracker::RestoreFromSave(std::move(loadedWorldDrops));
            SaveLoad::SyncStateToStatEngine();

            SKSE::log::info("SaveLoad: OnLoad restored CORE({}), TREE(nodes={}), GKEY({}), ITEM({}), WDRP({}).",
                hasCore, loadedState.tree.allocatedNodes.size(),
                GemInventory::CountTracked(), ItemTracker::CountTracked(), WorldDropTracker::ActiveCount());
        }
    }

    bool SaveLoad::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        const Core::DataPack* dataPack,
        const Core::PassiveTreeData* treeData,
        const Core::Config* config)
    {
        if (g_installed) {
            return true;
        }
        g_queue = queue;
        g_statEngine = statEngine;
        g_dataPack = dataPack;
        g_treeData = treeData;
        g_config = config;

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

    void SaveLoad::SchedulePostLoadOrNewGameInit(bool isNewGame)
    {
        if (isNewGame) {
            ResetMemoryState();
        }
        if (!g_queue) {
            return;
        }

        // 결정 119, 150 ④: 세이브 로드·새 게임 직후 엔진 반영은 반드시 주 갱신 스레드에서만 실행한다.
        g_queue->Enqueue([isNewGame]() {
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
            bool needStarterCurrency = false;
            {
                std::scoped_lock lock(g_stateMutex);
                if (g_config && g_config->giveStarterGems && !g_state.starterGemsGiven) {
                    needStarterGems = true;
                }
                if (g_config && g_config->giveStarterCurrency && !g_state.starterCurrencyGiven) {
                    needStarterCurrency = true;
                }
            }

            if (needStarterGems) {
                const auto added = GemInventory::GiveStarterGems(player);
                if (added == FormIds::kStage2GemForms.size()) {
                    std::scoped_lock lock(g_stateMutex);
                    g_state.starterGemsGiven = true;
                } else if (added > 0) {
                    SKSE::log::warn("SaveLoad::PostLoadInit: only {}/{} starter gems found (check SkyrimPoE.esp).",
                        added, FormIds::kStage2GemForms.size());
                    std::scoped_lock lock(g_stateMutex);
                    g_state.starterGemsGiven = true;
                }
            }

            if (needStarterCurrency) {
                const auto granted = CraftingRuntime::GiveStarterCurrency(player);
                if (granted > 0) {
                    std::scoped_lock lock(g_stateMutex);
                    g_state.starterCurrencyGiven = true;
                }
            }

            // 검수 M3 & C4: 로드 직후 착용 장비 즉시 스캔 및 미추적 고아 빛기둥 스윕
            EquipmentSync::RefreshFromPlayerNow();
            (void)WorldDropTracker::SweepOrphanBeamsNearPlayer();

            SaveLoad::SyncStateToStatEngine();
            if (g_statEngine && g_dataPack && g_config && g_dataPack->IsLoaded()) {
                (void)g_statEngine->RebuildIfDirty(*g_dataPack, *g_config);
            }

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
        if (!g_statEngine) {
            return;
        }
        const auto state = GetStateCopy();
        g_statEngine->SetLayer1(BuildLayer1FromState(state));
    }
}
