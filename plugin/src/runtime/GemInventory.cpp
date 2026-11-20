#include "PCH.h"
#include "runtime/GemInventory.h"
#include "runtime/FormIds.h"
#include "runtime/SaveLoad.h"
#include "runtime/UniqueIdUtil.h"
#include "core/WorldStampLink.h"

#include <unordered_map>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        namespace WorldStampLink = Core::WorldStampLink;

        // 검수 C2: UniqueIdUtil 공용 비중복 대역 사용(젬 바닥 스탬프 0x7F00..0xBFFF)
        constexpr std::uint16_t kInventoryUidMin = UniqueIdUtil::kInventoryUidMin;
        constexpr std::uint16_t kInventoryUidMax = UniqueIdUtil::kInventoryUidMax;
        constexpr std::uint16_t kWorldStampMin = UniqueIdUtil::kGemWorldStampMin;
        constexpr std::uint16_t kWorldStampMax = UniqueIdUtil::kGemWorldStampMax;
        constexpr WorldStampLink::StampBand kGemStampBand{ kWorldStampMin, kWorldStampMax };

        std::mutex g_mutex;
        std::vector<TrackedGemEntry> g_entries;
        std::uint16_t g_nextWorldStamp = kWorldStampMin;

        // C-35: Install(kDataLoaded)에서 1회 구축하는 읽기 전용 젬 폼 ID 캐시(싱크·뮤텍스 내 엔진 조회 차단)
        std::unordered_map<std::string, RE::FormID> g_gemIdToFormId;
        std::unordered_map<RE::FormID, std::string_view> g_formIdToGemId;

        Core::CommandQueue* g_queue = nullptr;
        bool g_installed = false;

        void BuildGemFormCache()
        {
            g_gemIdToFormId.clear();
            g_formIdToGemId.clear();
            for (const auto& entry : FormIds::kAllGemForms) {
                if (auto* misc = FormIds::Lookup<RE::TESObjectMISC>(entry.localFormId)) {
                    const RE::FormID fid = misc->GetFormID();
                    if (fid != 0) {
                        g_gemIdToFormId[std::string(entry.gemId)] = fid;
                        g_formIdToGemId[fid] = entry.gemId;
                    }
                }
            }
        }

        [[nodiscard]] RE::FormID CachedFormIdForGem(std::string_view gemId) noexcept
        {
            if (gemId.empty()) {
                return 0;
            }
            if (const auto it = g_gemIdToFormId.find(std::string(gemId)); it != g_gemIdToFormId.end()) {
                return it->second;
            }
            return 0;
        }

        [[nodiscard]] std::string_view GemIdForForm(RE::FormID fullFormId) noexcept
        {
            if (fullFormId == 0) {
                return {};
            }
            if (const auto it = g_formIdToGemId.find(fullFormId); it != g_formIdToGemId.end()) {
                return it->second;
            }
            return {};
        }

        [[nodiscard]] RE::ExtraDataList* CreateGameExtraDataList()
        {
            return UniqueIdUtil::CreateGameExtraDataList();
        }

        // 공용 할당기 위임(엔진 + GemInventory + ItemTracker UID 모두 회피, 검수 C2·C-7: 고갈 시 0 반환)
        [[nodiscard]] std::uint16_t NextFreeInventoryUniqueId(RE::TESObjectREFR* owner)
        {
            return UniqueIdUtil::AllocateInventoryUniqueId(owner);
        }

        // g_mutex가 잡힌 상태에서 호출: 바닥 스탬프 대역([0x7F00, 0xBFFF])에서 미사용 번호를 할당한다(고갈 시 0).
        [[nodiscard]] std::uint16_t AllocateWorldStampLocked()
        {
            std::unordered_set<std::uint16_t> used;
            for (const auto& e : g_entries) {
                if (e.isWorld && kGemStampBand.Contains(e.key.uniqueId)) {
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
            SKSE::log::error("GemInventory: world stamp range [0x{:04X}..0x{:04X}] exhausted; refusing duplicate stamp.",
                kWorldStampMin, kWorldStampMax);
            return 0;
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

        // 결정 123 + C-1 보완 스탬프 재검증(뮤텍스 밖에서 호출):
        // 1) 장부에서 해당 인스턴스가 아직 (refFormId, stampUid) 바닥 상태인지 확인
        // 2) 바닥 참조가 살아 있고 실제 베이스가 expectedGemId와 일치하는지 확인
        // 3) 이미 다른 유효 스탬프를 단 참조는 덮어쓰지 않음(Codex A2·B2)
        bool TryStampDroppedWorldRef(
            RE::FormID refFormId,
            std::uint64_t instanceId,
            std::uint16_t stampUid,
            const std::string& expectedGemId)
        {
            if (refFormId == 0 || !kGemStampBand.Contains(stampUid) || expectedGemId.empty()) {
                return false;
            }
            {
                std::scoped_lock lock(g_mutex);
                bool stillExpected = false;
                for (const auto& entry : g_entries) {
                    if (entry.gem.instanceId == instanceId &&
                        WorldStampLink::MatchesWorldEntry(entry.key, entry.isWorld, refFormId, stampUid, kGemStampBand)) {
                        stillExpected = true;
                        break;
                    }
                }
                if (!stillExpected) {
                    return true;  // 이미 주워졌거나 이동·소멸됨: 보완 예약 불필요
                }
            }

            auto* form = RE::TESForm::LookupByID(refFormId);
            auto* refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!refr || refr->IsDeleted() || refr->IsDisabled()) {
                return false;
            }
            const auto* base = refr->GetBaseObject();
            if (!base || GemIdForForm(base->GetFormID()) != expectedGemId) {
                SKSE::log::warn("GemInventory: world ref {:08X} base mismatch for expected gem '{}'; skipping stamp {:04X}.",
                    refFormId, expectedGemId, stampUid);
                return true;  // 잘못된 참조에 덮어쓰지 않고 종료
            }

            if (auto* existing = refr->extraList.GetByType<RE::ExtraUniqueID>()) {
                if (existing->uniqueID != 0 && !(existing->baseID == refFormId && existing->uniqueID == stampUid)) {
                    SKSE::log::warn("GemInventory: world ref {:08X} already carries stamp {:08X}:{:04X}; refusing overwrite with {:04X}.",
                        refFormId, existing->baseID, existing->uniqueID, stampUid);
                    return true;
                }
                existing->baseID = refFormId;
                existing->uniqueID = stampUid;
            } else {
                refr->extraList.Add(new RE::ExtraUniqueID(refFormId, stampUid));
            }
            return true;
        }

        // 주 갱신 스레드 전용(계약: g_txMutex 미보유 상태에서만 호출, 결정 59·123·C-1):
        // owner 인벤토리에서 (바닥 참조, 스탬프) 묶음이 정확히 일치하고 젬 종류도 같은 추적 젬을 찾아
        // SaveLoad::RunSerializedTransaction 안에서 owner의 새 인벤토리 번호로 다시 잇는다.
        // 잠금 순서: g_txMutex → 후보 수집(g_mutex) → 묶음 탐색·번호 할당(g_mutex 밖) → 키 커밋(g_mutex) → 묶음 고쳐 쓰기(g_mutex 밖).
        std::size_t RelinkPickedUpGemsIn(
            RE::TESObjectREFR* owner,
            std::uint16_t onlyStamp,
            bool postLoadOwnerNormalized = false)
        {
            if (!owner) {
                return 0;
            }
            const RE::FormID ownerId = owner->GetFormID();
            if (ownerId == 0) {
                return 0;
            }

            // 빠른 탈출: 대상 바닥 후보가 전혀 없으면 g_txMutex 및 인벤토리 스캔을 생략한다.
            {
                std::scoped_lock lock(g_mutex);
                bool hasCandidate = false;
                for (const auto& e : g_entries) {
                    if (e.isWorld && e.key.IsValid() && kGemStampBand.Contains(e.key.uniqueId) &&
                        (onlyStamp == 0 || e.key.uniqueId == onlyStamp)) {
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
                {
                    std::scoped_lock lock(g_mutex);
                    for (const auto& e : g_entries) {
                        if (e.isWorld && e.key.IsValid() && kGemStampBand.Contains(e.key.uniqueId) &&
                            (onlyStamp == 0 || e.key.uniqueId == onlyStamp)) {
                            candidates.push_back(WorldStampLink::WorldLinkCandidate{ e.gem.instanceId, e.key, e.gem.gemId });
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
                    const auto gemId = GemIdForForm(entry->object->GetFormID());
                    if (gemId.empty()) {
                        continue;
                    }
                    for (auto* extra : *entry->extraLists) {
                        if (!extra) {
                            continue;
                        }
                        const auto* unique = extra->GetByType<RE::ExtraUniqueID>();
                        if (!unique) {
                            continue;
                        }
                        const std::size_t idx = extraPtrs.size();
                        extraPtrs.push_back(extra);
                        observed.push_back(WorldStampLink::ObservedInventoryExtra{
                            idx,
                            Core::InventoryItemKey{ unique->baseID, unique->uniqueID },
                            std::string(gemId),
                            extra->GetCount()
                        });
                    }
                }

                const auto plan = WorldStampLink::PlanRelinks(
                    candidates, observed, kGemStampBand, postLoadOwnerNormalized ? ownerId : 0u);
                for (const std::size_t idx : plan.heldCountGtOne) {
                    const auto& obs = observed[idx];
                    SKSE::log::warn("GemInventory: holding relink for gem '{}' {:08X}:{:04X} because extra count is {} (> 1).",
                        obs.kindId, obs.extraKey.ownerFormId, obs.extraKey.uniqueId, obs.count);
                }
                for (const std::size_t idx : plan.heldAmbiguous) {
                    const auto& obs = observed[idx];
                    SKSE::log::warn("GemInventory: holding relink for gem '{}' {:08X}:{:04X} due to duplicate/ambiguous matches.",
                        obs.kindId, obs.extraKey.ownerFormId, obs.extraKey.uniqueId);
                }

                for (const auto& p : plan.planned) {
                    RE::ExtraDataList* targetExtra = extraPtrs[p.extraIndex];
                    if (!targetExtra || targetExtra->GetCount() > 1) {
                        continue;
                    }
                    // 실물 재확인(커밋 직전): 묶음이 아직 관측된 키(일반 런타임=바닥 참조:스탬프, 로드 직후=정규화 소유자:스탬프)를 달고 있는지 검증
                    const auto* preCheck = targetExtra->GetByType<RE::ExtraUniqueID>();
                    if (!preCheck || preCheck->baseID != p.observedExtraKey.ownerFormId || preCheck->uniqueID != p.observedExtraKey.uniqueId) {
                        continue;
                    }

                    const std::uint16_t newUid = NextFreeInventoryUniqueId(owner);
                    if (newUid == 0) {
                        SKSE::log::error("GemInventory: unable to allocate inventory UID while relinking gem instance {}.",
                            p.instanceId);
                        continue;
                    }
                    const Core::InventoryItemKey newKey{ ownerId, newUid };

                    bool committed = false;
                    {
                        std::scoped_lock lock(g_mutex);
                        for (auto& e : g_entries) {
                            if (e.gem.instanceId == p.instanceId) {
                                committed = WorldStampLink::CommitRelink(e.key, e.isWorld, p.worldKey, newKey);
                                break;
                            }
                        }
                    }
                    if (!committed) {
                        // 그사이 다른 이벤트로 키가 바뀌었거나 이미 이어짐: 엔진 묶음을 건드리지 않는다.
                        continue;
                    }

                    // 실물 재확인(고쳐 쓰기 직전, Codex A5): 묶음이 아직 관측된 키일 때만 기록
                    auto* liveUnique = targetExtra->GetByType<RE::ExtraUniqueID>();
                    if (!liveUnique || liveUnique->baseID != p.observedExtraKey.ownerFormId || liveUnique->uniqueID != p.observedExtraKey.uniqueId) {
                        SKSE::log::warn("GemInventory: extra key changed before rewrite for gem instance {}; reverting ledger key.",
                            p.instanceId);
                        std::scoped_lock lock(g_mutex);
                        for (auto& e : g_entries) {
                            if (e.gem.instanceId == p.instanceId && !e.isWorld && e.key == newKey) {
                                e.key = p.worldKey;
                                e.isWorld = true;
                                break;
                            }
                        }
                        continue;
                    }
                    liveUnique->baseID = ownerId;
                    liveUnique->uniqueID = newUid;
                    ++linked;
                    SKSE::log::info("GemInventory: relinked picked-up gem instance {} {:08X}:{:04X} -> {:08X}:{:04X}.",
                        p.instanceId, p.worldKey.ownerFormId, p.worldKey.uniqueId, ownerId, newUid);
                }
            });

            return linked;
        }

        // 주 갱신 스레드: 컨테이너 변경 이벤트로 예약된 줍기 1건을 잇는다(결정 123).
        void RelinkPickedUpGemOnMainUpdate(RE::FormID newOwnerFormId, std::uint16_t stamp)
        {
            auto* ownerForm = RE::TESForm::LookupByID(newOwnerFormId);
            auto* newOwner = ownerForm ? ownerForm->As<RE::TESObjectREFR>() : nullptr;
            if (newOwner && RelinkPickedUpGemsIn(newOwner, stamp) > 0) {
                return;
            }

            bool stillWorld = false;
            {
                std::scoped_lock lock(g_mutex);
                for (const auto& e : g_entries) {
                    if (e.isWorld && e.key.uniqueId == stamp) {
                        stillWorld = true;
                        break;
                    }
                }
            }
            if (stillWorld) {
                SKSE::log::warn(
                    "GemInventory: picked-up gem stamp {:04X} not relinked in container {:08X}{}; "
                    "will retry on next player inventory scan or load.",
                    stamp, newOwnerFormId, newOwner ? "" : " (reference unavailable)");
            }
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
                // 소유자 없이 UID만 있는 비정상 이벤트는 무시한다.
                if (event->newBaseID == 0 && event->newUniqueID != 0) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                std::uint64_t droppedInstanceId = 0;
                RE::FormID droppedRefId = 0;
                std::uint16_t stampUid = 0;
                std::string droppedGemId;

                {
                    std::scoped_lock lock(g_mutex);
                    for (auto it = g_entries.begin(); it != g_entries.end(); ++it) {
                        if (it->key.ownerFormId == event->oldBaseID &&
                            it->key.uniqueId == event->oldUniqueID) {
                            // C-35: event->objectID가 채워져 있고 베이스 캐시가 있으면 일치하는 젬만 처리(엔진 조회 없음)
                            if (event->objectID != 0 && it->baseFormId != 0 && it->baseFormId != event->objectID) {
                                continue;
                            }
                            // 아이템 소멸/삭제(newBaseID == 0 && newUniqueID == 0) 시 좀비 엔트리를 남기지 않고 제거한다.
                            if (event->newBaseID == 0 && event->newUniqueID == 0) {
                                g_entries.erase(it);
                                return RE::BSEventNotifyControl::kContinue;
                            }

                            if (WorldStampLink::NeedsWorldStamp(event->newBaseID, event->newUniqueID)) {
                                // C-1 §2.2: 직전 키가 바닥 키였어도(다시 잇기 전 재드랍) 새 스탬프를 즉시 할당·선기록한다.
                                stampUid = AllocateWorldStampLocked();
                                it->key.ownerFormId = event->newBaseID;
                                it->key.uniqueId = stampUid;
                                it->isWorld = true;
                                if (stampUid != 0) {
                                    droppedInstanceId = it->gem.instanceId;
                                    droppedRefId = event->newBaseID;
                                    droppedGemId = it->gem.gemId;
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

                // 결정 123: 버릴 때 바닥 참조 번호 남기기는 결정 119의 예외로 이벤트 안에서(뮤텍스 해제 후) 즉시 수행한다.
                // 참조가 아직 조회되지 않을 때만 주 갱신 큐로 1회 보완을 예약한다.
                if (droppedInstanceId != 0 && droppedRefId != 0 && stampUid != 0) {
                    if (!TryStampDroppedWorldRef(droppedRefId, droppedInstanceId, stampUid, droppedGemId) && g_queue) {
                        g_queue->Enqueue([droppedRefId, droppedInstanceId, stampUid, droppedGemId]() {
                            (void)TryStampDroppedWorldRef(droppedRefId, droppedInstanceId, stampUid, droppedGemId);
                        });
                    }
                }

                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            UniqueIdEventSink() = default;
        };

        // 결정 59·123·C-1: 바닥에서 주운 스탬프 젬을 감지해 다시 잇기를 주 갱신 큐로 예약한다.
        class ContainerChangedEventSink final : public RE::BSTEventSink<RE::TESContainerChangedEvent>
        {
        public:
            static ContainerChangedEventSink& Get()
            {
                static ContainerChangedEventSink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESContainerChangedEvent* event,
                RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
            {
                if (!event || !g_queue ||
                    !WorldStampLink::IsStampedWorldPickup(
                        event->oldContainer, event->newContainer, event->uniqueID, event->itemCount, kGemStampBand)) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                const auto gemId = GemIdForForm(event->baseObj);
                if (gemId.empty()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                bool tracked = false;
                {
                    std::scoped_lock lock(g_mutex);
                    for (const auto& e : g_entries) {
                        if (WorldStampLink::MatchesPickupEvent(
                                e.key, e.isWorld, e.gem.gemId, event->uniqueID, gemId, kGemStampBand)) {
                            tracked = true;
                            break;
                        }
                    }
                }
                if (tracked) {
                    const RE::FormID newOwner = event->newContainer;
                    const std::uint16_t stamp = event->uniqueID;
                    g_queue->Enqueue([newOwner, stamp]() {
                        RelinkPickedUpGemOnMainUpdate(newOwner, stamp);
                    });
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            ContainerChangedEventSink() = default;
        };
    }

    bool GemInventory::Install(Core::CommandQueue* queue)
    {
        if (g_installed) {
            return true;
        }
        g_queue = queue;
        BuildGemFormCache();

        auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
        if (!holder) {
            SKSE::log::error("GemInventory: ScriptEventSourceHolder unavailable.");
            return false;
        }
        holder->AddEventSink<RE::TESUniqueIDChangeEvent>(&UniqueIdEventSink::Get());
        holder->AddEventSink<RE::TESContainerChangedEvent>(&ContainerChangedEventSink::Get());
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
        SKSE::log::info("GemInventory: TESUniqueIDChangeEvent + TESContainerChangedEvent sinks registered (cached {} gem forms).",
            g_gemIdToFormId.size());
        return true;
    }

    std::size_t GemInventory::GiveStarterGems(RE::PlayerCharacter* player)
    {
        if (!player) {
            return 0;
        }
        std::size_t added = 0;
        for (const auto& entry : FormIds::kAllGemForms) {
            auto* misc = FormIds::Lookup<RE::TESObjectMISC>(entry.localFormId);
            if (!misc) {
                SKSE::log::warn("GemInventory: starter gem form '{}' (0x{:06X}) missing from {}.",
                    entry.gemId, entry.localFormId, FormIds::kPluginName);
                continue;
            }
            player->AddObjectToContainer(misc, nullptr, 1, nullptr);
            ++added;
        }
        SKSE::log::info("GemInventory: gave {}/{} starter gems to player.", added, FormIds::kAllGemForms.size());
        return added;
    }

    std::size_t GemInventory::GiveStage5StarterGems(RE::PlayerCharacter* player)
    {
        if (!player) {
            return 0;
        }
        std::size_t added = 0;
        for (std::size_t i = FormIds::kStage5GemStartIndex; i < FormIds::kAllGemForms.size(); ++i) {
            const auto& entry = FormIds::kAllGemForms[i];
            auto* misc = FormIds::Lookup<RE::TESObjectMISC>(entry.localFormId);
            if (!misc) {
                SKSE::log::warn("GemInventory: stage5 gem form '{}' (0x{:06X}) missing from {}.",
                    entry.gemId, entry.localFormId, FormIds::kPluginName);
                continue;
            }
            player->AddObjectToContainer(misc, nullptr, 1, nullptr);
            ++added;
        }
        SKSE::log::info("GemInventory: gave {}/{} stage5 starter gems to player.", added, FormIds::kStage5GemCount);
        return added;
    }

    std::size_t GemInventory::RelinkPickedUpGems(RE::TESObjectREFR* owner, bool postLoadOwnerNormalized)
    {
        return RelinkPickedUpGemsIn(owner, 0, postLoadOwnerNormalized);
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

        const auto ownerId = player->GetFormID();
        const auto uid = NextFreeInventoryUniqueId(player);
        if (uid == 0) {
            SKSE::log::error("GemInventory: failed to allocate inventory UID for '{}'.", gem.gemId);
            return std::nullopt;
        }

        auto* extra = CreateGameExtraDataList();
        if (!extra) {
            SKSE::log::error("GemInventory: failed to allocate ExtraDataList for '{}'.", gem.gemId);
            return std::nullopt;
        }
        extra->Add(new RE::ExtraUniqueID(ownerId, uid));

        const std::uint64_t instanceId = gem.instanceId;
        const RE::FormID baseFormId = misc->GetFormID();
        Core::InventoryItemKey key{ ownerId, uid };
        {
            std::scoped_lock lock(g_mutex);
            g_entries.push_back(TrackedGemEntry{ key, false, std::move(gem), baseFormId });
        }

        player->AddObjectToContainer(misc, extra, 1, nullptr);

        // AddObjectToContainer 도중 동기 이벤트로 키가 갱신됐을 수 있으므로 최신 키를 조회하고
        // 인벤토리 실물 ExtraDataList 존재 여부를 확인한다(M-5, C-5 일관성).
        Core::InventoryItemKey finalKey = key;
        {
            std::scoped_lock lock(g_mutex);
            for (const auto& entry : g_entries) {
                if (entry.gem.instanceId == instanceId) {
                    finalKey = entry.key;
                    break;
                }
            }
        }
        if (!finalKey.IsValid() || FindExtraListByUniqueId(player, misc, finalKey) == nullptr) {
            SKSE::log::error("GemInventory: returned gem instance {} not found in player inventory after AddObjectToContainer; rolling back ledger entry.",
                instanceId);
            std::scoped_lock lock(g_mutex);
            std::erase_if(g_entries, [instanceId](const TrackedGemEntry& e) {
                return e.gem.instanceId == instanceId;
            });
            return std::nullopt;
        }
        return finalKey;
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
                // Q-6: 동일 베이스 추적 젬과 순정 젬이 공존할 때 RemoveItem(nullptr)이 추적 젬을 먹는 엔진 동작 차단
                if (!UniqueIdUtil::RemovePlainItem(player, misc, 1)) {
                    return std::nullopt;
                }
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
        // C-35: OnLoad(엔진 호출 금지)에서도 읽기 전용 폼 캐시만으로 baseFormId를 채운다.
        for (auto& entry : entries) {
            if (entry.baseFormId == 0) {
                entry.baseFormId = CachedFormIdForGem(entry.gem.gemId);
            }
        }

        std::scoped_lock lock(g_mutex);
        g_entries = std::move(entries);
        // 복원된 바닥 스탬프와 충돌하지 않도록 g_nextWorldStamp를 최댓값 다음으로 전진한다.
        std::uint16_t maxWorld = 0;
        for (const auto& entry : g_entries) {
            if (entry.isWorld && kGemStampBand.Contains(entry.key.uniqueId)) {
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
