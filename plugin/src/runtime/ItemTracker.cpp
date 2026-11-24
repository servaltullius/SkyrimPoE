#include "PCH.h"
#include "runtime/ItemTracker.h"
#include "runtime/ModState.h"
#include "runtime/SaveLoad.h"
#include "runtime/UniqueIdUtil.h"
#include "core/WorldStampLink.h"

#include <mutex>
#include <string>
#include <unordered_set>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        namespace WorldStampLink = Core::WorldStampLink;

        constexpr std::uint16_t kInventoryUidMin = UniqueIdUtil::kInventoryUidMin;
        constexpr std::uint16_t kInventoryUidMax = UniqueIdUtil::kInventoryUidMax;
        constexpr std::uint16_t kWorldStampMin = UniqueIdUtil::kItemWorldStampMin;
        constexpr std::uint16_t kWorldStampMax = UniqueIdUtil::kItemWorldStampMax;
        constexpr WorldStampLink::StampBand kItemStampBand{ kWorldStampMin, kWorldStampMax };

        [[nodiscard]] std::string FormKindId(RE::FormID baseFormId)
        {
            return (baseFormId != 0) ? std::to_string(baseFormId) : std::string{};
        }

        // 리프 뮤텍스(잠금 계층 최하위: 내부에서 엔진 함수 호출 절대 금지)
        std::mutex g_mutex;
        std::vector<TrackedItemEntry> g_entries;
        std::uint16_t g_nextWorldStamp = kWorldStampMin;

        // AddObjectToContainer 중 엔진이 동기 TESUniqueIDChangeEvent로 재할당하는 최종 UID 포착 버퍼
        struct PendingAddCapture
        {
            bool active = false;
            RE::FormID ownerFormId = 0;
            RE::FormID baseFormId = 0;
            std::uint16_t seedUid = 0;
            std::uint16_t resolvedUid = 0;
        };
        PendingAddCapture g_pendingAdd;

        Core::CommandQueue* g_queue = nullptr;
        std::function<void()> g_onEquipChanged;
        bool g_installed = false;

        [[nodiscard]] std::uint16_t AllocateWorldStampLocked()
        {
            std::unordered_set<std::uint16_t> used;
            for (const auto& e : g_entries) {
                if (e.isWorld && kItemStampBand.Contains(e.key.uniqueId)) {
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
            SKSE::log::error("ItemTracker: world stamp range [0x{:04X}..0x{:04X}] exhausted; refusing duplicate stamp.",
                kWorldStampMin, kWorldStampMax);
            return 0;
        }

        // 결정 123 + C-33/C-34: 버릴 때 바닥 참조에 ExtraUniqueID(groundRefId, stampUid)를 즉시 기록(뮤텍스 밖에서 호출)
        // priorKey: 버린 이벤트의 직전 키. 바닥 참조에 이미 붙은 번호가 이 키와 같을 때만 바꿔 쓴다(Codex 교차 검토 지적 1).
        bool TryStampDroppedWorldRef(RE::FormID refFormId, RE::FormID expectedBaseFormId, std::uint16_t stampUid,
            Core::InventoryItemKey priorKey)
        {
            if (refFormId == 0 || !kItemStampBand.Contains(stampUid)) {
                return false;
            }
            {
                std::scoped_lock lock(g_mutex);
                bool stillExpected = false;
                for (const auto& e : g_entries) {
                    if (WorldStampLink::MatchesWorldEntry(e.key, e.isWorld, refFormId, stampUid, kItemStampBand) &&
                        (expectedBaseFormId == 0 || e.record.baseFormId == expectedBaseFormId)) {
                        stillExpected = true;
                        break;
                    }
                }
                if (!stillExpected) {
                    return true;
                }
            }

            auto* form = RE::TESForm::LookupByID(refFormId);
            auto* refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!refr || refr->IsDeleted() || refr->IsDisabled()) {
                return false;
            }
            const auto* base = refr->GetBaseObject();
            if (!base || (expectedBaseFormId != 0 && base->GetFormID() != expectedBaseFormId)) {
                return true;
            }

            if (auto* existing = refr->extraList.GetByType<RE::ExtraUniqueID>()) {
                if (existing->uniqueID != 0 && !(existing->baseID == refFormId && existing->uniqueID == stampUid)) {
                    // 다시 버린 물건의 예전 스탬프가 바닥 참조에 남는 경우가 있다(젬 쪽 자동 시험 실측, 2026-11-21).
                    // 이번 이벤트의 직전 키와 같거나, 같은 참조에 붙은 아이템 스탬프 대역 번호이면서 장부의 다른 항목이
                    // 쓰지 않을 때만 바꿔 쓰고, 그 밖의 번호는 거부한다.
                    const Core::InventoryItemKey existingKey{ existing->baseID, existing->uniqueID };
                    const Core::InventoryItemKey selfKey{ refFormId, stampUid };
                    bool ownStale = priorKey.IsValid() && existingKey == priorKey;
                    if (!ownStale && existing->baseID == refFormId && kItemStampBand.Contains(existing->uniqueID)) {
                        std::scoped_lock lock(g_mutex);
                        ownStale = std::none_of(g_entries.begin(), g_entries.end(), [&](const auto& e) {
                            return e.key == existingKey && e.key != selfKey;
                        });
                    }
                    if (!ownStale) {
                        SKSE::log::warn("ItemTracker: world ref {:08X} already carries foreign key {:08X}:{:04X}; refusing overwrite with {:04X}.",
                            refFormId, existing->baseID, existing->uniqueID, stampUid);
                        return true;
                    }
                    SKSE::log::info("ItemTracker: replacing this item's previous stamp {:08X}:{:04X} on world ref {:08X} with {:04X}.",
                        existing->baseID, existing->uniqueID, refFormId, stampUid);
                }
                existing->baseID = refFormId;
                existing->uniqueID = stampUid;
            } else {
                refr->extraList.Add(new RE::ExtraUniqueID(refFormId, stampUid));
            }
            return true;
        }

        // 주 갱신 스레드 전용(계약: g_txMutex 미보유 상태에서만 호출, 결정 59·123·C-34):
        // SaveLoad::RunSerializedTransaction 안에서 (바닥 참조, 스탬프) 묶음을 검증·재바인딩한다.
        std::size_t RelinkPickedUpItemsIn(
            RE::TESObjectREFR* owner,
            std::uint16_t onlyStamp,
            RE::FormID onlyBaseFormId,
            bool postLoadOwnerNormalized = false)
        {
            if (!owner) {
                return 0;
            }
            const RE::FormID ownerId = owner->GetFormID();
            if (ownerId == 0) {
                return 0;
            }

            {
                std::scoped_lock lock(g_mutex);
                bool hasCandidate = false;
                for (const auto& e : g_entries) {
                    if (e.isWorld && e.key.IsValid() && kItemStampBand.Contains(e.key.uniqueId) &&
                        (onlyStamp == 0 || e.key.uniqueId == onlyStamp) &&
                        (onlyBaseFormId == 0 || e.record.baseFormId == onlyBaseFormId)) {
                        hasCandidate = true;
                        break;
                    }
                }
                if (!hasCandidate) {
                    return 0;
                }
            }

            std::size_t linked = 0;
            SaveLoad::RunSerializedTransaction([&]() {
                std::vector<WorldStampLink::WorldLinkCandidate> candidates;
                std::vector<Core::InventoryItemKey> heldKeys;  // 재검수 R-4: 이 소유자의 인벤토리 항목이 이미 쓰는 키
                {
                    std::scoped_lock lock(g_mutex);
                    for (std::size_t i = 0; i < g_entries.size(); ++i) {
                        const auto& e = g_entries[i];
                        if (e.isWorld && e.key.IsValid() && kItemStampBand.Contains(e.key.uniqueId) &&
                            (onlyStamp == 0 || e.key.uniqueId == onlyStamp) &&
                            (onlyBaseFormId == 0 || e.record.baseFormId == onlyBaseFormId)) {
                            candidates.push_back(WorldStampLink::WorldLinkCandidate{
                                static_cast<std::uint64_t>(e.record.baseFormId),
                                e.key,
                                FormKindId(e.record.baseFormId)
                            });
                        } else if (!e.isWorld && e.key.ownerFormId == ownerId) {
                            heldKeys.push_back(e.key);
                        }
                    }
                }
                if (candidates.empty()) {
                    return;
                }

                auto* changes = owner->GetInventoryChanges(true);
                if (!changes || !changes->entryList) {
                    return;
                }

                std::vector<RE::ExtraDataList*> extraPtrs;
                std::vector<WorldStampLink::ObservedInventoryExtra> observed;
                for (auto* entry : *changes->entryList) {
                    if (!entry || !entry->object || !entry->extraLists) {
                        continue;
                    }
                    const RE::FormID objId = entry->object->GetFormID();
                    if (onlyBaseFormId != 0 && objId != onlyBaseFormId) {
                        continue;
                    }
                    const std::string kindStr = FormKindId(objId);
                    if (kindStr.empty()) {
                        continue;
                    }
                    for (auto* extra : *entry->extraLists) {
                        if (!extra) continue;
                        const auto* unique = extra->GetByType<RE::ExtraUniqueID>();
                        if (!unique) continue;
                        const std::size_t idx = extraPtrs.size();
                        extraPtrs.push_back(extra);
                        observed.push_back(WorldStampLink::ObservedInventoryExtra{
                            idx,
                            Core::InventoryItemKey{ unique->baseID, unique->uniqueID },
                            kindStr,
                            extra->GetCount()
                        });
                    }
                }

                const auto plan = WorldStampLink::PlanRelinks(
                    candidates, observed, kItemStampBand, postLoadOwnerNormalized ? ownerId : 0u, heldKeys);
                for (const std::size_t idx : plan.heldCountGtOne) {
                    const auto& obs = observed[idx];
                    SKSE::log::warn("ItemTracker: holding relink for base {} {:08X}:{:04X} because extra count is {} (> 1).",
                        obs.kindId, obs.extraKey.ownerFormId, obs.extraKey.uniqueId, obs.count);
                }
                for (const std::size_t idx : plan.heldAmbiguous) {
                    const auto& obs = observed[idx];
                    SKSE::log::warn("ItemTracker: holding relink for base {} {:08X}:{:04X} due to duplicate/ambiguous matches.",
                        obs.kindId, obs.extraKey.ownerFormId, obs.extraKey.uniqueId);
                }

                // 재검수 R-5: 적용 순서는 WorldStampLink::ExecuteRelinks 한곳에 두고, 여기서는 엔진 묶음과 아이템 장부 접근만 제공한다.
                // 아이템 후보의 instanceId 칸에는 베이스 FormID를 넣었다(위 candidates 참고).
                class ItemRelinkExecutor final : public WorldStampLink::IRelinkExecutor
                {
                public:
                    ItemRelinkExecutor(RE::TESObjectREFR* a_owner, std::vector<RE::ExtraDataList*>& a_extras) :
                        owner_(a_owner), extras_(a_extras)
                    {}

                    std::optional<Core::InventoryItemKey> ReadExtraKey(std::size_t i) override
                    {
                        auto* extra = (i < extras_.size()) ? extras_[i] : nullptr;
                        const auto* unique = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                        if (!unique) {
                            return std::nullopt;
                        }
                        return Core::InventoryItemKey{ unique->baseID, unique->uniqueID };
                    }
                    std::int32_t ExtraCount(std::size_t i) override
                    {
                        auto* extra = (i < extras_.size()) ? extras_[i] : nullptr;
                        return extra ? extra->GetCount() : 0;
                    }
                    std::uint16_t AllocateInventoryUid() override { return UniqueIdUtil::AllocateInventoryUniqueId(owner_); }
                    bool CommitLedger(const WorldStampLink::PlannedRelink& p, const Core::InventoryItemKey& newKey) override
                    {
                        const auto expectedBase = static_cast<RE::FormID>(p.instanceId);
                        std::scoped_lock lock(g_mutex);
                        for (auto& e : g_entries) {
                            if (e.record.baseFormId == expectedBase &&
                                WorldStampLink::CommitRelink(e.key, e.isWorld, p.worldKey, newKey)) {
                                return true;
                            }
                        }
                        return false;
                    }
                    void RevertLedger(const WorldStampLink::PlannedRelink& p, const Core::InventoryItemKey& newKey) override
                    {
                        const auto expectedBase = static_cast<RE::FormID>(p.instanceId);
                        std::scoped_lock lock(g_mutex);
                        for (auto& e : g_entries) {
                            if (!e.isWorld && e.key == newKey && e.record.baseFormId == expectedBase) {
                                e.key = p.worldKey;
                                e.isWorld = true;
                                break;
                            }
                        }
                    }
                    bool WriteExtraKey(std::size_t i, const Core::InventoryItemKey& expectedKey,
                        const Core::InventoryItemKey& newKey) override
                    {
                        auto* extra = (i < extras_.size()) ? extras_[i] : nullptr;
                        auto* unique = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                        if (!unique || unique->baseID != expectedKey.ownerFormId || unique->uniqueID != expectedKey.uniqueId) {
                            return false;
                        }
                        unique->baseID = newKey.ownerFormId;
                        unique->uniqueID = newKey.uniqueId;
                        return true;
                    }
                    void OnAllocateFailed(const WorldStampLink::PlannedRelink& p) override
                    {
                        SKSE::log::error("ItemTracker: unable to allocate inventory UID while relinking item {:08X}:{:04X}.",
                            p.worldKey.ownerFormId, p.worldKey.uniqueId);
                    }
                    void OnExtraChangedBeforeRewrite(const WorldStampLink::PlannedRelink& p) override
                    {
                        SKSE::log::warn("ItemTracker: extra key changed before rewrite for base {:08X}; reverted ledger key.",
                            static_cast<RE::FormID>(p.instanceId));
                    }
                    void OnLinked(const WorldStampLink::PlannedRelink& p, const Core::InventoryItemKey& newKey) override
                    {
                        // 재검수 R-4: 정확 대조인지 로드 직후 소유자 정규화 대조인지 남긴다.
                        SKSE::log::info("ItemTracker: relinked picked-up item {:08X}:{:04X} -> {:08X}:{:04X} (match={}, observed {:08X}:{:04X}).",
                            p.worldKey.ownerFormId, p.worldKey.uniqueId, newKey.ownerFormId, newKey.uniqueId,
                            p.observedExtraKey == p.worldKey ? "exact" : "owner-normalized",
                            p.observedExtraKey.ownerFormId, p.observedExtraKey.uniqueId);
                    }

                private:
                    RE::TESObjectREFR* owner_;
                    std::vector<RE::ExtraDataList*>& extras_;
                };
                ItemRelinkExecutor exec(owner, extraPtrs);
                linked = WorldStampLink::ExecuteRelinks(plan, ownerId, exec);
            });

            if (linked > 0) {
                if (auto* player = RE::PlayerCharacter::GetSingleton(); player && ownerId == player->GetFormID()) {
                    if (g_onEquipChanged) {
                        g_onEquipChanged();
                    }
                }
            }
            return linked;
        }

        // 주 갱신 스레드: 바닥에서 주운 스탬프 아이템을 newOwner의 충돌 없는 인벤토리 UID로 재바인딩(결정 123)
        void RebindPickedUpItemOnMainUpdate(RE::FormID newOwnerFormId, RE::FormID baseFormId, std::uint16_t stampUid)
        {
            auto* ownerForm = RE::TESForm::LookupByID(newOwnerFormId);
            auto* newOwner = ownerForm ? ownerForm->As<RE::TESObjectREFR>() : nullptr;
            if (newOwner) {
                (void)RelinkPickedUpItemsIn(newOwner, stampUid, baseFormId);
            }
        }

        class ItemUniqueIdSink final : public RE::BSTEventSink<RE::TESUniqueIDChangeEvent>
        {
        public:
            static ItemUniqueIdSink& Get()
            {
                static ItemUniqueIdSink inst;
                return inst;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESUniqueIDChangeEvent* event,
                RE::BSTEventSource<RE::TESUniqueIDChangeEvent>*) override
            {
                const auto generation = g_queue ? g_queue->CurrentGeneration(Core::CommandQueue::Kind::Tracking) : 0;
                if (!ModState::AllowsGameplay() || !event || event->oldBaseID == 0 || event->oldUniqueID == 0) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                // 검수 Minor 6: 비정상 (newBaseID == 0 && newUniqueID != 0) 이벤트 무시(GemInventory와 동일 가드)
                if (event->newBaseID == 0 && event->newUniqueID != 0) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                RE::FormID droppedRefId = 0;
                RE::FormID droppedBaseId = 0;
                std::uint16_t stampUid = 0;
                bool touchedPlayer = false;
                const Core::InventoryItemKey priorKey{ event->oldBaseID, event->oldUniqueID };
                auto* player = RE::PlayerCharacter::GetSingleton();
                const RE::FormID playerId = player ? player->GetFormID() : 0;

                {
                    std::scoped_lock lock(g_mutex);
                    if (g_queue && generation != g_queue->CurrentGeneration(Core::CommandQueue::Kind::Tracking)) {
                        return RE::BSEventNotifyControl::kContinue;
                    }
                    // AddObjectWithTrackedUniqueId 호출 도중 엔진이 시드 UID를 최종 인벤토리 UID로 재할당한 경우 포착
                    if (g_pendingAdd.active &&
                        event->oldBaseID == g_pendingAdd.ownerFormId &&
                        event->oldUniqueID == g_pendingAdd.seedUid &&
                        event->newBaseID == g_pendingAdd.ownerFormId &&
                        event->newUniqueID != 0 &&
                        (event->objectID == 0 || event->objectID == g_pendingAdd.baseFormId)) {
                        g_pendingAdd.resolvedUid = event->newUniqueID;
                    }
                    for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
                        if (it->key.ownerFormId == event->oldBaseID &&
                            it->key.uniqueId == event->oldUniqueID &&
                            (event->objectID == 0 || it->record.baseFormId == event->objectID)) {
                            if (event->oldBaseID == playerId || event->newBaseID == playerId) {
                                touchedPlayer = true;
                            }
                            // 검수 C2: 소멸/분해/재료 소모 시 좀비 엔트리 즉시 삭제
                            if (event->newBaseID == 0 && event->newUniqueID == 0) {
                                g_entries.erase(it);
                                break;
                            }

                            if (WorldStampLink::NeedsWorldStamp(event->newBaseID, event->newUniqueID)) {
                                // C-33: 직전 키가 바닥 키였어도(다시 잇기 전 재드랍) 새 바닥 참조에 스탬프를 남긴다.
                                stampUid = AllocateWorldStampLocked();
                                it->key.ownerFormId = event->newBaseID;
                                it->key.uniqueId = stampUid;
                                it->isWorld = true;
                                if (stampUid != 0) {
                                    droppedRefId = event->newBaseID;
                                    droppedBaseId = it->record.baseFormId;
                                }
                            } else {
                                it->key.ownerFormId = event->newBaseID;
                                it->key.uniqueId = event->newUniqueID;
                                it->isWorld = false;
                            }
                            break;
                        }
                    }
                }

                // 결정 123: 버릴 때 바닥 스탬프는 이벤트 안에서 즉시 기록, 미조회 시 주 갱신 1회 보완
                if (droppedRefId != 0 && stampUid != 0) {
                    if (!TryStampDroppedWorldRef(droppedRefId, droppedBaseId, stampUid, priorKey) && g_queue) {
                        g_queue->EnqueueForGeneration(generation, Core::CommandQueue::Kind::Tracking, [droppedRefId, droppedBaseId, stampUid, priorKey]() {
                            (void)TryStampDroppedWorldRef(droppedRefId, droppedBaseId, stampUid, priorKey);
                        });
                    }
                }

                if (touchedPlayer && g_onEquipChanged && g_queue) {
                    g_queue->EnqueueForGeneration(generation, Core::CommandQueue::Kind::Tracking, []() {
                        if (g_onEquipChanged) {
                            g_onEquipChanged();
                        }
                    });
                }

                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            ItemUniqueIdSink() = default;
        };

        class ItemContainerChangedSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
        {
        public:
            static ItemContainerChangedSink& Get()
            {
                static ItemContainerChangedSink inst;
                return inst;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESContainerChangedEvent* event,
                RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
            {
                const auto generation = g_queue ? g_queue->CurrentGeneration(Core::CommandQueue::Kind::Tracking) : 0;
                if (!ModState::AllowsGameplay() || !event || !g_queue) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                // 결정 123: 바닥에서 주움(oldContainer==0, newContainer!=0, itemCount>0, 우리 바닥 스탬프 대역)
                if (WorldStampLink::IsStampedWorldPickup(
                        event->oldContainer, event->newContainer, event->uniqueID, event->itemCount, kItemStampBand)) {
                    const std::string eventKind = FormKindId(event->baseObj);
                    bool isTracked = false;
                    {
                        std::scoped_lock lock(g_mutex);
                        for (const auto& e : g_entries) {
                            if (WorldStampLink::MatchesPickupEvent(
                                    e.key, e.isWorld, FormKindId(e.record.baseFormId), event->uniqueID, eventKind, kItemStampBand)) {
                                isTracked = true;
                                break;
                            }
                        }
                    }
                    if (isTracked) {
                        const auto newOwner = event->newContainer;
                        const auto baseObj = event->baseObj;
                        const auto stamp = event->uniqueID;
                        g_queue->EnqueueForGeneration(generation, Core::CommandQueue::Kind::Tracking, [newOwner, baseObj, stamp]() {
                            RebindPickedUpItemOnMainUpdate(newOwner, baseObj, stamp);
                        });
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            ItemContainerChangedSink() = default;
        };

        class PlayerEquipSink final : public RE::BSTEventSink<RE::TESEquipEvent>
        {
        public:
            static PlayerEquipSink& Get()
            {
                static PlayerEquipSink inst;
                return inst;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESEquipEvent* event,
                RE::BSTEventSource<RE::TESEquipEvent>*) override
            {
                const auto generation = g_queue ? g_queue->CurrentGeneration(Core::CommandQueue::Kind::Tracking) : 0;
                if (!ModState::AllowsGameplay() || !event || !event->actor || !g_queue || !g_onEquipChanged) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (event->actor->IsPlayerRef()) {
                    g_queue->EnqueueForGeneration(generation, Core::CommandQueue::Kind::Tracking, []() {
                        if (g_onEquipChanged) {
                            g_onEquipChanged();
                        }
                    });
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            PlayerEquipSink() = default;
        };
    }

    bool ItemTracker::Install(Core::CommandQueue* queue, std::function<void()> onPlayerEquipChanged)
    {
        g_queue = queue;
        g_onEquipChanged = std::move(onPlayerEquipChanged);
        if (g_installed) {
            return true;
        }
        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            SKSE::log::error("ItemTracker: ScriptEventSourceHolder unavailable.");
            return false;
        }
        holder->AddEventSink<RE::TESUniqueIDChangeEvent>(&ItemUniqueIdSink::Get());
        holder->AddEventSink<RE::TESContainerChangedEvent>(&ItemContainerChangedSink::Get());
        holder->AddEventSink<RE::TESEquipEvent>(&PlayerEquipSink::Get());

        UniqueIdUtil::RegisterUsedInventoryUidCollector([](std::unordered_set<std::uint16_t>& used) {
            std::scoped_lock lock(g_mutex);
            for (const auto& e : g_entries) {
                if (!e.isWorld && e.key.uniqueId >= kInventoryUidMin && e.key.uniqueId <= kInventoryUidMax) {
                    used.insert(e.key.uniqueId);
                }
            }
        });
        UniqueIdUtil::RegisterTrackedKeyChecker([](RE::FormID ownerId, std::uint16_t uid) {
            std::scoped_lock lock(g_mutex);
            for (const auto& e : g_entries) {
                if (e.key.ownerFormId == ownerId && e.key.uniqueId == uid) {
                    return true;
                }
            }
            return false;
        });

        g_installed = true;
        SKSE::log::info("ItemTracker: TESUniqueIDChangeEvent + TESContainerChangedEvent + TESEquipEvent sinks registered.");
        return true;
    }

    Core::InventoryItemKey ItemTracker::EnsureUniqueId(
        RE::TESObjectREFR* owner,
        RE::TESBoundObject* baseObj,
        RE::ExtraDataList* extraList,
        RE::ExtraDataList** outFinalExtra)
    {
        if (outFinalExtra) {
            *outFinalExtra = nullptr;
        }
        if (!owner || !baseObj) {
            return {};
        }
        const RE::FormID ownerId = owner->GetFormID();
        if (ownerId == 0) {
            return {};
        }

        // 1. 이미 단독 묶음(count == 1)이고 ExtraUniqueID가 있으면 그대로 재사용
        if (extraList) {
            const std::int32_t cnt = extraList->GetCount();
            if (cnt <= 1) {
                if (const auto* existing = extraList->GetByType<RE::ExtraUniqueID>()) {
                    if (existing->baseID == ownerId && existing->uniqueID != 0) {
                        if (outFinalExtra) *outFinalExtra = extraList;
                        return Core::InventoryItemKey{ ownerId, existing->uniqueID };
                    }
                }
                const std::uint16_t uid = UniqueIdUtil::AllocateInventoryUniqueId(owner);
                if (uid == 0) {
                    return {};
                }
                if (auto* existing = extraList->GetByType<RE::ExtraUniqueID>()) {
                    existing->baseID = ownerId;
                    existing->uniqueID = uid;
                } else {
                    extraList->Add(new RE::ExtraUniqueID(ownerId, uid));
                }
                if (outFinalExtra) *outFinalExtra = extraList;
                return Core::InventoryItemKey{ ownerId, uid };
            }
        }

        // 2. 검수 C2, M5, C-8: extraList == nullptr 또는 count > 1 일반 스택은 정확히 1개만 분리해 신규 ExtraDataList + UID 부여
        // 단련·마법부여·이름·충전량·독·소유권이 있으면 1개 분리에 보존한다.
        // 재검수 R-2: 아이템을 빼기 전에 번호를 받을 수 있는지 먼저 확인한다. 뺀 뒤에 할당이 실패하면 그 1개가 사라진다.
        // (할당기는 사용 중인 번호를 훑어 빈 번호를 고를 뿐 예약하지 않으므로, 같은 스레드에서 곧바로 다시 부르면 같은 결과다.)
        if (UniqueIdUtil::AllocateInventoryUniqueId(owner) == 0) {
            SKSE::log::error("ItemTracker::EnsureUniqueId: no free inventory UID for owner {:08X}; item left untouched.", ownerId);
            return {};
        }
        auto* freshExtra = UniqueIdUtil::CreateGameExtraDataList();
        if (!freshExtra) {
            SKSE::log::error("ItemTracker::EnsureUniqueId: CreateGameExtraDataList failed.");
            return {};
        }
        if (extraList) {
            if (const auto* health = extraList->GetByType<RE::ExtraHealth>()) {
                freshExtra->Add(new RE::ExtraHealth(health->health));
            }
            if (const auto* ench = extraList->GetByType<RE::ExtraEnchantment>()) {
                freshExtra->Add(new RE::ExtraEnchantment(ench->enchantment, ench->charge, ench->removeOnUnequip));
            }
            if (const auto* ch = extraList->GetByType<RE::ExtraCharge>()) {
                auto* copyCharge = new RE::ExtraCharge();
                copyCharge->charge = ch->charge;
                freshExtra->Add(copyCharge);
            }
            if (const auto* pois = extraList->GetByType<RE::ExtraPoison>(); pois && pois->poison) {
                freshExtra->Add(new RE::ExtraPoison(pois->poison, static_cast<std::int32_t>(pois->count)));
            }
            if (const auto* own = extraList->GetByType<RE::ExtraOwnership>(); own && own->owner) {
                freshExtra->Add(new RE::ExtraOwnership(own->owner));
            }
            if (const auto* text = extraList->GetByType<RE::ExtraTextDisplayData>()) {
                if (!text->displayName.empty()) {
                    freshExtra->Add(new RE::ExtraTextDisplayData(text->displayName.c_str()));
                }
            }
        }
        const auto countOfBase = [owner, baseObj]() -> std::int32_t {
            const auto counts = owner->GetInventoryCounts([baseObj](RE::TESBoundObject& a_obj) { return &a_obj == baseObj; }, true);
            const auto it = counts.find(baseObj);
            return it != counts.end() ? it->second : 0;
        };
        const std::int32_t countBeforeSplit = countOfBase();
        if (extraList) {
            owner->RemoveItem(baseObj, 1, RE::ITEM_REMOVE_REASON::kRemove, extraList, nullptr);
        } else {
            // Q-6: extraList == nullptr일 때 RemoveItem(nullptr)은 기존 추적 ExtraDataList를 먼저 소비하므로 일반 스택만 안전 제거
            if (!UniqueIdUtil::RemovePlainItem(owner, baseObj, 1)) {
                UniqueIdUtil::DestroyGameExtraDataList(freshExtra);
                return {};
            }
        }
        // Codex 지적 6: 실제로 1개가 빠졌을 때만 다시 넣는다. 빠지지 않았는데 넣으면 아이템이 하나 늘어난다.
        if (countOfBase() >= countBeforeSplit) {
            SKSE::log::error("ItemTracker::EnsureUniqueId: split removal did not reduce base {:08X} count; aborting without re-add.",
                baseObj->GetFormID());
            UniqueIdUtil::DestroyGameExtraDataList(freshExtra);
            return {};
        }
        bool untrackedLeft = false;
        const auto key = AddObjectWithTrackedUniqueId(
            owner, baseObj, freshExtra, outFinalExtra, UntrackedAddPolicy::Keep, &untrackedLeft);
        if (!key.IsValid() && !untrackedLeft) {
            // 번호 할당 실패로 다시 넣지 못했다(위의 사전 확인 때문에 정상적으로는 오지 않는 경로).
            // 앞에서 뺀 1개를 일반품으로라도 돌려줘 아이템 자체는 잃지 않게 한다.
            owner->AddObjectToContainer(baseObj, nullptr, 1, nullptr);
            SKSE::log::error("ItemTracker::EnsureUniqueId: could not re-add split item for base {:08X}; restored it as a plain item.",
                baseObj->GetFormID());
        }
        return key;
    }

    Core::InventoryItemKey ItemTracker::AddObjectWithTrackedUniqueId(
        RE::TESObjectREFR* owner,
        RE::TESBoundObject* baseObj,
        RE::ExtraDataList* freshExtra,
        RE::ExtraDataList** outFinalExtra,
        UntrackedAddPolicy policy,
        bool* outUntrackedItemLeft)
    {
        if (outFinalExtra) {
            *outFinalExtra = nullptr;
        }
        if (outUntrackedItemLeft) {
            *outUntrackedItemLeft = false;
        }
        if (!owner || !baseObj || !freshExtra) {
            return {};
        }
        const RE::FormID ownerId = owner->GetFormID();
        if (ownerId == 0) {
            return {};
        }

        // 안전망: AddObjectToContainer 직전 해당 베이스 오브젝트의 기존 UID 집합·묶음·수량 스냅샷
        std::unordered_set<std::uint16_t> preExistingUids;
        std::unordered_set<const RE::ExtraDataList*> preExistingExtras;
        if (auto* changes = owner->GetInventoryChanges(true); changes && changes->entryList) {
            for (auto* entry : *changes->entryList) {
                if (!entry || entry->object != baseObj || !entry->extraLists) {
                    continue;
                }
                for (auto* ex : *entry->extraLists) {
                    if (!ex) continue;
                    preExistingExtras.insert(ex);
                    if (const auto* u = ex->GetByType<RE::ExtraUniqueID>(); u && u->baseID == ownerId && u->uniqueID != 0) {
                        preExistingUids.insert(u->uniqueID);
                    }
                }
            }
        }
        const auto countOfBase = [owner, baseObj]() -> std::int32_t {
            // a_noInit=true: 위에서 GetInventoryChanges(true)로 이미 만든 인벤토리를 다시 초기화하지 않는다(사전 스냅숏이 어긋나지 않게).
            const auto counts = owner->GetInventoryCounts([baseObj](RE::TESBoundObject& a_obj) { return &a_obj == baseObj; }, true);
            const auto it = counts.find(baseObj);
            return it != counts.end() ? it->second : 0;
        };
        const std::int32_t preCount = countOfBase();

        const std::uint16_t seedUid = UniqueIdUtil::AllocateInventoryUniqueId(owner);
        if (seedUid == 0) {
            UniqueIdUtil::DestroyGameExtraDataList(freshExtra);
            return {};
        }
        if (auto* existing = freshExtra->GetByType<RE::ExtraUniqueID>()) {
            existing->baseID = ownerId;
            existing->uniqueID = seedUid;
        } else {
            freshExtra->Add(new RE::ExtraUniqueID(ownerId, seedUid));
        }

        {
            std::scoped_lock lock(g_mutex);
            g_pendingAdd.active = true;
            g_pendingAdd.ownerFormId = ownerId;
            g_pendingAdd.baseFormId = baseObj->GetFormID();
            g_pendingAdd.seedUid = seedUid;
            g_pendingAdd.resolvedUid = 0;
        }

        owner->AddObjectToContainer(baseObj, freshExtra, 1, nullptr);

        std::uint16_t capturedUid = 0;
        {
            std::scoped_lock lock(g_mutex);
            capturedUid = g_pendingAdd.resolvedUid;
            g_pendingAdd = PendingAddCapture{};
        }

        std::uint16_t finalUid = (capturedUid != 0) ? capturedUid : seedUid;
        RE::ExtraDataList* finalExtra = FindExtraListByKey(owner, baseObj, Core::InventoryItemKey{ ownerId, finalUid });

        // 엔진이 다른 경로로 UID를 재발급해 이벤트가 누락된 경우 신규 추가된 ExtraUniqueID 차집합으로 즉시 확정
        if (!finalExtra) {
            if (auto* changes = owner->GetInventoryChanges(true); changes && changes->entryList) {
                for (auto* entry : *changes->entryList) {
                    if (!entry || entry->object != baseObj || !entry->extraLists) {
                        continue;
                    }
                    for (auto* ex : *entry->extraLists) {
                        if (!ex) continue;
                        if (const auto* u = ex->GetByType<RE::ExtraUniqueID>();
                            u && u->baseID == ownerId && u->uniqueID != 0 && !preExistingUids.contains(u->uniqueID)) {
                            finalUid = u->uniqueID;
                            finalExtra = ex;
                            break;
                        }
                    }
                    if (finalExtra) break;
                }
            }
        }

        // C-5: 실제 인벤토리에서 ExtraDataList를 확인하지 못하면 가짜 유효 키를 반환하지 않고 무효 키를 반환한다.
        if (!finalExtra || finalUid == 0) {
            // 재검수 R-15: 실제로 들어갔는지는 수량 변화로 판단한다. 들어간 채 무효 키만 돌려주면 호출부의 환불·재시도가 복제를 만든다.
            bool left = countOfBase() > preCount;
            if (left && policy == UntrackedAddPolicy::RemoveAgain) {
                // 방금 생긴 묶음(추가 전에 없던 ExtraDataList)을 골라 뺀다. 없으면 엔진이 일반 스택에 합친 것이므로
                // 일반품 1개를 빼는 것이 방금 넣은 것을 빼는 것과 같다.
                RE::ExtraDataList* addedExtra = nullptr;
                std::size_t newExtraCount = 0;
                if (auto* changes = owner->GetInventoryChanges(true); changes && changes->entryList) {
                    for (auto* entry : *changes->entryList) {
                        if (!entry || entry->object != baseObj || !entry->extraLists) {
                            continue;
                        }
                        for (auto* ex : *entry->extraLists) {
                            if (ex && !preExistingExtras.contains(ex)) {
                                addedExtra = ex;
                                ++newExtraCount;
                            }
                        }
                    }
                }
                const std::int32_t addedCount = countOfBase() - preCount;
                if (addedCount != 1 || newExtraCount > 1) {
                    // 수량이 1이 아닌 만큼 바뀌었거나 새 묶음이 여럿이면 어느 것이 방금 넣은 것인지 특정할 수 없다(Codex 지적 2).
                    // 엉뚱한 물건을 빼지 않도록 그대로 두고, 들어간 것으로 처리한다(호출부는 환불하지 않음 → 복제 없음).
                    left = true;
                } else {
                    if (addedExtra) {
                        owner->RemoveItem(baseObj, 1, RE::ITEM_REMOVE_REASON::kRemove, addedExtra, nullptr);
                    } else {
                        // 새 묶음이 없다 = 엔진이 일반 스택에 합쳤다. 일반품 1개를 빼는 것이 방금 넣은 것을 빼는 것과 같다.
                        (void)UniqueIdUtil::RemovePlainItem(owner, baseObj, 1);
                    }
                    left = countOfBase() > preCount;
                }
            }
            if (outUntrackedItemLeft) {
                *outUntrackedItemLeft = left;
            }
            SKSE::log::error("ItemTracker::AddObjectWithTrackedUniqueId: failed to confirm tracked ExtraUniqueID for base {:08X} in owner {:08X} ({}).",
                baseObj->GetFormID(), ownerId, left ? "item left untracked" : "added item removed again");
            return {};
        }

        if (outFinalExtra) {
            *outFinalExtra = finalExtra;
        }
        return Core::InventoryItemKey{ ownerId, finalUid };
    }

    std::size_t ItemTracker::RelinkPickedUpItems(RE::TESObjectREFR* owner, bool postLoadOwnerNormalized)
    {
        return RelinkPickedUpItemsIn(owner, 0, 0, postLoadOwnerNormalized);
    }

    std::optional<Core::PoEItemRecord> ItemTracker::FindByKey(const Core::InventoryItemKey& key)
    {
        if (!key.IsValid()) {
            return std::nullopt;
        }
        std::scoped_lock lock(g_mutex);
        for (const auto& e : g_entries) {
            if (!e.isWorld && e.key == key) {
                return e.record;
            }
        }
        return std::nullopt;
    }

    void ItemTracker::Upsert(const Core::InventoryItemKey& key, const Core::PoEItemRecord& record, bool isWorld)
    {
        if (!key.IsValid()) {
            return;
        }
        std::scoped_lock lock(g_mutex);
        for (auto& e : g_entries) {
            if (e.key == key && e.isWorld == isWorld) {
                e.record = record;
                return;
            }
        }
        g_entries.push_back(TrackedItemEntry{ key, isWorld, record });
    }

    void ItemTracker::RemoveByKey(const Core::InventoryItemKey& key)
    {
        if (!key.IsValid()) {
            return;
        }
        std::scoped_lock lock(g_mutex);
        std::erase_if(g_entries, [&](const TrackedItemEntry& e) { return e.key == key; });
    }

    std::vector<TrackedItemEntry> ItemTracker::TakeSnapshotForSave()
    {
        std::scoped_lock lock(g_mutex);
        return g_entries;
    }

    void ItemTracker::RestoreFromSave(std::vector<TrackedItemEntry> entries)
    {
        std::scoped_lock lock(g_mutex);
        g_entries = std::move(entries);
        // 검수 m1: 복원된 바닥 스탬프 최댓값 다음으로 링 커서 전진
        std::uint16_t maxStamp = 0;
        for (const auto& e : g_entries) {
            if (e.isWorld && e.key.uniqueId >= kWorldStampMin && e.key.uniqueId <= kWorldStampMax) {
                if (e.key.uniqueId > maxStamp) {
                    maxStamp = e.key.uniqueId;
                }
            }
        }
        if (maxStamp >= kWorldStampMin) {
            g_nextWorldStamp = (maxStamp == kWorldStampMax)
                ? kWorldStampMin
                : static_cast<std::uint16_t>(maxStamp + 1);
        }
    }

    void ItemTracker::Clear()
    {
        std::scoped_lock lock(g_mutex);
        g_entries.clear();
        g_nextWorldStamp = kWorldStampMin;
    }

    std::size_t ItemTracker::CountTracked()
    {
        std::scoped_lock lock(g_mutex);
        return g_entries.size();
    }

    RE::ExtraDataList* ItemTracker::FindExtraListByKey(
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
}
