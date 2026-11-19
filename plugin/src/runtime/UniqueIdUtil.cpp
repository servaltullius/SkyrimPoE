#include "PCH.h"
#include "runtime/UniqueIdUtil.h"

#include <mutex>
#include <vector>

namespace SkyrimPoE::Runtime::UniqueIdUtil
{
    namespace
    {
        std::mutex g_collectorMutex;
        std::vector<std::function<void(std::unordered_set<std::uint16_t>&)>> g_collectors;
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
        return kInventoryUidMax;
    }
}
