#include "PCH.h"
#include "runtime/SaveLoad.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/CraftingRuntime.h"
#include "runtime/EquipmentSync.h"
#include "runtime/FormIds.h"
#include "runtime/GemInventory.h"
#include "runtime/ItemTracker.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/ProxySpellRuntime.h"
#include "runtime/UniqueIdUtil.h"
#include "runtime/WorldDropTracker.h"

#include "core/ItemAffixParser.h"
#include "core/MapSaveIdentity.h"
#include "core/MapSystem.h"
#include "core/PassiveStatParser.h"
#include "core/PassiveTree.h"
#include "core/SaveCapPolicy.h"

#include <bit>

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
        constexpr std::uint32_t kMaxSocketedJewels = 64;
        constexpr std::uint32_t kPlayerRefFormId = 0x00000014;
        // 결정 166의 MAPS v1을 불러올 때 귀환 여부를 판정하는 바닐라 셀 전용 표다.
        constexpr std::array<RE::FormID, 5> kLegacyMapsV1Cells{
            0x000B6BE6, 0x00043FAB, 0x00015224, 0x000152B1, 0x0001521C
        };
        constexpr RE::FormID kLegacyMapsV1ArenaCell = 0x00015211;
        constexpr std::uint32_t kLegacyMapsVersion = 1;

        // 재검수 R-3: 상한을 넘으면 플레이어 소지품 → 바닥 → 그 밖 순으로 남긴다(Core::SelectEntriesWithinSaveCap).
        template <class Entry>
        [[nodiscard]] std::vector<std::size_t> SelectForSave(const std::vector<Entry>& entries, std::size_t cap)
        {
            std::vector<Core::SaveCapCandidate> cands;
            cands.reserve(entries.size());
            for (const auto& e : entries) {
                cands.push_back(Core::SaveCapCandidate{ e.key.ownerFormId, e.isWorld });
            }
            return Core::SelectEntriesWithinSaveCap(cands, cap, kPlayerRefFormId);
        }

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::PassiveTreeData* g_treeData = nullptr;
        const Core::Config* g_config = nullptr;
        const Core::ItemData* g_itemData = nullptr;
        Core::CharmSystem* g_charmSystem = nullptr;
        bool g_installed = false;

        // 잠금 순서 고정: g_txMutex -> g_stateMutex / GemInventory::g_mutex (결정 137(ㅁ))
        std::mutex g_txMutex;
        std::mutex g_stateMutex;
        PersistentState g_state;
        std::mutex g_revertCbMutex;
        std::vector<std::function<void()>> g_revertCallbacks;
        // Q-1: 세이브 시점에 잔존하던 NPC kTemporary SpeedMult 델타(로드 후 주 갱신에서 안전 원복)
        std::mutex g_speedModMutex;
        std::vector<std::pair<RE::FormID, float>> g_pendingSpeedModReverts;
        double g_speedRevertRetryAccumSec = 0.0;
        constexpr double kSpeedRevertRetryIntervalSec = 2.0;
        constexpr std::size_t kMaxPendingSpeedReverts = 1024;

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
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorId);
                if (!actor) {
                    if ((actorId >> 24) == 0xFF) {
                        // 실행 중 만든 FF 액터를 찾지 못하면 이미 지워진 것이고, 번호가 다른 액터에게 다시 쓰일 수 있다.
                        // 세이브(SPDM)에서 읽은 항목도 여기서 버린다(Codex 지적 10).
                        resolved.push_back(entry);
                    } else {
                        ++remaining;
                    }
                    continue;
                }
                if (actor->IsPlayerRef() || actor->IsDeleted()) {
                    resolved.push_back(entry);
                    continue;
                }
                auto* av = actor->AsActorValueOwner();
                if (!av) {
                    ++remaining;
                    continue;
                }
                resolved.push_back(entry);
                // 엔진이 리셋으로 임시 수정치를 이미 0으로 돌렸으면 원복할 것이 없다.
                const float curTemp = actor->GetActorValueModifier(
                    RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult);
                if (std::abs(curTemp) < 0.01f) {
                    continue;
                }
                const float restore = -appliedDelta;
                if (std::abs(restore) >= 0.01f) {
                    av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kSpeedMult, restore);
                    av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, 0.01f);
                    av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kCarryWeight, -0.01f);
                    ++revertedActors;
                }
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

        bool WriteAffixList(SKSE::SerializationInterface* intfc, const std::vector<Core::RolledAffix>& list)
        {
            const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(list.size(), kMaxAffixesPerItem));
            if (!intfc->WriteRecordData(&count, sizeof(count))) return false;
            for (std::uint32_t a = 0; a < count; ++a) {
                if (!WriteString(intfc, list[a].affixId)) return false;
                const auto valCount = static_cast<std::uint32_t>(
                    std::min<std::size_t>(list[a].values.size(), kMaxValuesPerAffix));
                if (!intfc->WriteRecordData(&valCount, sizeof(valCount))) return false;
                for (std::uint32_t v = 0; v < valCount; ++v) {
                    if (!intfc->WriteRecordData(&list[a].values[v], sizeof(double))) return false;
                }
            }
            return true;
        }

        bool ReadAffixList(SKSE::SerializationInterface* intfc, std::vector<Core::RolledAffix>& out)
        {
            std::uint32_t count = 0;
            if (!ReadExactPod(intfc, count) || count > kMaxAffixesPerItem) {
                return false;
            }
            out.clear();
            out.reserve(count);
            for (std::uint32_t a = 0; a < count; ++a) {
                Core::RolledAffix af;
                std::uint32_t valCount = 0;
                if (!ReadString(intfc, af.affixId)) return false;
                if (!ReadExactPod(intfc, valCount) || valCount > kMaxValuesPerAffix) return false;
                af.values.resize(valCount);
                for (std::uint32_t v = 0; v < valCount; ++v) {
                    if (!ReadExactPod(intfc, af.values[v])) return false;
                    if (!std::isfinite(af.values[v])) {
                        af.values[v] = 0.0;
                    }
                }
                out.push_back(std::move(af));
            }
            return true;
        }

        bool WritePoEItemRecordV2(SKSE::SerializationInterface* intfc, const Core::PoEItemRecord& rec)
        {
            const auto rarityByte = static_cast<std::uint8_t>(rec.rarity);
            const auto slotGroupByte = static_cast<std::uint8_t>(rec.slotGroup);
            const auto defSubByte = static_cast<std::uint8_t>(rec.defenseSubtype);
            const std::uint8_t amuletByte = rec.isAmulet ? 1 : 0;
            bool ok = true;
            ok = ok && intfc->WriteRecordData(&rec.baseFormId, sizeof(rec.baseFormId));
            ok = ok && intfc->WriteRecordData(&rarityByte, sizeof(rarityByte));
            ok = ok && intfc->WriteRecordData(&rec.itemLevel, sizeof(rec.itemLevel));
            ok = ok && intfc->WriteRecordData(&slotGroupByte, sizeof(slotGroupByte));
            ok = ok && intfc->WriteRecordData(&defSubByte, sizeof(defSubByte));
            ok = ok && intfc->WriteRecordData(&amuletByte, sizeof(amuletByte));
            ok = ok && WriteString(intfc, rec.weaponTag);
            ok = ok && WriteAffixList(intfc, rec.affixes);
            // v2 확장: 고유 아이템 ID, 주얼 색상, 고정 임플리싯 어픽스
            ok = ok && WriteString(intfc, rec.uniqueId);
            ok = ok && intfc->WriteRecordData(&rec.jewelColor, sizeof(rec.jewelColor));
            ok = ok && WriteAffixList(intfc, rec.implicitAffixes);
            return ok;
        }

        struct ReadPoEItemResult
        {
            bool streamOk = false;
            bool baseResolved = false;
            RE::FormID rawBaseFormId = 0;
        };

        ReadPoEItemResult ReadPoEItemRecord(
            SKSE::SerializationInterface* intfc,
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

            // 5~6단계(결정 161, 164, 166): 정신력 오라 토글, 보스·4차 결투장 이정표 달성 수(0xFu), 패시브 주얼 소켓 선합산
            l1.activeAuraMask = state.activeAuraMask;
            l1.completedMilestoneCount = static_cast<std::uint8_t>(
                std::popcount(static_cast<std::uint32_t>(state.tree.ascendancyMilestonesBitmask & 0xFu)));
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
            MapDeviceRuntime::ResetOnRevert();
            if (g_charmSystem) {
                g_charmSystem->ResetOnRevert();
            }
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
            // C-8: 레코드 단위 독립 쓰기 — 한 레코드가 실패해도 뒤 레코드 기록을 계속 시도한다.
            std::uint32_t trackedCount = 0;
            std::uint32_t nodeCount = 0;
            std::uint32_t itemCount = 0;
            std::uint32_t wdrpCount = 0;
            std::uint32_t jewelCount = 0;

            if (!ok) {
                SKSE::log::error("SaveLoad: error while writing CORE record.");
            }

            if (intfc->OpenRecord(SaveLoad::kRecordGemKeys, SaveLoad::kGemKeysVersion)) {
                // C-4, 재검수 R-3: 상한 초과 시 플레이어 소지품 → 바닥 → 그 밖 순으로 남긴다.
                const auto keep = SelectForSave(tracked, kMaxListLen);
                if (keep.size() < tracked.size()) {
                    SKSE::log::warn("SaveLoad: GKEY count {} exceeded cap {}; dropped {} entries not held by the player first.",
                        tracked.size(), kMaxListLen, tracked.size() - keep.size());
                }
                trackedCount = static_cast<std::uint32_t>(keep.size());
                bool gkeyOk = intfc->WriteRecordData(&trackedCount, sizeof(trackedCount));
                for (std::size_t k = 0; k < keep.size() && gkeyOk; ++k) {
                    const auto& entry = tracked[keep[k]];
                    const std::uint8_t worldByte = entry.isWorld ? 1 : 0;
                    gkeyOk = gkeyOk && intfc->WriteRecordData(&entry.key.ownerFormId, sizeof(entry.key.ownerFormId));
                    gkeyOk = gkeyOk && intfc->WriteRecordData(&entry.key.uniqueId, sizeof(entry.key.uniqueId));
                    gkeyOk = gkeyOk && intfc->WriteRecordData(&worldByte, sizeof(worldByte));
                    gkeyOk = gkeyOk && WriteGemInstance(intfc, entry.gem);
                }
                if (!gkeyOk) {
                    SKSE::log::error("SaveLoad: error while writing GKEY record.");
                }
            } else {
                SKSE::log::error("SaveLoad: failed to open GKEY record for writing.");
            }

            // 3단계: 패시브 트리·전직·보스 이정표 영구 상태('TREE' v1)
            if (intfc->OpenRecord(SaveLoad::kRecordTree, SaveLoad::kTreeVersion)) {
                const auto& tr = state.tree;
                const std::uint8_t clsSelByte = tr.classSelected ? 1 : 0;
                bool treeOk = intfc->WriteRecordData(&clsSelByte, sizeof(clsSelByte));
                treeOk = treeOk && intfc->WriteRecordData(&tr.classId, sizeof(tr.classId));
                treeOk = treeOk && WriteString(intfc, tr.ascendancyId);

                nodeCount = static_cast<std::uint32_t>(std::min<std::size_t>(tr.allocatedNodes.size(), kMaxTreeNodes));
                treeOk = treeOk && intfc->WriteRecordData(&nodeCount, sizeof(nodeCount));
                for (std::uint32_t i = 0; i < nodeCount && treeOk; ++i) {
                    treeOk = treeOk && intfc->WriteRecordData(&tr.allocatedNodes[i], sizeof(std::uint32_t));
                }

                std::vector<std::pair<std::uint32_t, std::uint32_t>> sortedMasteries(
                    tr.masterySelections.begin(), tr.masterySelections.end());
                std::sort(sortedMasteries.begin(), sortedMasteries.end());
                const auto masteryCount = static_cast<std::uint32_t>(std::min<std::size_t>(sortedMasteries.size(), kMaxListLen));
                treeOk = treeOk && intfc->WriteRecordData(&masteryCount, sizeof(masteryCount));
                for (std::uint32_t i = 0; i < masteryCount && treeOk; ++i) {
                    treeOk = treeOk && intfc->WriteRecordData(&sortedMasteries[i].first, sizeof(std::uint32_t));
                    treeOk = treeOk && intfc->WriteRecordData(&sortedMasteries[i].second, sizeof(std::uint32_t));
                }

                treeOk = treeOk && intfc->WriteRecordData(&tr.bossKillCount, sizeof(tr.bossKillCount));
                treeOk = treeOk && intfc->WriteRecordData(&tr.dragonKillCount, sizeof(tr.dragonKillCount));
                treeOk = treeOk && intfc->WriteRecordData(&tr.ascendancyMilestonesBitmask, sizeof(tr.ascendancyMilestonesBitmask));
                if (!treeOk) {
                    SKSE::log::error("SaveLoad: error while writing TREE record.");
                }
            } else {
                SKSE::log::error("SaveLoad: failed to open TREE record for writing.");
            }

            // 4~5단계: 제작/감정/고유/벨트/주얼 PoE 아이템('ITEM' v2) + starterCurrencyGiven 헤더
            if (intfc->OpenRecord(SaveLoad::kRecordItems, SaveLoad::kItemsVersion)) {
                const std::uint8_t starterCurrByte = state.starterCurrencyGiven ? 1 : 0;
                // C-4, 재검수 R-3: GKEY와 같은 순위로 남긴다(장비 칸 GEAR가 가리키는 플레이어 소지품이 빠지지 않게).
                const auto keep = SelectForSave(trackedItems, kMaxItemRecords);
                if (keep.size() < trackedItems.size()) {
                    SKSE::log::warn("SaveLoad: ITEM count {} exceeded cap {}; dropped {} entries not held by the player first.",
                        trackedItems.size(), kMaxItemRecords, trackedItems.size() - keep.size());
                }
                itemCount = static_cast<std::uint32_t>(keep.size());
                bool itemOk = intfc->WriteRecordData(&starterCurrByte, sizeof(starterCurrByte));
                itemOk = itemOk && intfc->WriteRecordData(&itemCount, sizeof(itemCount));
                for (std::size_t k = 0; k < keep.size() && itemOk; ++k) {
                    const auto& entry = trackedItems[keep[k]];
                    const std::uint8_t worldByte = entry.isWorld ? 1 : 0;
                    itemOk = itemOk && intfc->WriteRecordData(&entry.key.ownerFormId, sizeof(entry.key.ownerFormId));
                    itemOk = itemOk && intfc->WriteRecordData(&entry.key.uniqueId, sizeof(entry.key.uniqueId));
                    itemOk = itemOk && intfc->WriteRecordData(&worldByte, sizeof(worldByte));
                    itemOk = itemOk && WritePoEItemRecordV2(intfc, entry.record);
                }
                if (!itemOk) {
                    SKSE::log::error("SaveLoad: error while writing ITEM record.");
                }
            } else {
                SKSE::log::error("SaveLoad: failed to open ITEM record for writing.");
            }

            // 4단계(검수 C4): 활성 바닥 드랍·빛기둥('WDRP' v1)
            if (intfc->OpenRecord(SaveLoad::kRecordWorldDrops, SaveLoad::kWorldDropsVersion)) {
                const std::size_t startIdx = (savedWorldDrops.size() > kMaxWorldDropRecords)
                    ? (savedWorldDrops.size() - kMaxWorldDropRecords) : 0;
                wdrpCount = static_cast<std::uint32_t>(savedWorldDrops.size() - startIdx);
                bool wdrpOk = intfc->WriteRecordData(&wdrpCount, sizeof(wdrpCount));
                for (std::size_t i = startIdx; i < savedWorldDrops.size() && wdrpOk; ++i) {
                    const auto& w = savedWorldDrops[i];
                    wdrpOk = wdrpOk && intfc->WriteRecordData(&w.dropRefFormId, sizeof(w.dropRefFormId));
                    wdrpOk = wdrpOk && intfc->WriteRecordData(&w.corpseRefFormId, sizeof(w.corpseRefFormId));
                    wdrpOk = wdrpOk && intfc->WriteRecordData(&w.beamRefFormId, sizeof(w.beamRefFormId));
                    wdrpOk = wdrpOk && intfc->WriteRecordData(&w.worldDropTier, sizeof(w.worldDropTier));
                }
                if (!wdrpOk) {
                    SKSE::log::error("SaveLoad: error while writing WDRP record.");
                }
            } else {
                SKSE::log::error("SaveLoad: failed to open WDRP record for writing.");
            }

            // 5단계(결정 161): 정신력 오라 토글 + 5단계 시작 젬 지급 여부('SPRT' v1)
            if (intfc->OpenRecord(SaveLoad::kRecordSpirit, SaveLoad::kSpiritVersion)) {
                const std::uint8_t starterS5Byte = state.starterStage5GemsGiven ? 1 : 0;
                bool sprtOk = intfc->WriteRecordData(&state.activeAuraMask, sizeof(state.activeAuraMask));
                sprtOk = sprtOk && intfc->WriteRecordData(&starterS5Byte, sizeof(starterS5Byte));
                if (!sprtOk) {
                    SKSE::log::error("SaveLoad: error while writing SPRT record.");
                }
            } else {
                SKSE::log::error("SaveLoad: failed to open SPRT record for writing.");
            }

            // 5단계(결정 162, 164): 가상 반지2/벨트 + 참 슬롯 + 패시브 주얼 소켓('GEAR' v1)
            if (intfc->OpenRecord(SaveLoad::kRecordGear, SaveLoad::kGearVersion)) {
                bool gearOk = intfc->WriteRecordData(&state.virtualRing2Key.ownerFormId, sizeof(state.virtualRing2Key.ownerFormId));
                gearOk = gearOk && intfc->WriteRecordData(&state.virtualRing2Key.uniqueId, sizeof(state.virtualRing2Key.uniqueId));
                gearOk = gearOk && intfc->WriteRecordData(&state.virtualRing2BaseFormId, sizeof(state.virtualRing2BaseFormId));
                gearOk = gearOk && intfc->WriteRecordData(&state.virtualBeltKey.ownerFormId, sizeof(state.virtualBeltKey.ownerFormId));
                gearOk = gearOk && intfc->WriteRecordData(&state.virtualBeltKey.uniqueId, sizeof(state.virtualBeltKey.uniqueId));
                gearOk = gearOk && intfc->WriteRecordData(&state.virtualBeltBaseFormId, sizeof(state.virtualBeltBaseFormId));

                const auto charmCount = static_cast<std::uint32_t>(state.charmSlots.size());
                gearOk = gearOk && intfc->WriteRecordData(&charmCount, sizeof(charmCount));
                for (std::uint32_t c = 0; c < charmCount && gearOk; ++c) {
                    const auto& cs = state.charmSlots[c];
                    const std::uint8_t occ = cs.occupied ? 1 : 0;
                    const auto kindByte = static_cast<std::uint8_t>(cs.kind);
                    gearOk = gearOk && intfc->WriteRecordData(&occ, sizeof(occ));
                    gearOk = gearOk && intfc->WriteRecordData(&kindByte, sizeof(kindByte));
                    gearOk = gearOk && intfc->WriteRecordData(&cs.charges, sizeof(cs.charges));
                }

                std::vector<std::pair<std::uint32_t, Core::PoEItemRecord>> sortedJewels(
                    state.socketedJewels.begin(), state.socketedJewels.end());
                std::sort(sortedJewels.begin(), sortedJewels.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
                if (sortedJewels.size() > kMaxSocketedJewels) {
                    SKSE::log::warn("SaveLoad: socketed jewel count {} exceeded cap {}.", sortedJewels.size(), kMaxSocketedJewels);
                }
                jewelCount = static_cast<std::uint32_t>(std::min<std::size_t>(sortedJewels.size(), kMaxSocketedJewels));
                gearOk = gearOk && intfc->WriteRecordData(&jewelCount, sizeof(jewelCount));
                for (std::uint32_t j = 0; j < jewelCount && gearOk; ++j) {
                    gearOk = gearOk && intfc->WriteRecordData(&sortedJewels[j].first, sizeof(std::uint32_t));
                    gearOk = gearOk && WritePoEItemRecordV2(intfc, sortedJewels[j].second);
                }
                if (!gearOk) {
                    SKSE::log::error("SaveLoad: error while writing GEAR record.");
                }
            } else {
                SKSE::log::error("SaveLoad: failed to open GEAR record for writing.");
            }

            // Q-1: 세이브 시점 잔존 NPC SpeedMult 델타('SPDM' v1) 기록
            std::uint32_t spdmCount = 0;
            if (!savedSpeedMods.empty()) {
                if (intfc->OpenRecord(SaveLoad::kRecordSpeedMods, SaveLoad::kSpeedModsVersion)) {
                    spdmCount = static_cast<std::uint32_t>(std::min<std::size_t>(savedSpeedMods.size(), kMaxListLen));
                    if (savedSpeedMods.size() > kMaxListLen) {
                        // Codex 지적 13: 상한을 넘은 액터의 원복 근거는 이 세이브에 남지 않는다. 조용히 버리지 않고 알린다.
                        SKSE::log::warn("SaveLoad: SPDM has {} actors over cap {}; {} SpeedMult revert(s) will not be saved.",
                            savedSpeedMods.size(), kMaxListLen, savedSpeedMods.size() - kMaxListLen);
                    }
                    bool spdmOk = intfc->WriteRecordData(&spdmCount, sizeof(spdmCount));
                    for (std::uint32_t i = 0; i < spdmCount && spdmOk; ++i) {
                        spdmOk = spdmOk && intfc->WriteRecordData(&savedSpeedMods[i].first, sizeof(RE::FormID));
                        spdmOk = spdmOk && intfc->WriteRecordData(&savedSpeedMods[i].second, sizeof(float));
                    }
                    if (!spdmOk) {
                        SKSE::log::error("SaveLoad: error while writing SPDM record.");
                    }
                } else {
                    SKSE::log::error("SaveLoad: failed to open SPDM record for writing.");
                }
            }

            // 결정 169, R4: MAPS v2는 던전 키와 팩 판 번호를 저장한다.
            // 결정 176 ④: 허가 바이트는 R4-B1에서 v2에 더했다. v2는 그 전까지 게임에서 저장된 적이 없다.
            std::uint32_t mapSpawnCount = 0;
            if (intfc->OpenRecord(SaveLoad::kRecordMaps, SaveLoad::kMapsVersion)) {
                const auto& atl = state.atlas;
                const std::uint8_t tribByte = atl.firstTributeGiven ? 1 : 0;
                bool mapsOk = intfc->WriteRecordData(&atl.completedTierMask, sizeof(atl.completedTierMask));
                mapsOk = mapsOk && intfc->WriteRecordData(&atl.totalMapsCleared, sizeof(atl.totalMapsCleared));
                mapsOk = mapsOk && intfc->WriteRecordData(&atl.highestTierCleared, sizeof(atl.highestTierCleared));
                mapsOk = mapsOk && intfc->WriteRecordData(&tribByte, sizeof(tribByte));

                const auto& am = savedMapState;
                const std::uint8_t activeByte = am.active ? 1 : 0;
                const std::uint8_t arenaByte = am.isArena4 ? 1 : 0;
                const std::uint8_t bossKilledByte = am.bossKilled ? 1 : 0;
                const std::uint8_t failedByte = am.failed ? 1 : 0;
                const std::uint8_t authorizedByte = am.playerAuthorized ? 1 : 0;
                mapsOk = mapsOk && intfc->WriteRecordData(&activeByte, sizeof(activeByte));
                mapsOk = mapsOk && intfc->WriteRecordData(&arenaByte, sizeof(arenaByte));
                mapsOk = mapsOk && intfc->WriteRecordData(&bossKilledByte, sizeof(bossKilledByte));
                mapsOk = mapsOk && intfc->WriteRecordData(&failedByte, sizeof(failedByte));
                mapsOk = mapsOk && intfc->WriteRecordData(&authorizedByte, sizeof(authorizedByte));
                mapsOk = mapsOk && intfc->WriteRecordData(&am.runId, sizeof(am.runId));
                mapsOk = mapsOk && intfc->WriteRecordData(&am.tier, sizeof(am.tier));
                mapsOk = mapsOk && Core::WriteMapSaveIdentity(
                    [intfc](const void* data, std::uint32_t size) { return intfc->WriteRecordData(data, size); }, am);
                mapsOk = mapsOk && intfc->WriteRecordData(&am.portalsRemaining, sizeof(am.portalsRemaining));
                mapsOk = mapsOk && intfc->WriteRecordData(&am.arenaPhase, sizeof(am.arenaPhase));
                mapsOk = mapsOk && WriteAffixList(intfc, am.mods);
                mapsOk = mapsOk && intfc->WriteRecordData(&am.returnCellFormId, sizeof(am.returnCellFormId));
                mapsOk = mapsOk && intfc->WriteRecordData(&am.returnWorldspaceFormId, sizeof(am.returnWorldspaceFormId));
                mapsOk = mapsOk && intfc->WriteRecordData(am.returnPos.data(), static_cast<std::uint32_t>(sizeof(am.returnPos)));
                mapsOk = mapsOk && intfc->WriteRecordData(am.returnAngle.data(), static_cast<std::uint32_t>(sizeof(am.returnAngle)));
                mapsOk = mapsOk && intfc->WriteRecordData(&am.bossRefFormId, sizeof(am.bossRefFormId));

                mapSpawnCount = static_cast<std::uint32_t>(
                    std::min<std::size_t>(am.spawnedRefFormIds.size(), Core::kMaxMapSpawnedActors));
                mapsOk = mapsOk && intfc->WriteRecordData(&mapSpawnCount, sizeof(mapSpawnCount));
                for (std::uint32_t s = 0; s < mapSpawnCount && mapsOk; ++s) {
                    mapsOk = mapsOk && intfc->WriteRecordData(&am.spawnedRefFormIds[s], sizeof(std::uint32_t));
                }
                if (!mapsOk) {
                    SKSE::log::error("SaveLoad: error while writing MAPS record.");
                }
            } else {
                SKSE::log::error("SaveLoad: failed to open MAPS record for writing.");
            }

            SKSE::log::info(
                "SaveLoad: saved CORE(Lv {}), GKEY({}), TREE(nodes={}), ITEM({}), WDRP({}), SPRT(0x{:X}), GEAR(jewels={}), SPDM({}), MAPS(atlas=0x{:04X}, active={}, spawns={}).",
                state.exileLevel, trackedCount, nodeCount, itemCount, wdrpCount,
                state.activeAuraMask, jewelCount, spdmCount,
                state.atlas.completedTierMask, savedMapState.active, mapSpawnCount);
        }

        // SKSE 코세이브 콜백: 불러오기(순수 메모리 복원만 수행, 엔진 조작은 주 갱신으로 이연)
        void OnLoad(SKSE::SerializationInterface* intfc)
        {
            if (!intfc) {
                return;
            }

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
            Core::AtlasState loadedAtlas{};
            Core::ActiveMapState loadedActiveMap{};
            RE::FormID loadedLegacyMapCell = 0;
            bool hasMaps = false;

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
                    sawGemKeysRecord = true;
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
                        // C-3: GKEY 뒷부분이 잘렸더라도 앞선 완전 파싱 항목은 살려 C-1 고아화를 최소화한다.
                        if (!tmp.empty()) {
                            loadedGems = std::move(tmp);
                        }
                        SKSE::log::error("SaveLoad: failed to parse full GKEY record (truncated or corrupt; recovered {} entries).",
                            loadedGems.size());
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
                        tmpTree.ascendancyMilestonesBitmask &= 0xFu;
                        loadedTree = std::move(tmpTree);
                        hasTree = true;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse TREE record (truncated or corrupt); falling back to empty tree.");
                    }
                } else if (type == SaveLoad::kRecordItems) {
                    sawItemsRecord = true;
                    if (version != 1 && version != SaveLoad::kItemsVersion) {
                        SKSE::log::warn("SaveLoad: unsupported ITEM record version {} (expected 1 or {}).", version, SaveLoad::kItemsVersion);
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
                } else if (type == SaveLoad::kRecordSpirit) {
                    sawSpiritRecord = true;
                    if (version != SaveLoad::kSpiritVersion) {
                        SKSE::log::warn("SaveLoad: unsupported SPRT record version {} (expected {}).", version, SaveLoad::kSpiritVersion);
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
                } else if (type == SaveLoad::kRecordGear) {
                    if (version != SaveLoad::kGearVersion) {
                        SKSE::log::warn("SaveLoad: unsupported GEAR record version {} (expected {}).", version, SaveLoad::kGearVersion);
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
                } else if (type == SaveLoad::kRecordSpeedMods) {
                    if (version != SaveLoad::kSpeedModsVersion) {
                        SKSE::log::warn("SaveLoad: unsupported SPDM record version {} (expected {}).", version, SaveLoad::kSpeedModsVersion);
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
                } else if (type == SaveLoad::kRecordMaps) {
                    if (version != kLegacyMapsVersion && version != SaveLoad::kMapsVersion) {
                        SKSE::log::warn("SaveLoad: unsupported MAPS record version {}.", version);
                        continue;
                    }
                    Core::AtlasState tmpAtlas{};
                    Core::ActiveMapState tmpMap{};
                    std::uint8_t tribByte = 0;
                    std::uint8_t activeByte = 0, arenaByte = 0, bossKilledByte = 0, failedByte = 0, authorizedByte = 0;
                    std::uint8_t legacyIndex = 0;
                    RE::FormID rawRetCell = 0, rawRetWs = 0, rawBossRef = 0;
                    std::uint32_t spawnCount = 0;
                    bool ok = true;
                    ok = ok && ReadExactPod(intfc, tmpAtlas.completedTierMask);
                    ok = ok && ReadExactPod(intfc, tmpAtlas.totalMapsCleared);
                    ok = ok && ReadExactPod(intfc, tmpAtlas.highestTierCleared);
                    ok = ok && ReadExactPod(intfc, tribByte);

                    ok = ok && ReadExactPod(intfc, activeByte);
                    ok = ok && ReadExactPod(intfc, arenaByte);
                    ok = ok && ReadExactPod(intfc, bossKilledByte);
                    ok = ok && ReadExactPod(intfc, failedByte);
                    if (version != kLegacyMapsVersion) {
                        ok = ok && ReadExactPod(intfc, authorizedByte);
                    }
                    ok = ok && ReadExactPod(intfc, tmpMap.runId);
                    ok = ok && ReadExactPod(intfc, tmpMap.tier);
                    if (version == kLegacyMapsVersion) {
                        ok = ok && ReadExactPod(intfc, legacyIndex);
                    } else {
                        ok = ok && Core::ReadMapSaveIdentity(
                            [intfc](void* data, std::uint32_t size) { return ReadExact(intfc, data, size); }, tmpMap);
                    }
                    ok = ok && ReadExactPod(intfc, tmpMap.portalsRemaining);
                    ok = ok && ReadExactPod(intfc, tmpMap.arenaPhase);
                    ok = ok && ReadAffixList(intfc, tmpMap.mods);
                    ok = ok && ReadExactPod(intfc, rawRetCell);
                    ok = ok && ReadExactPod(intfc, rawRetWs);
                    ok = ok && ReadExact(intfc, tmpMap.returnPos.data(), static_cast<std::uint32_t>(sizeof(tmpMap.returnPos)));
                    ok = ok && ReadExact(intfc, tmpMap.returnAngle.data(), static_cast<std::uint32_t>(sizeof(tmpMap.returnAngle)));
                    ok = ok && ReadExactPod(intfc, rawBossRef);
                    ok = ok && ReadExactPod(intfc, spawnCount);

                    if (ok && spawnCount <= Core::kMaxMapSpawnedActors) {
                        tmpMap.spawnedRefFormIds.reserve(spawnCount);
                        for (std::uint32_t s = 0; s < spawnCount && ok; ++s) {
                            RE::FormID rawSpawn = 0;
                            ok = ok && ReadExactPod(intfc, rawSpawn);
                            if (ok && rawSpawn != 0) {
                                RE::FormID resSpawn = 0;
                                // 배치2 검수 Major #4: 동적 생성 참조(0xFFxxxxxx) 대역만 스폰 목록으로 허용
                                if (intfc->ResolveFormID(rawSpawn, resSpawn) &&
                                    (resSpawn & 0xFF000000u) == 0xFF000000u) {
                                    tmpMap.spawnedRefFormIds.push_back(resSpawn);
                                }
                            }
                        }
                    } else {
                        ok = false;
                    }

                    if (ok) {
                        tmpAtlas.highestTierCleared = std::min<std::uint8_t>(tmpAtlas.highestTierCleared, Core::kMaxMapTier);
                        tmpAtlas.firstTributeGiven = (tribByte != 0);

                        tmpMap.active = (activeByte != 0);
                        tmpMap.isArena4 = (arenaByte != 0);
                        tmpMap.bossKilled = (bossKilledByte != 0);
                        tmpMap.failed = (failedByte != 0);
                        tmpMap.playerAuthorized = (authorizedByte != 0);
                        tmpMap.tier = std::clamp<std::uint8_t>(tmpMap.tier, Core::kMinMapTier, Core::kMaxMapTier);
                        tmpMap.portalsRemaining = std::min<std::uint8_t>(
                            tmpMap.portalsRemaining,
                            tmpMap.isArena4 ? Core::kArena4MaxPortals : Core::kNormalMapMaxPortals);
                        tmpMap.arenaPhase = std::clamp<std::uint8_t>(tmpMap.arenaPhase, 1u, 3u);
                        tmpMap.summary = Core::MapSystem::AggregateMapMods(tmpMap.mods);
                        // 손상된 귀환 좌표는 0으로 바꿔 쓰지 않는다. 셀·월드스페이스를 비워 고정 마커로 가게 한다.
                        const bool returnPosValid = std::ranges::all_of(tmpMap.returnPos, [](float c) { return std::isfinite(c); });
                        for (float& a : tmpMap.returnAngle) {
                            if (!std::isfinite(a)) a = 0.0f;
                        }
                        if (rawRetCell != 0) {
                            RE::FormID res = 0;
                            if (intfc->ResolveFormID(rawRetCell, res) && res != 0) {
                                tmpMap.returnCellFormId = res;
                            }
                        }
                        if (rawRetWs != 0) {
                            RE::FormID res = 0;
                            if (intfc->ResolveFormID(rawRetWs, res) && res != 0) {
                                tmpMap.returnWorldspaceFormId = res;
                            }
                        }
                        if (!returnPosValid) {
                            tmpMap.returnCellFormId = 0;
                            tmpMap.returnWorldspaceFormId = 0;
                            tmpMap.returnPos = { 0.0f, 0.0f, 0.0f };
                            SKSE::log::warn("SaveLoad: MAPS return position was not finite; the fixed marker will be used.");
                        }
                        if (rawBossRef != 0) {
                            RE::FormID res = 0;
                            if (intfc->ResolveFormID(rawBossRef, res) &&
                                (res & 0xFF000000u) == 0xFF000000u) {
                                tmpMap.bossRefFormId = res;
                            }
                        }

                        loadedAtlas = tmpAtlas;
                        loadedLegacyMapCell = 0;
                        if (version == kLegacyMapsVersion && tmpMap.active) {
                            loadedLegacyMapCell = tmpMap.isArena4
                                ? kLegacyMapsV1ArenaCell
                                : kLegacyMapsV1Cells[legacyIndex % kLegacyMapsV1Cells.size()];
                        }
                        loadedActiveMap = version == kLegacyMapsVersion && !tmpMap.active
                            ? Core::ActiveMapState{}
                            : std::move(tmpMap);
                        hasMaps = true;
                    } else {
                        SKSE::log::error("SaveLoad: failed to parse MAPS record.");
                    }
                } else {
                    SKSE::log::warn("SaveLoad: ignoring unknown record type 0x{:08X}.", type);
                }
            }

            if (hasMaps) {
                loadedState.atlas = loadedAtlas;
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
            if (hasCore || hasTree || hasItems || hasSpirit || hasGear || hasMaps || sawItemsRecord || sawSpiritRecord || sawGemKeysRecord) {
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
            if (hasMaps) {
                MapDeviceRuntime::RestoreFromSave(std::move(loadedActiveMap), loadedLegacyMapCell);
            } else {
                MapDeviceRuntime::ResetOnRevert();
            }
            {
                std::scoped_lock lock(g_speedModMutex);
                g_pendingSpeedModReverts = std::move(loadedSpeedMods);
            }
            SaveLoad::SyncStateToStatEngine();

            SKSE::log::info(
                "SaveLoad: OnLoad restored CORE({}), TREE(nodes={}), GKEY({}), ITEM({}), WDRP({}), SPRT({}), GEAR(jewels={}), MAPS(atlas=0x{:04X}).",
                hasCore, loadedState.tree.allocatedNodes.size(),
                GemInventory::CountTracked(), ItemTracker::CountTracked(), WorldDropTracker::ActiveCount(),
                hasSpirit, loadedState.socketedJewels.size(), loadedState.atlas.completedTierMask);
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

    void SaveLoad::SchedulePostLoadOrNewGameInit(bool isNewGame)
    {
        MapDeviceRuntime::DisarmUninstallCleanup();
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
            MapDeviceRuntime::OnPostLoadRebind();

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

    void SaveLoad::DeferSpeedRevert(RE::FormID actorId, float appliedDelta)
    {
        if (actorId == 0 || !std::isfinite(appliedDelta) || std::abs(appliedDelta) < 0.01f) {
            return;
        }
        if ((actorId >> 24) == 0xFF) {
            SKSE::log::info("SaveLoad: not deferring SpeedMult revert for vanished dynamic actor {:08X} (FormID may be reused).", actorId);
            return;
        }
        std::scoped_lock lock(g_speedModMutex);
        g_pendingSpeedModReverts.emplace_back(actorId, appliedDelta);
        if (g_pendingSpeedModReverts.size() > kMaxPendingSpeedReverts) {
            // 영영 다시 나타나지 않는 액터(지워진 임시 참조)의 항목이 끝없이 쌓이지 않게 가장 오래된 것부터 버린다.
            const auto excess = g_pendingSpeedModReverts.size() - kMaxPendingSpeedReverts;
            SKSE::log::warn("SaveLoad: pending SpeedMult reverts exceeded {}; dropping {} oldest.", kMaxPendingSpeedReverts, excess);
            g_pendingSpeedModReverts.erase(g_pendingSpeedModReverts.begin(),
                g_pendingSpeedModReverts.begin() + static_cast<std::ptrdiff_t>(excess));
        }
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
    void SaveLoad::RevertAllPendingSpeedModsNow()
    {
        const auto [reverted, remaining] = ApplyPendingSpeedRevertsNow();
        if (remaining > 0) {
            SKSE::log::warn("SaveLoad: uninstall cleanup reverted {} SpeedMult entr(ies); {} actor(s) not found now and kept for retry.",
                reverted, remaining);
        } else {
            SKSE::log::info("SaveLoad: uninstall cleanup reverted {} SpeedMult entr(ies).", reverted);
        }
    }
}
