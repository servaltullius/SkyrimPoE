#include "PCH.h"
#include "runtime/GemInventory.h"
#include "runtime/FormIds.h"
#include "runtime/UniqueIdUtil.h"

namespace SkyrimPoE::Runtime
{
    namespace
    {
        // 검수 C2: UniqueIdUtil 공용 비중복 대역 사용(젬 바닥 스탬프 0x7F00..0xBFFF)
        constexpr std::uint16_t kInventoryUidMin = UniqueIdUtil::kInventoryUidMin;
        constexpr std::uint16_t kInventoryUidMax = UniqueIdUtil::kInventoryUidMax;
        constexpr std::uint16_t kWorldStampMin = UniqueIdUtil::kGemWorldStampMin;
        constexpr std::uint16_t kWorldStampMax = UniqueIdUtil::kGemWorldStampMax;

        std::mutex g_mutex;
        std::vector<TrackedGemEntry> g_entries;
        std::uint16_t g_nextWorldStamp = kWorldStampMin;

        Core::CommandQueue* g_queue = nullptr;
        bool g_installed = false;

        [[nodiscard]] bool IsStage2GemForm(RE::FormID fullFormId) noexcept
        {
            for (const auto& entry : FormIds::kStage2GemForms) {
                auto* form = FormIds::Lookup<RE::TESObjectMISC>(entry.localFormId);
                if (form && form->GetFormID() == fullFormId) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] RE::ExtraDataList* CreateGameExtraDataList()
        {
            return UniqueIdUtil::CreateGameExtraDataList();
        }

        // 공용 할당기 위임(엔진 + GemInventory + ItemTracker UID 모두 회피, 검수 C2)
        [[nodiscard]] std::uint16_t NextFreeInventoryUniqueId(RE::TESObjectREFR* owner)
        {
            return UniqueIdUtil::AllocateInventoryUniqueId(owner);
        }

        // g_mutex가 잡힌 상태에서 호출: 바닥 스탬프 대역([0x7F00, 0xFFFD])에서 미사용 번호를 할당한다.
        [[nodiscard]] std::uint16_t AllocateWorldStampLocked()
        {
            std::unordered_set<std::uint16_t> used;
            for (const auto& e : g_entries) {
                if (e.isWorld && e.key.uniqueId >= kWorldStampMin && e.key.uniqueId <= kWorldStampMax) {
                    used.insert(e.key.uniqueId);
                }
            }
            const std::uint32_t span = static_cast<std::uint32_t>(kWorldStampMax - kWorldStampMin + 1);
            for (std::uint32_t offset = 0; offset < span; ++offset) {
                const auto candidate = static_cast<std::uint16_t>(
                    kWorldStampMin + ((static_cast<std::uint32_t>(g_nextWorldStamp - kWorldStampMin) + offset) % span));
                if (!used.contains(candidate)) {
                    g_nextWorldStamp = (candidate == kWorldStampMax)
                        ? kWorldStampMin
                        : static_cast<std::uint16_t>(candidate + 1);
                    return candidate;
                }
            }
            return kWorldStampMax;
        }

        [[nodiscard]] RE::ExtraDataList* FindExtraListByUniqueId(
            RE::TESObjectREFR* owner,
            RE::TESBoundObject* baseObj,
            const Core::InventoryItemKey& key)
        {
            if (!owner || !baseObj || !key.IsValid()) {
                return nullptr;
            }
            auto* changes = owner->GetInventoryChanges(true);
            if (!changes || !changes->entryList) {
                return nullptr;
            }
            for (auto* entry : *changes->entryList) {
                if (!entry || entry->object != baseObj || !entry->extraLists) {
                    continue;
                }
                for (auto* extra : *entry->extraLists) {
                    if (!extra) continue;
                    const auto* unique = extra->GetByType<RE::ExtraUniqueID>();
                    if (unique && unique->baseID == key.ownerFormId && unique->uniqueID == key.uniqueId) {
                        return extra;
                    }
                }
            }
            return nullptr;
        }

        [[nodiscard]] std::int32_t CountPlainItems(RE::TESObjectREFR* owner, RE::TESBoundObject* baseObj)
        {
            if (!owner || !baseObj) {
                return 0;
            }
            auto* changes = owner->GetInventoryChanges(true);
            if (!changes || !changes->entryList) {
                return 0;
            }
            for (auto* entry : *changes->entryList) {
                if (!entry || entry->object != baseObj) {
                    continue;
                }
                std::int32_t markedCount = 0;
                if (entry->extraLists) {
                    for (auto* extra : *entry->extraLists) {
                        if (extra && extra->HasType<RE::ExtraUniqueID>()) {
                            markedCount += extra->GetCount();
                        }
                    }
                }
                return entry->countDelta - markedCount;
            }
            return 0;
        }

        // 결정 123: 바닥 참조에 ExtraUniqueID 스탬프를 부착한다(뮤텍스 밖에서 호출).
        // 성공하면 g_entries의 uniqueId를 즉시 갱신하고 true를 반환한다.
        bool TryStampDroppedWorldRef(RE::FormID refFormId, std::uint64_t instanceId, std::uint16_t stampUid)
        {
            auto* form = RE::TESForm::LookupByID(refFormId);
            auto* refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!refr || refr->IsDeleted() || refr->IsDisabled()) {
                return false;
            }
            const auto* base = refr->GetBaseObject();
            if (!base || !IsStage2GemForm(base->GetFormID())) {
                return false;
            }

            if (auto* unique = refr->extraList.GetByType<RE::ExtraUniqueID>()) {
                unique->baseID = refFormId;
                unique->uniqueID = stampUid;
            } else {
                refr->extraList.Add(new RE::ExtraUniqueID(refFormId, stampUid));
            }

            std::scoped_lock lock(g_mutex);
            for (auto& entry : g_entries) {
                if (entry.gem.instanceId == instanceId && entry.isWorld && entry.key.ownerFormId == refFormId) {
                    entry.key.uniqueId = stampUid;
                    break;
                }
            }
            return true;
        }

        class UniqueIdEventSink final : public RE::BSTEventSink<RE::TESUniqueIDChangeEvent>
        {
        public:
            static UniqueIdEventSink& Get()
            {
                static UniqueIdEventSink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESUniqueIDChangeEvent* event,
                RE::BSTEventSource<RE::TESUniqueIDChangeEvent>*) override
            {
                // 무효한 이전 키(0:0)는 추적 대상이 아니므로 즉시 무시해 무관한 아이템 탈취를 막는다.
                if (!event || event->oldBaseID == 0 || event->oldUniqueID == 0) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                std::uint64_t droppedInstanceId = 0;
                RE::FormID droppedRefId = 0;
                std::uint16_t stampUid = 0;

                {
                    std::scoped_lock lock(g_mutex);
                    for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
                        if (it->key.ownerFormId == event->oldBaseID &&
                            it->key.uniqueId == event->oldUniqueID) {
                            // 아이템 소멸/삭제(newBaseID == 0 && newUniqueID == 0) 시 좀비 엔트리를 남기지 않고 제거한다.
                            if (event->newBaseID == 0 && event->newUniqueID == 0) {
                                g_entries.erase(it);
                                return RE::BSEventNotifyControl::kContinue;
                            }

                            const bool wasWorld = it->isWorld;
                            it->key.ownerFormId = event->newBaseID;
                            it->key.uniqueId = event->newUniqueID;
                            it->isWorld = (event->newUniqueID == 0);

                            if (!wasWorld && it->isWorld && event->newBaseID != 0) {
                                droppedInstanceId = it->gem.instanceId;
                                droppedRefId = event->newBaseID;
                                stampUid = AllocateWorldStampLocked();
                            }
                            break;
                        }
                    }
                }

                // 결정 123: 버릴 때 바닥 참조 번호 남기기는 결정 119의 예외로 이벤트 안에서(뮤텍스 해제 후) 즉시 수행한다.
                // 참조가 아직 조회되지 않을 때만 주 갱신 큐로 1회 보완을 예약한다.
                if (droppedInstanceId != 0 && droppedRefId != 0) {
                    if (!TryStampDroppedWorldRef(droppedRefId, droppedInstanceId, stampUid) && g_queue) {
                        g_queue->Enqueue([droppedRefId, droppedInstanceId, stampUid]() {
                            (void)TryStampDroppedWorldRef(droppedRefId, droppedInstanceId, stampUid);
                        });
                    }
                }

                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            UniqueIdEventSink() = default;
        };
    }

    bool GemInventory::Install(Core::CommandQueue* queue)
    {
        if (g_installed) {
            return true;
        }
        g_queue = queue;
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            SKSE::log::error("GemInventory: ScriptEventSourceHolder unavailable.");
            return false;
        }
        holder->AddEventSink<RE::TESUniqueIDChangeEvent>(&UniqueIdEventSink::Get());
        UniqueIdUtil::RegisterUsedInventoryUidCollector([](std::unordered_set<std::uint16_t>& used) {
            std::scoped_lock lock(g_mutex);
            for (const auto& e : g_entries) {
                if (!e.isWorld && e.key.uniqueId >= kInventoryUidMin && e.key.uniqueId <= kInventoryUidMax) {
                    used.insert(e.key.uniqueId);
                }
            }
        });
        g_installed = true;
        SKSE::log::info("GemInventory: TESUniqueIDChangeEvent sink registered.");
        return true;
    }

    std::size_t GemInventory::GiveStarterGems(RE::PlayerCharacter* player)
    {
        if (!player) {
            return 0;
        }
        std::size_t added = 0;
        for (const auto& entry : FormIds::kStage2GemForms) {
            auto* misc = FormIds::Lookup<RE::TESObjectMISC>(entry.localFormId);
            if (!misc) {
                SKSE::log::warn("GemInventory: starter gem form '{}' (0x{:06X}) missing from {}.",
                    entry.gemId, entry.localFormId, FormIds::kPluginName);
                continue;
            }
            player->AddObjectToContainer(misc, nullptr, 1, nullptr);
            ++added;
        }
        SKSE::log::info("GemInventory: gave {}/{} starter gems to player.", added, FormIds::kStage2GemForms.size());
        return added;
    }

    PlayerGemInventorySnapshot GemInventory::ListPlayerGems(RE::PlayerCharacter* player)
    {
        PlayerGemInventorySnapshot snap;
        if (!player) {
            return snap;
        }
        const auto playerId = player->GetFormID();

        // 1. 플레이어 인벤토리에 실제로 존재하는 추적 데이터 젬 수집
        std::vector<TrackedGemEntry> candidates;
        {
            std::scoped_lock lock(g_mutex);
            for (const auto& entry : g_entries) {
                if (!entry.isWorld && entry.key.ownerFormId == playerId && entry.key.IsValid()) {
                    candidates.push_back(entry);
                }
            }
        }
        for (auto& cand : candidates) {
            const auto localId = FormIds::FindLocalFormIdForGem(cand.gem.gemId);
            auto* misc = localId ? FormIds::Lookup<RE::TESObjectMISC>(localId) : nullptr;
            if (misc && FindExtraListByUniqueId(player, misc, cand.key) != nullptr) {
                snap.trackedGems.push_back(std::move(cand));
            }
        }

        // 2. 순정 젬(ExtraUniqueID 없는 겹침 아이템) 수량 집계
        for (const auto& entry : FormIds::kStage2GemForms) {
            auto* misc = FormIds::Lookup<RE::TESObjectMISC>(entry.localFormId);
            if (!misc) {
                continue;
            }
            const auto count = CountPlainItems(player, misc);
            if (count > 0) {
                snap.plainStacks.push_back(PlayerGemInventorySnapshot::PlainStack{
                    std::string(entry.gemId),
                    count
                });
            }
        }
        return snap;
    }

    std::optional<Core::InventoryItemKey> GemInventory::ReturnGemToPlayerInventory(
        RE::PlayerCharacter* player,
        Core::GemInstance gem)
    {
        if (!player) {
            return std::nullopt;
        }
        const auto localId = FormIds::FindLocalFormIdForGem(gem.gemId);
        if (localId == 0) {
            SKSE::log::error("GemInventory: unknown gemId '{}'.", gem.gemId);
            return std::nullopt;
        }
        auto* misc = FormIds::Lookup<RE::TESObjectMISC>(localId);
        if (!misc) {
            SKSE::log::error("GemInventory: MISC form 0x{:06X} for '{}' not found.", localId, gem.gemId);
            return std::nullopt;
        }

        auto* extra = CreateGameExtraDataList();
        if (!extra) {
            SKSE::log::error("GemInventory: failed to allocate ExtraDataList for '{}'.", gem.gemId);
            return std::nullopt;
        }

        const auto ownerId = player->GetFormID();
        const auto uid = NextFreeInventoryUniqueId(player);
        extra->Add(new RE::ExtraUniqueID(ownerId, uid));

        const std::uint64_t instanceId = gem.instanceId;
        Core::InventoryItemKey key{ ownerId, uid };
        {
            std::scoped_lock lock(g_mutex);
            g_entries.push_back(TrackedGemEntry{ key, false, std::move(gem) });
        }

        player->AddObjectToContainer(misc, extra, 1, nullptr);

        // AddObjectToContainer 도중 동기 이벤트로 키가 갱신됐을 수 있으므로 최신 키를 조회해 반환한다.
        {
            std::scoped_lock lock(g_mutex);
            for (const auto& entry : g_entries) {
                if (entry.gem.instanceId == instanceId) {
                    return entry.key;
                }
            }
        }
        return key;
    }

    std::optional<Core::GemInstance> GemInventory::TakeGemFromPlayerInventory(
        RE::PlayerCharacter* player,
        const Core::InventoryItemKey& key,
        std::string_view fallbackGemId,
        std::uint8_t defaultSockets,
        std::uint64_t& nextInstanceId)
    {
        if (!player) {
            return std::nullopt;
        }

        // 1. 추적 중인 데이터 젬 회수 시도
        if (key.IsValid()) {
            std::optional<TrackedGemEntry> found;
            {
                std::scoped_lock lock(g_mutex);
                for (const auto& entry : g_entries) {
                    if (!entry.isWorld && entry.key == key) {
                        found = entry;
                        break;
                    }
                }
            }
            if (found) {
                const auto localId = FormIds::FindLocalFormIdForGem(found->gem.gemId);
                auto* misc = localId ? FormIds::Lookup<RE::TESObjectMISC>(localId) : nullptr;
                auto* extra = FindExtraListByUniqueId(player, misc, key);
                if (misc && extra) {
                    const std::uint64_t targetInstanceId = found->gem.instanceId;
                    player->RemoveItem(misc, 1, RE::ITEM_REMOVE_REASON::kRemove, extra, nullptr);
                    // RemoveItem 중 동기 이벤트로 키가 바뀌었더라도 불변 instanceId로 확실히 제거한다.
                    std::scoped_lock lock(g_mutex);
                    std::erase_if(g_entries, [&](const TrackedGemEntry& e) {
                        return e.gem.instanceId == targetInstanceId;
                    });
                    return found->gem;
                }
            }
            // 결정 137(마): 추적 데이터 젬 키가 명시된 요청이 실패했을 때 순정 젬 스택으로 폴백하지 않는다.
            return std::nullopt;
        }

        // 2. 순정 젬(미장착 겹침 아이템) 1개 소비 후 새 인스턴스 부여(결정 105)
        if (!fallbackGemId.empty()) {
            const auto localId = FormIds::FindLocalFormIdForGem(fallbackGemId);
            auto* misc = localId ? FormIds::Lookup<RE::TESObjectMISC>(localId) : nullptr;
            if (misc && CountPlainItems(player, misc) > 0) {
                player->RemoveItem(misc, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                Core::GemInstance fresh;
                fresh.instanceId = nextInstanceId++;
                fresh.gemId = std::string(fallbackGemId);
                fresh.level = 1;
                fresh.quality = 0;
                fresh.xp = 0;
                fresh.socketCount = defaultSockets;
                return fresh;
            }
        }

        return std::nullopt;
    }

    std::vector<TrackedGemEntry> GemInventory::TakeSnapshotForSave()
    {
        std::scoped_lock lock(g_mutex);
        return g_entries;
    }

    void GemInventory::RestoreFromSave(std::vector<TrackedGemEntry> entries)
    {
        std::scoped_lock lock(g_mutex);
        g_entries = std::move(entries);
        // 복원된 바닥 스탬프와 충돌하지 않도록 g_nextWorldStamp를 최댓값 다음으로 전진한다.
        std::uint16_t maxWorld = 0;
        for (const auto& entry : g_entries) {
            if (entry.isWorld && entry.key.uniqueId >= kWorldStampMin && entry.key.uniqueId <= kWorldStampMax) {
                if (entry.key.uniqueId > maxWorld) {
                    maxWorld = entry.key.uniqueId;
                }
            }
        }
        g_nextWorldStamp = (maxWorld >= kWorldStampMin && maxWorld < kWorldStampMax)
            ? static_cast<std::uint16_t>(maxWorld + 1)
            : kWorldStampMin;
    }

    void GemInventory::Clear()
    {
        std::scoped_lock lock(g_mutex);
        g_entries.clear();
        g_nextWorldStamp = kWorldStampMin;
    }

    std::size_t GemInventory::CountTracked()
    {
        std::scoped_lock lock(g_mutex);
        return g_entries.size();
    }
}
