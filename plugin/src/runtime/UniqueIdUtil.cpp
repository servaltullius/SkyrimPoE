#include "PCH.h"
#include "runtime/UniqueIdUtil.h"

#include <algorithm>
#include <mutex>
#include <vector>

namespace SkyrimPoE::Runtime::UniqueIdUtil
{
    namespace
    {
        std::mutex g_collectorMutex;
        std::vector<std::function<void(std::unordered_set<std::uint16_t>&)>> g_collectors;
        std::vector<std::function<bool(RE::FormID, std::uint16_t)>> g_trackedCheckers;

        [[nodiscard]] bool IsTrackedExtraList(const RE::ExtraDataList* extra)
        {
            if (!extra) {
                return false;
            }
            const auto* unique = extra->GetByType<RE::ExtraUniqueID>();
            if (!unique || unique->uniqueID == 0) {
                return false;
            }
            // 바닥 스탬프 대역(0x7F00..0xFFFD)은 미재연결 추적품일 수 있으므로 항상 보호한다.
            if (unique->uniqueID >= kGemWorldStampMin && unique->uniqueID <= kItemWorldStampMax) {
                return true;
            }
            std::vector<std::function<bool(RE::FormID, std::uint16_t)>> checkers;
            {
                std::scoped_lock lock(g_collectorMutex);
                checkers = g_trackedCheckers;
            }
            for (const auto& fn : checkers) {
                if (fn && fn(unique->baseID, unique->uniqueID)) {
                    return true;
                }
            }
            return false;
        }

        // 추적품 + 강화/마법부여/명명/즐겨찾기 등 플레이어가 보호해야 할 특수 ExtraDataList 판별(M-1)
        [[nodiscard]] bool IsProtectedExtraList(const RE::ExtraDataList* extra)
        {
            if (!extra) {
                return false;
            }
            if (IsTrackedExtraList(extra)) {
                return true;
            }
            return extra->HasType<RE::ExtraHealth>() ||
                extra->HasType<RE::ExtraEnchantment>() ||
                extra->HasType<RE::ExtraTextDisplayData>() ||
                extra->HasType<RE::ExtraHotkey>();
        }
    }

    RE::ExtraDataList* CreateGameExtraDataList()
    {
        const auto ver = REL::Module::get().version();
        const std::size_t allocSize = (ver >= SKSE::RUNTIME_SSE_1_6_629) ? 0x20 : 0x18;
        auto* mem = RE::calloc<std::byte>(allocSize);
        if (!mem) {
            return nullptr;
        }
        auto* list = reinterpret_cast<RE::ExtraDataList*>(mem);
        static REL::Relocation<void(RE::ExtraDataList*)> ctor{ RELOCATION_ID(11437, 11583) };
        ctor(list);
        return list;
    }

    void DestroyGameExtraDataList(RE::ExtraDataList* list)
    {
        if (!list) {
            return;
        }
        static REL::Relocation<void(RE::ExtraDataList*)> dtor{ RELOCATION_ID(11438, 11584) };
        dtor(list);
        RE::free(list);
    }

    void RegisterUsedInventoryUidCollector(std::function<void(std::unordered_set<std::uint16_t>&)> collector)
    {
        if (!collector) {
            return;
        }
        std::scoped_lock lock(g_collectorMutex);
        g_collectors.push_back(std::move(collector));
    }

    void RegisterTrackedKeyChecker(std::function<bool(RE::FormID, std::uint16_t)> checker)
    {
        if (!checker) {
            return;
        }
        std::scoped_lock lock(g_collectorMutex);
        g_trackedCheckers.push_back(std::move(checker));
    }

    std::uint16_t AllocateInventoryUniqueId(RE::TESObjectREFR* owner)
    {
        std::unordered_set<std::uint16_t> used;
        if (owner) {
            // 검수 M6: 읽기 탐색은 반드시 GetInventoryChanges(true)로 호출해 미초기화 컨테이너 변이 차단
            auto* changes = owner->GetInventoryChanges(true);
            if (changes && changes->entryList) {
                for (auto* entry : *changes->entryList) {
                    if (!entry || !entry->extraLists) {
                        continue;
                    }
                    for (auto* extra : *entry->extraLists) {
                        const auto* unique = extra ? extra->GetByType<RE::ExtraUniqueID>() : nullptr;
                        if (unique && unique->uniqueID >= kInventoryUidMin && unique->uniqueID <= kInventoryUidMax) {
                            used.insert(unique->uniqueID);
                        }
                    }
                }
            }
        }

        std::vector<std::function<void(std::unordered_set<std::uint16_t>&)>> cbs;
        {
            std::scoped_lock lock(g_collectorMutex);
            cbs = g_collectors;
        }
        for (const auto& cb : cbs) {
            if (cb) {
                cb(used);
            }
        }

        for (std::uint32_t candidate = kInventoryUidMin; candidate <= kInventoryUidMax; ++candidate) {
            const auto uid = static_cast<std::uint16_t>(candidate);
            if (!used.contains(uid)) {
                return uid;
            }
        }
        SKSE::log::error("UniqueIdUtil: inventory UID range [0x{:04X}..0x{:04X}] exhausted; refusing duplicate UID.",
            kInventoryUidMin, kInventoryUidMax);
        return 0;
    }

    std::int32_t CountRemovablePlainItems(RE::TESObjectREFR* owner, RE::TESBoundObject* baseObj)
    {
        if (!owner || !baseObj) {
            return 0;
        }
        auto* changes = owner->GetInventoryChanges(false);
        RE::InventoryEntryData* targetEntry = nullptr;
        if (changes && changes->entryList) {
            for (auto* entry : *changes->entryList) {
                if (entry && entry->object == baseObj) {
                    targetEntry = entry;
                    break;
                }
            }
        }
        std::int32_t baseCount = 0;
        if (const auto* baseForm = owner->GetBaseObject()) {
            if (const auto* container = baseForm->As<RE::TESContainer>()) {
                baseCount = container->GetObjectCount(baseObj);
            }
        }
        const std::int32_t total = baseCount + (targetEntry ? targetEntry->countDelta : 0);
        if (!targetEntry || !targetEntry->extraLists || targetEntry->extraLists->empty()) {
            return std::max<std::int32_t>(0, total);
        }
        // RemovePlainItem의 사전 검증과 같은 규칙: 일반 스택 + 보호되지 않은 ExtraDataList
        std::int32_t extraTotal = 0;
        std::int32_t unprotected = 0;
        for (auto* ex : *targetEntry->extraLists) {
            if (!ex) {
                continue;
            }
            const std::int32_t cnt = std::max<std::int32_t>(1, ex->GetCount());
            extraTotal += cnt;
            if (!IsProtectedExtraList(ex)) {
                unprotected += cnt;
            }
        }
        return std::max<std::int32_t>(0, total - extraTotal) + unprotected;
    }

    bool RemovePlainItem(RE::TESObjectREFR* owner, RE::TESBoundObject* baseObj, std::int32_t count)
    {
        if (!owner || !baseObj || count <= 0) {
            return false;
        }

        auto* changes = owner->GetInventoryChanges(false);
        RE::InventoryEntryData* targetEntry = nullptr;
        if (changes && changes->entryList) {
            for (auto* entry : *changes->entryList) {
                if (entry && entry->object == baseObj) {
                    targetEntry = entry;
                    break;
                }
            }
        }

        std::int32_t baseCount = 0;
        if (const auto* baseForm = owner->GetBaseObject()) {
            if (const auto* container = baseForm->As<RE::TESContainer>()) {
                baseCount = container->GetObjectCount(baseObj);
            }
        }
        const std::int32_t initialTotal = baseCount + (targetEntry ? targetEntry->countDelta : 0);
        if (initialTotal < count) {
            SKSE::log::warn("UniqueIdUtil::RemovePlainItem: total {} < requested {} for base 0x{:08X}.",
                initialTotal, count, baseObj->GetFormID());
            return false;
        }

        const bool hasExtraLists = targetEntry && targetEntry->extraLists && !targetEntry->extraLists->empty();
        if (!hasExtraLists) {
            // 특수/추적 ExtraDataList가 전혀 없고 보유 수량이 충분하므로 기본 RemoveItem이 일반 스택만 안전하게 제거한다.
            owner->RemoveItem(baseObj, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            return true;
        }

        // 사전 수량 검증(M-1): count > 1 요청 시 도중 실패로 부분 차감되지 않도록 제거 가능한 일반+비보호 수량을 먼저 확인
        {
            std::int32_t preExtraTotal = 0;
            std::int32_t preUnprotected = 0;
            for (auto* ex : *targetEntry->extraLists) {
                if (!ex) {
                    continue;
                }
                const std::int32_t cnt = std::max<std::int32_t>(1, ex->GetCount());
                preExtraTotal += cnt;
                if (!IsProtectedExtraList(ex)) {
                    preUnprotected += cnt;
                }
            }
            const std::int32_t prePlain = std::max<std::int32_t>(0, initialTotal - preExtraTotal);
            if (prePlain + preUnprotected < count) {
                SKSE::log::warn("UniqueIdUtil::RemovePlainItem: removable plain({}) + unprotected({}) < requested {} for base 0x{:08X}.",
                    prePlain, preUnprotected, count, baseObj->GetFormID());
                return false;
            }
        }

        // Q-6 우회: 일반(비-extra) 스택이 있으면 임시 빈 ExtraDataList로 정확히 1개만 제거하고,
        // 일반 스택이 없고 미추적·비강화 ExtraDataList만 있으면 보호 대상(추적·스탬프·단련·마부·명명·즐겨찾기)을 피해 명시 제거한다.
        for (std::int32_t i = 0; i < count; ++i) {
            auto* freshChanges = owner->GetInventoryChanges(false);
            RE::InventoryEntryData* curEntry = nullptr;
            if (freshChanges && freshChanges->entryList) {
                for (auto* entry : *freshChanges->entryList) {
                    if (entry && entry->object == baseObj) {
                        curEntry = entry;
                        break;
                    }
                }
            }
            if (!curEntry) {
                return false;
            }
            if (!curEntry->extraLists || curEntry->extraLists->empty()) {
                owner->RemoveItem(baseObj, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                continue;
            }

            std::int32_t extraTotal = 0;
            RE::ExtraDataList* untrackedBag = nullptr;
            RE::ExtraDataList* untrackedWorn = nullptr;
            for (auto* ex : *curEntry->extraLists) {
                if (!ex) {
                    continue;
                }
                extraTotal += std::max<std::int32_t>(1, ex->GetCount());
                if (!IsProtectedExtraList(ex)) {
                    const bool worn = ex->HasType<RE::ExtraWorn>() || ex->HasType<RE::ExtraWornLeft>();
                    if (!worn && !untrackedBag) {
                        untrackedBag = ex;
                    } else if (worn && !untrackedWorn) {
                        untrackedWorn = ex;
                    }
                }
            }

            const std::int32_t totalCount = baseCount + curEntry->countDelta;
            const std::int32_t plainCount = totalCount - extraTotal;
            if (plainCount >= 1) {
                auto* tempExtra = CreateGameExtraDataList();
                if (!tempExtra) {
                    SKSE::log::error("UniqueIdUtil::RemovePlainItem: CreateGameExtraDataList failed.");
                    return false;
                }
                curEntry->AddExtraList(tempExtra);
                owner->RemoveItem(baseObj, 1, RE::ITEM_REMOVE_REASON::kRemove, tempExtra, nullptr);
            } else if (RE::ExtraDataList* pick = untrackedBag ? untrackedBag : untrackedWorn) {
                owner->RemoveItem(baseObj, 1, RE::ITEM_REMOVE_REASON::kRemove, pick, nullptr);
            } else {
                SKSE::log::warn("UniqueIdUtil::RemovePlainItem: no unprotected plain item left for base 0x{:08X} (total={}, extra={}).",
                    baseObj->GetFormID(), totalCount, extraTotal);
                return false;
            }
        }
        return true;
    }
}
