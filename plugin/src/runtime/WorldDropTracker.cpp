#include "PCH.h"
#include "runtime/WorldDropTracker.h"
#include "runtime/FormIds.h"

#include "core/LootTable.h"

#include <cmath>
#include <mutex>
#include <random>
#include <unordered_set>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        struct ActiveWorldDrop
        {
            RE::FormID dropRefFormId = 0;
            RE::FormID corpseRefFormId = 0;
            RE::FormID beamRefFormId = 0;
            std::uint8_t worldDropTier = 2;
            double elapsedSec = 0.0;
        };

        const Core::ItemData* g_itemData = nullptr;
        const Core::Config* g_config = nullptr;

        std::mutex g_dropMutex;
        std::vector<ActiveWorldDrop> g_active;
        std::mt19937_64 g_rng{ std::random_device{}() };
        double g_sweepAccumSec = 0.0;
        RE::FormID g_lastPlayerWorldspaceId = 0;
        RE::FormID g_lastPlayerInteriorCellId = 0;
        bool g_lastWasInterior = false;
        bool g_hasLocationHistory = false;

        [[nodiscard]] bool IsSkyrimPoELootForm(const RE::TESForm* base) noexcept
        {
            if (!base) {
                return false;
            }
            const RE::FormID fid = base->GetFormID();
            for (const auto localId : FormIds::kCurrencyOrbForms) {
                if (const auto* f = FormIds::Lookup<RE::TESObjectMISC>(localId); f && f->GetFormID() == fid) {
                    return true;
                }
            }
            for (std::size_t off = 0; off < FormIds::kSealedTokenCount; ++off) {
                const auto localId = FormIds::kSealedTokenBase + static_cast<RE::FormID>(off);
                if (const auto* f = FormIds::Lookup<RE::TESObjectMISC>(localId); f && f->GetFormID() == fid) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::int32_t CountItemInContainer(RE::TESObjectREFR* owner, RE::TESBoundObject* obj)
        {
            if (!owner || !obj) return 0;
            const auto counts = owner->GetInventoryCounts([obj](RE::TESBoundObject& c) { return &c == obj; });
            const auto it = counts.find(obj);
            return it != counts.end() ? it->second : 0;
        }

        [[nodiscard]] bool IsBeamBaseForm(const RE::TESForm* base) noexcept
        {
            if (!base) {
                return false;
            }
            const auto* rareBeam = FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamRare);
            const auto* currBeam = FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamCurrency);
            const RE::FormID id = base->GetFormID();
            return (rareBeam && rareBeam->GetFormID() == id) || (currBeam && currBeam->GetFormID() == id);
        }

        // 정적 임시 빛기둥 마커 전용 삭제 헬퍼(절대 Actor에 호출하지 않음)
        void DeleteBeamMarker(RE::FormID beamFormId)
        {
            if (beamFormId == 0) {
                return;
            }
            auto* form = RE::TESForm::LookupByID(beamFormId);
            auto* refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!refr || refr->As<RE::Actor>() != nullptr) {
                return;
            }
            if (!IsBeamBaseForm(refr->GetBaseObject())) {
                return;
            }
            refr->Disable();
            refr->SetDelete(true);
        }

        void PlayDropSound(bool highTier)
        {
            if (!g_config || !g_config->lootSoundEnabled) {
                return;
            }
            if (auto* audio = RE::BSAudioManager::GetSingleton()) {
                const RE::FormID sfxId = highTier ? FormIds::kSfxLootHighOrb : FormIds::kSfxLootRare;
                audio->Play(sfxId);
            }
        }

        bool TryReturnSingleDropToCorpse(const ActiveWorldDrop& d)
        {
            DeleteBeamMarker(d.beamRefFormId);

            auto* dropForm = RE::TESForm::LookupByID(d.dropRefFormId);
            auto* dropRef = dropForm ? dropForm->As<RE::TESObjectREFR>() : nullptr;
            if (!dropRef || dropRef->IsDeleted() || dropRef->IsDisabled()) {
                return true;  // 이미 주웠거나 사라짐
            }

            auto* corpseForm = RE::TESForm::LookupByID(d.corpseRefFormId);
            auto* corpseRef = corpseForm ? corpseForm->As<RE::TESObjectREFR>() : nullptr;
            auto* baseObj = dropRef->GetBaseObject();

            // 검수 C4, M4: SkyrimPoE 전리품 폼임을 재확인하고 시체가 로드·유효할 때만 정확한 스택 수량으로 회수
            if (corpseRef && !corpseRef->IsDeleted() && !corpseRef->IsDisabled() && baseObj &&
                IsSkyrimPoELootForm(baseObj) &&
                dropRef->As<RE::Actor>() == nullptr) {
                const std::int32_t cnt = std::max<std::int32_t>(1, dropRef->extraList.GetCount());
                corpseRef->AddObjectToContainer(baseObj, nullptr, cnt, nullptr);
                dropRef->Disable();
                dropRef->SetDelete(true);
                return true;
            }
            return false;
        }
    }

    void WorldDropTracker::Initialize(const Core::ItemData* itemData, const Core::Config* config)
    {
        g_itemData = itemData;
        g_config = config;
    }

    bool WorldDropTracker::SpawnPhysicalDropFromContainer(
        RE::TESObjectREFR* sourceContainer,
        RE::TESBoundObject* itemObj,
        std::uint8_t worldDropTier,
        bool isCurrencyDrop)
    {
        if (!sourceContainer || !itemObj || !g_config) {
            return false;
        }

        // 상한 초과 시 가장 오래된 드랍 1건을 시체로 회수(결정 16, 159)
        ActiveWorldDrop evicted{};
        bool hasEvicted = false;
        {
            std::scoped_lock lock(g_dropMutex);
            if (g_active.size() >= g_config->worldDropMaxCount && !g_active.empty()) {
                evicted = g_active.front();
                g_active.erase(g_active.begin());
                hasEvicted = true;
            }
        }
        if (hasEvicted) {
            (void)TryReturnSingleDropToCorpse(evicted);
        }

        // 인벤토리에 추가 후 kDropping으로 월드로 이동(복제 금지, 검수 M3: 전후 수량 대조로 중복 지급 차단)
        const std::int32_t countBefore = CountItemInContainer(sourceContainer, itemObj);
        sourceContainer->AddObjectToContainer(itemObj, nullptr, 1, nullptr);
        const auto droppedHandle = sourceContainer->RemoveItem(
            itemObj, 1, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
        auto droppedPtr = droppedHandle.get();
        if (!droppedPtr) {
            // RemoveItem 실패로 시체에 이미 남아 있으면(countAfter > countBefore) 추가 지급하지 않고 true 취급,
            // 둘 다 없어진 경우에만 시체에 1개 안전 복구
            const std::int32_t countAfter = CountItemInContainer(sourceContainer, itemObj);
            if (countAfter <= countBefore) {
                sourceContainer->AddObjectToContainer(itemObj, nullptr, 1, nullptr);
            }
            return false;
        }

        RE::FormID beamId = 0;
        if (g_config->lootBeamEnabled) {
            const RE::FormID beamLocal = isCurrencyDrop ? FormIds::kLootBeamCurrency : FormIds::kLootBeamRare;
            if (auto* beamBase = FormIds::Lookup<RE::TESObjectSTAT>(beamLocal)) {
                if (auto beamPtr = droppedPtr->PlaceObjectAtMe(beamBase, false)) {
                    beamId = beamPtr->GetFormID();
                }
            }
        }

        PlayDropSound(isCurrencyDrop && worldDropTier >= 2);

        {
            std::scoped_lock lock(g_dropMutex);
            g_active.push_back(ActiveWorldDrop{
                droppedPtr->GetFormID(),
                sourceContainer->GetFormID(),
                beamId,
                worldDropTier,
                0.0
            });
        }
        return true;
    }

    void WorldDropTracker::OnKillAwarded(
        RE::ActorHandle corpseHandle,
        std::uint32_t effectiveMonsterLevel,
        bool isBoss,
        bool isDragon)
    {
        if (!g_itemData || !g_itemData->IsLoaded() || !g_config || !g_config->enableLootDrops) {
            return;
        }
        auto corpsePtr = corpseHandle.get();
        auto* corpse = corpsePtr.get();
        if (!corpse || corpse->IsDeleted()) {
            return;
        }

        std::vector<Core::DropEntry> drops;
        {
            std::scoped_lock lock(g_dropMutex);
            drops = Core::LootTable::RollKillDrops(
                effectiveMonsterLevel, isBoss, isDragon, *g_config, *g_itemData, g_rng);
        }
        if (drops.empty()) {
            return;
        }

        for (const auto& d : drops) {
            RE::TESObjectMISC* misc = nullptr;
            bool isCurr = false;
            if (d.kind == Core::DropKind::SealedToken) {
                misc = FormIds::Lookup<RE::TESObjectMISC>(
                    FormIds::SealedTokenLocalFormId(d.tier, d.rarity, d.slotGroup));
            } else {
                isCurr = true;
                misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CurrencyLocalFormId(d.currency));
            }
            if (!misc) {
                continue;
            }

            if (g_config->worldDropEnabled && d.worldDropTier >= g_config->worldDropMinTier) {
                // 검수 M3: SpawnPhysicalDropFromContainer가 실패 시 내부에서 시체 잔여를 보장하므로 중복 추가 금지
                (void)SpawnPhysicalDropFromContainer(corpse, misc, d.worldDropTier, isCurr);
            } else {
                corpse->AddObjectToContainer(misc, nullptr, 1, nullptr);
            }
        }
    }

    std::size_t WorldDropTracker::SweepOrphanBeamsNearPlayer()
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell = player ? player->GetParentCell() : nullptr;
        if (!cell) {
            return 0;
        }

        std::unordered_set<RE::FormID> trackedBeams;
        {
            std::scoped_lock lock(g_dropMutex);
            for (const auto& d : g_active) {
                if (d.beamRefFormId != 0) {
                    trackedBeams.insert(d.beamRefFormId);
                }
            }
        }

        // 검수 m6: ForEachReference 스핀락 밖에서 두 빛기둥 FormID를 1회 캐싱
        const auto* rareBeam = FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamRare);
        const auto* currBeam = FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamCurrency);
        const RE::FormID rareBeamId = rareBeam ? rareBeam->GetFormID() : 0;
        const RE::FormID currBeamId = currBeam ? currBeam->GetFormID() : 0;
        if (rareBeamId == 0 && currBeamId == 0) {
            return 0;
        }

        std::vector<RE::FormID> orphans;
        cell->ForEachReference([&](RE::TESObjectREFR* ref) {
            if (!ref) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const RE::FormID fid = ref->GetFormID();
            if ((fid & 0xFF000000u) == 0xFF000000u &&
                !ref->IsDeleted() &&
                !ref->IsDisabled() &&
                ref->As<RE::Actor>() == nullptr &&
                !trackedBeams.contains(fid)) {
                const auto* base = ref->GetBaseObject();
                const RE::FormID baseId = base ? base->GetFormID() : 0;
                if (baseId != 0 && (baseId == rareBeamId || baseId == currBeamId)) {
                    orphans.push_back(fid);
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        for (const auto fid : orphans) {
            DeleteBeamMarker(fid);
        }
        if (!orphans.empty()) {
            SKSE::log::info("WorldDropTracker: swept {} orphan loot beam marker(s).", orphans.size());
        }
        return orphans.size();
    }

    void WorldDropTracker::Tick(double dt, bool paused)
    {
        if (paused || !g_config) {
            return;
        }

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* curCell = player ? player->GetParentCell() : nullptr;
        auto* curWs = player ? player->GetWorldspace() : nullptr;
        const bool isInterior = curCell ? curCell->IsInteriorCell() : false;
        const RE::FormID wsId = curWs ? curWs->GetFormID() : 0;
        const RE::FormID intCellId = (isInterior && curCell) ? curCell->GetFormID() : 0;

        // 검수 M4: 야외 인접 셀 경계 통과는 무시하고, 인테리어↔야외 전환·인테리어 셀 변경·월드스페이스 변경만 구역 이탈로 판정
        bool zoneChanged = false;
        if (curCell) {
            if (g_hasLocationHistory) {
                if (isInterior != g_lastWasInterior ||
                    (isInterior && intCellId != g_lastPlayerInteriorCellId) ||
                    (!isInterior && wsId != g_lastPlayerWorldspaceId)) {
                    zoneChanged = true;
                }
            }
            g_lastWasInterior = isInterior;
            g_lastPlayerInteriorCellId = intCellId;
            g_lastPlayerWorldspaceId = wsId;
            g_hasLocationHistory = true;
        }

        if (zoneChanged) {
            (void)SweepOrphanBeamsNearPlayer();
        }

        g_sweepAccumSec += dt;
        if (g_sweepAccumSec < 0.5 && !zoneChanged) {
            return;
        }
        const double step = g_sweepAccumSec;
        g_sweepAccumSec = 0.0;

        std::vector<ActiveWorldDrop> snapshot;
        {
            std::scoped_lock lock(g_dropMutex);
            if (g_active.empty()) {
                return;
            }
            for (auto& d : g_active) {
                d.elapsedSec += step;
            }
            snapshot = g_active;
        }

        const auto playerPos = player ? player->GetPosition() : RE::NiPoint3{};
        constexpr float kFarReturnDistSq = 4096.0f * 4096.0f;

        std::vector<RE::FormID> toRemoveDropIds;
        for (const auto& d : snapshot) {
            auto* dropForm = RE::TESForm::LookupByID(d.dropRefFormId);
            auto* dropRef = dropForm ? dropForm->As<RE::TESObjectREFR>() : nullptr;

            // 1. 주웠거나 사라짐 → 빛기둥 즉시 삭제
            if (!dropRef || dropRef->IsDeleted() || dropRef->IsDisabled()) {
                DeleteBeamMarker(d.beamRefFormId);
                toRemoveDropIds.push_back(d.dropRefFormId);
                continue;
            }

            // 2. 시간 초과·구역 이탈·플레이어 원거리 이탈(>4096u) → 시체로 회수(시체 부재 시 빛기둥만 정리)
            const auto dp = dropRef->GetPosition();
            const float px = dp.x - playerPos.x;
            const float py = dp.y - playerPos.y;
            const float pz = dp.z - playerPos.z;
            const bool tooFar = player && ((px * px + py * py + pz * pz) > kFarReturnDistSq);
            if (zoneChanged || tooFar || d.elapsedSec >= g_config->worldDropReturnSeconds) {
                (void)TryReturnSingleDropToCorpse(d);
                toRemoveDropIds.push_back(d.dropRefFormId);
                continue;
            }

            // 3. 물리 구름에 맞춰 빛기둥 위치 동기화
            if (d.beamRefFormId != 0) {
                if (auto* bf = RE::TESForm::LookupByID(d.beamRefFormId)) {
                    if (auto* beamRef = bf->As<RE::TESObjectREFR>(); beamRef && !beamRef->IsDeleted()) {
                        const auto bp = beamRef->GetPosition();
                        const float dx = dp.x - bp.x;
                        const float dy = dp.y - bp.y;
                        const float dz = dp.z - bp.z;
                        if (dx * dx + dy * dy + dz * dz > 4.0f) {
                            beamRef->SetPosition(dp);
                        }
                    }
                }
            }
        }

        if (!toRemoveDropIds.empty()) {
            std::scoped_lock lock(g_dropMutex);
            std::erase_if(g_active, [&](const ActiveWorldDrop& d) {
                return std::find(toRemoveDropIds.begin(), toRemoveDropIds.end(), d.dropRefFormId) != toRemoveDropIds.end();
            });
        }
    }

    void WorldDropTracker::ReturnAllToCorpsesNow()
    {
        std::vector<ActiveWorldDrop> all;
        {
            std::scoped_lock lock(g_dropMutex);
            all.swap(g_active);
        }
        for (const auto& d : all) {
            (void)TryReturnSingleDropToCorpse(d);
        }
    }

    std::vector<SavedWorldDropEntry> WorldDropTracker::TakeSnapshotForSave()
    {
        std::scoped_lock lock(g_dropMutex);
        std::vector<SavedWorldDropEntry> out;
        out.reserve(g_active.size());
        for (const auto& d : g_active) {
            out.push_back(SavedWorldDropEntry{
                d.dropRefFormId,
                d.corpseRefFormId,
                d.beamRefFormId,
                d.worldDropTier
            });
        }
        return out;
    }

    void WorldDropTracker::RestoreFromSave(std::vector<SavedWorldDropEntry> entries)
    {
        std::scoped_lock lock(g_dropMutex);
        g_active.clear();
        g_active.reserve(entries.size());
        for (const auto& e : entries) {
            g_active.push_back(ActiveWorldDrop{
                e.dropRefFormId,
                e.corpseRefFormId,
                e.beamRefFormId,
                e.worldDropTier,
                0.0
            });
        }
    }

    void WorldDropTracker::Clear()
    {
        std::scoped_lock lock(g_dropMutex);
        g_active.clear();
        g_sweepAccumSec = 0.0;
        g_lastPlayerWorldspaceId = 0;
        g_lastPlayerInteriorCellId = 0;
        g_lastWasInterior = false;
        g_hasLocationHistory = false;
    }

    std::size_t WorldDropTracker::ActiveCount()
    {
        std::scoped_lock lock(g_dropMutex);
        return g_active.size();
    }
}
