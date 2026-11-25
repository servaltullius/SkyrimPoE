#include "PCH.h"
#include "runtime/WorldDropTracker.h"
#include "runtime/FormIds.h"
#include "runtime/SaveLoad.h"
#include "runtime/MapDeviceRuntime.h"

#include "core/LootTable.h"
#include "core/MapSystem.h"
#include "core/Migration.h"

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
            RE::FormID spawnCellId = 0;
            RE::ObjectRefHandle dropHandle{};
            RE::ActorHandle corpseHandle{};
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
            const auto localId = FormIds::ResolveLocalFormIdIfOurs(base->GetFormID());
            if (localId == 0) {
                return false;
            }
            const auto* gem = Core::GemForms::FindPhysical(localId);
            return FormIds::CurrencyKindFromLocalFormId(localId).has_value() ||
                   FormIds::DecodeSealedTokenLocalFormId(localId).has_value() ||
                   localId == FormIds::kUniqueSealedToken ||
                   FormIds::JewelColorFromLocalFormId(localId).has_value() ||
                   FormIds::CharmKindFromLocalFormId(localId).has_value() ||
                   FormIds::IsMapItemLocalFormId(localId) ||
                   FormIds::IsAscendancy4TributeLocalFormId(localId) ||
                   (gem && Core::GemForms::IsRecoveryForm(SaveLoad::WorldGemGeneration(), *gem));
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
            const auto localId = FormIds::ResolveLocalFormIdIfOurs(base->GetFormID());
            return localId == FormIds::kLootBeamRare ||
                   localId == FormIds::kLootBeamCurrency ||
                   localId == FormIds::kLootBeamUnique ||
                   localId == FormIds::kLootBeamMap;
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

        enum class ReturnOutcome
        {
            Completed,        // 이미 주워짐/소실 또는 시체 회수 완료 -> 추적 종료
            CorpseGone,       // 시체가 영구 삭제/비유효 -> 빛기둥만 제거 후 추적 종료
            RetryLater        // 시체가 일시 미로드 상태 -> 빛기둥만 먼저 제거하고 추적 유지(C-20)
        };

        [[nodiscard]] RE::TESObjectREFR* ResolveDropRef(const ActiveWorldDrop& d)
        {
            if (auto ptr = d.dropHandle.get()) {
                return ptr.get();
            }
            auto* form = RE::TESForm::LookupByID(d.dropRefFormId);
            auto* refr = form ? form->As<RE::TESObjectREFR>() : nullptr;
            if (!refr || refr->As<RE::Actor>() != nullptr || !IsSkyrimPoELootForm(refr->GetBaseObject())) {
                return nullptr;
            }
            return refr;
        }

        [[nodiscard]] RE::Actor* ResolveCorpseActor(const ActiveWorldDrop& d)
        {
            if (auto ptr = d.corpseHandle.get()) {
                return ptr.get();
            }
            auto* form = RE::TESForm::LookupByID(d.corpseRefFormId);
            return form ? form->As<RE::Actor>() : nullptr;
        }

        ReturnOutcome TryReturnSingleDropToCorpse(const ActiveWorldDrop& d)
        {
            DeleteBeamMarker(d.beamRefFormId);

            auto* dropRef = ResolveDropRef(d);
            // Q-5: 삭제·비활성 또는 월드 셀 소속 상실(인벤토리로 회수됨)이면 이미 주워진 것으로 판정
            if (!dropRef || dropRef->IsDeleted() || dropRef->IsDisabled() || dropRef->GetParentCell() == nullptr) {
                return ReturnOutcome::Completed;
            }

            // Q-8: 시체 쪽은 핸들 우선 룩업 + Actor 여부 및 사망 상태 검증
            auto* corpseActor = ResolveCorpseActor(d);
            if (!corpseActor || corpseActor->IsDeleted()) {
                return ReturnOutcome::CorpseGone;
            }
            if (corpseActor->IsDisabled() || !corpseActor->IsDead()) {
                return ReturnOutcome::CorpseGone;
            }
            if (!corpseActor->Is3DLoaded() && corpseActor->GetParentCell() == nullptr) {
                return ReturnOutcome::RetryLater;
            }

            auto* baseObj = dropRef->GetBaseObject();
            if (baseObj && IsSkyrimPoELootForm(baseObj) && dropRef->As<RE::Actor>() == nullptr) {
                const std::int32_t cnt = std::max<std::int32_t>(1, dropRef->extraList.GetCount());
                corpseActor->AddObjectToContainer(baseObj, nullptr, cnt, nullptr);
                dropRef->Disable();
                dropRef->SetDelete(true);
                return ReturnOutcome::Completed;
            }
            return ReturnOutcome::CorpseGone;
        }

        bool DiscardRunDrop(const ActiveWorldDrop& drop, std::span<const RE::FormID> corpseIds,
            RE::FormID runCellId, bool packOwnsCell)
        {
            const auto resolve = [&]() -> RE::NiPointer<RE::TESObjectREFR> {
                if (auto ptr = drop.dropHandle.get()) {
                    return ptr;
                }
                auto* form = RE::TESForm::LookupByID(drop.dropRefFormId);
                return RE::NiPointer<RE::TESObjectREFR>(form ? form->As<RE::TESObjectREFR>() : nullptr);
            };
            const auto inRunCell = [runCellId](const RE::TESObjectREFR* ref) {
                const auto* cell = ref ? ref->GetParentCell() : nullptr;
                return runCellId != 0 && cell && cell->GetFormID() == runCellId;
            };

            auto ref = resolve();
            const bool fromRunCorpse = Core::MapSystem::IsRunDrop(drop.corpseRefFormId, corpseIds);
            const bool inCell = inRunCell(ref.get());
            if (!Core::IsLegacyRunDrop(fromRunCorpse, inCell, packOwnsCell)) {
                return false;
            }
            if (ref && inCell && !ref->IsDeleted() && !ref->IsDisabled() && !ref->As<RE::Actor>() &&
                IsSkyrimPoELootForm(ref->GetBaseObject())) {
                ref->Disable();
                ref->SetDelete(true);
            }
            if (auto* beamForm = drop.beamRefFormId ? RE::TESForm::LookupByID(drop.beamRefFormId) : nullptr) {
                auto* beam = beamForm->As<RE::TESObjectREFR>();
                if (beam && (inRunCell(beam) || !beam->GetParentCell())) {
                    DeleteBeamMarker(drop.beamRefFormId);
                }
            }
            return true;
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
        bool isCurrencyDrop,
        bool isUniqueDrop,
        bool isMapDrop)
    {
        if (!sourceContainer || !itemObj || !g_config) {
            return false;
        }

        // 인벤토리에 추가 후 kDropping으로 월드로 이동(복제 금지, 검수 M3: 전후 수량 대조로 중복 지급 차단)
        const std::int32_t countBefore = CountItemInContainer(sourceContainer, itemObj);
        sourceContainer->AddObjectToContainer(itemObj, nullptr, 1, nullptr);
        if (CountItemInContainer(sourceContainer, itemObj) <= countBefore) {
            return false;
        }
        const auto droppedHandle = sourceContainer->RemoveItem(
            itemObj, 1, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
        auto droppedPtr = droppedHandle.get();
        if (!droppedPtr) {
            // RemoveItem 실패 뒤 수량이 처음과 같거나 줄었을 때만 시체에 1개 복구한다.
            const std::int32_t countAfter = CountItemInContainer(sourceContainer, itemObj);
            if (countAfter <= countBefore) {
                sourceContainer->AddObjectToContainer(itemObj, nullptr, 1, nullptr);
            }
            return false;
        }

        RE::FormID beamId = 0;
        if (g_config->lootBeamEnabled) {
            const RE::FormID beamLocal = isUniqueDrop
                ? FormIds::kLootBeamUnique
                : (isMapDrop ? FormIds::kLootBeamMap : (isCurrencyDrop ? FormIds::kLootBeamCurrency : FormIds::kLootBeamRare));
            if (auto* beamBase = FormIds::Lookup<RE::TESObjectSTAT>(beamLocal)) {
                if (auto beamPtr = droppedPtr->PlaceObjectAtMe(beamBase, false)) {
                    beamId = beamPtr->GetFormID();
                }
            }
        }

        PlayDropSound(isUniqueDrop || isMapDrop || (isCurrencyDrop && worldDropTier >= 2));

        auto* corpseActor = sourceContainer->As<RE::Actor>();
        // 배출 직후 드랍의 셀을 아직 모르면 시체의 셀을 쓴다(상한 회수에서 회차 안 드랍으로 보호되게).
        const auto* cell = droppedPtr->GetParentCell();
        if (!cell) {
            cell = sourceContainer->GetParentCell();
        }
        const ActiveWorldDrop newDrop{
            droppedPtr->GetFormID(),
            sourceContainer->GetFormID(),
            beamId,
            cell ? cell->GetFormID() : 0,
            droppedHandle,
            corpseActor ? corpseActor->GetHandle() : RE::ActorHandle{},
            worldDropTier,
            0.0
        };
        {
            std::scoped_lock lock(g_dropMutex);
            g_active.push_back(newDrop);
        }

        // 결정 178: 새 드랍을 만들었을 때만 상한을 맞춘다. 회차 밖의 오래된 드랍부터 회수한다.
        std::unordered_set<RE::FormID> attempted;
        for (;;) {
            ActiveWorldDrop candidate;
            {
                std::scoped_lock lock(g_dropMutex);
                if (g_active.size() <= g_config->worldDropMaxCount) {
                    break;
                }
                auto oldest = g_active.end();
                for (auto it = g_active.begin(); it != g_active.end(); ++it) {
                    if (it->dropRefFormId == newDrop.dropRefFormId || attempted.contains(it->dropRefFormId)) {
                        continue;
                    }
                    if (!MapDeviceRuntime::IsActiveMapCell(it->spawnCellId)) {
                        oldest = it;
                        break;
                    }
                    if (oldest == g_active.end()) {
                        oldest = it;
                    }
                }
                if (oldest == g_active.end()) {
                    break;
                }
                candidate = *oldest;
            }
            attempted.insert(candidate.dropRefFormId);
            const auto outcome = TryReturnSingleDropToCorpse(candidate);
            std::scoped_lock lock(g_dropMutex);
            const auto entry = std::ranges::find(g_active, candidate.dropRefFormId, &ActiveWorldDrop::dropRefFormId);
            if (entry != g_active.end()) {
                if (outcome == ReturnOutcome::RetryLater) {
                    entry->beamRefFormId = 0; // 회수는 미뤘지만 빛기둥은 이미 정리했다.
                } else {
                    g_active.erase(entry);
                }
            }
        }
        return true;
    }

    bool WorldDropTracker::OnKillAwarded(
        RE::ActorHandle corpseHandle,
        std::uint32_t effectiveMonsterLevel,
        bool isBoss,
        bool isDragon,
        Core::MonsterRarity monsterRarity,
        std::uint8_t activeMapTier,
        double bonusIiqPct,
        double bonusIirPct,
        bool guaranteeFirstTribute,
        bool deliverToPlayer)
    {
        auto corpsePtr = corpseHandle.get();
        auto* corpse = corpsePtr.get();
        auto* bagOwner = deliverToPlayer ? RE::PlayerCharacter::GetSingleton() : nullptr;
        // 회차를 끝낼 귀환으로 비활성화된 시체에 넣으면 회차 종료 정리로 사라지므로 닿을 수 없는 것으로 본다.
        if (!g_config || !g_config->enableLootDrops || !g_itemData || !g_itemData->IsLoaded() ||
            (deliverToPlayer ? !bagOwner : (!corpse || corpse->IsDeleted() || corpse->IsDisabled()))) {
            if (!guaranteeFirstTribute) {
                return false;
            }
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* tribute = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
            if (!player || !tribute) {
                return false;
            }
            const auto countBefore = CountItemInContainer(player, tribute);
            player->AddObjectToContainer(tribute, nullptr, 1, nullptr);
            return CountItemInContainer(player, tribute) > countBefore;
        }
        const auto gemGeneration = SaveLoad::WorldGemGeneration();
        std::vector<Core::DropEntry> drops;
        {
            std::scoped_lock lock(g_dropMutex);
            drops = Core::LootTable::RollKillDrops(
                effectiveMonsterLevel, isBoss, isDragon, *g_config, *g_itemData, g_rng, monsterRarity,
                activeMapTier, bonusIiqPct, bonusIirPct, guaranteeFirstTribute, gemGeneration);
        }
        bool tributeCreated = false;
        for (const auto& d : drops) {
            RE::TESObjectMISC* misc = nullptr;
            bool isCurr = false;
            bool isUniq = false;
            bool isMap = false;
            switch (d.kind) {
                case Core::DropKind::SealedToken:
                    misc = FormIds::Lookup<RE::TESObjectMISC>(
                        FormIds::SealedTokenLocalFormId(d.tier, d.rarity, d.slotGroup));
                    break;
                case Core::DropKind::CurrencyOrb:
                    isCurr = true;
                    misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CurrencyLocalFormId(d.currency));
                    break;
                case Core::DropKind::UniqueToken:
                    isUniq = true;
                    misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kUniqueSealedToken);
                    break;
                case Core::DropKind::Jewel:
                    misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(d.jewelColor));
                    break;
                case Core::DropKind::Charm:
                    misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::CharmLocalFormId(d.charm));
                    break;
                case Core::DropKind::SkillGem:
                    if (const auto localId = Core::GemForms::ActiveForm(gemGeneration, d.gemId); localId != 0) {
                        misc = FormIds::Lookup<RE::TESObjectMISC>(localId);
                    }
                    break;
                case Core::DropKind::MapItem:
                    isMap = true;
                    misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::MapItemLocalFormId(d.tier));
                    break;
                case Core::DropKind::AscendancyTribute:
                    isUniq = true;
                    misc = FormIds::Lookup<RE::TESObjectMISC>(FormIds::kAscendancy4Tribute);
                    break;
            }
            if (!misc) {
                continue;
            }

            const bool isTribute = d.kind == Core::DropKind::AscendancyTribute;
            RE::TESObjectREFR* container = bagOwner ? static_cast<RE::TESObjectREFR*>(bagOwner) : corpse;
            const auto countBefore = isTribute ? CountItemInContainer(container, misc) : 0;
            bool onGround = false;
            if (!bagOwner && g_config->worldDropEnabled && d.worldDropTier >= g_config->worldDropMinTier) {
                // 바닥 배출에 실패해도 시체에 남았을 수 있으므로 중복 추가하지 않는다.
                onGround = SpawnPhysicalDropFromContainer(corpse, misc, d.worldDropTier, isCurr, isUniq, isMap);
            } else {
                container->AddObjectToContainer(misc, nullptr, 1, nullptr);
            }
            if (isTribute) {
                tributeCreated = onGround || CountItemInContainer(container, misc) > countBefore;
            }
        }
        return tributeCreated;
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

        // 검수 m6 + 6단계: ForEachReference 스핀락 밖에서 네 빛기둥 FormID를 1회 캐싱
        const auto* rareBeam = FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamRare);
        const auto* currBeam = FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamCurrency);
        const auto* uniqBeam = FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamUnique);
        const auto* mapBeam = FormIds::Lookup<RE::TESObjectSTAT>(FormIds::kLootBeamMap);
        const RE::FormID rareBeamId = rareBeam ? rareBeam->GetFormID() : 0;
        const RE::FormID currBeamId = currBeam ? currBeam->GetFormID() : 0;
        const RE::FormID uniqBeamId = uniqBeam ? uniqBeam->GetFormID() : 0;
        const RE::FormID mapBeamId = mapBeam ? mapBeam->GetFormID() : 0;
        if (rareBeamId == 0 && currBeamId == 0 && uniqBeamId == 0 && mapBeamId == 0) {
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
                if (baseId != 0 && (baseId == rareBeamId || baseId == currBeamId || baseId == uniqBeamId || baseId == mapBeamId)) {
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
        const RE::FormID currentCellId = curCell ? curCell->GetFormID() : 0;
        const RE::FormID intCellId = isInterior ? currentCellId : 0;

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
        std::vector<RE::FormID> clearedBeamDropIds;
        for (const auto& d : snapshot) {
            auto* dropRef = ResolveDropRef(d);
            const auto* dropCell = dropRef ? dropRef->GetParentCell() : nullptr;

            // 포털로 나간 동안 내려간 회차 셀의 드랍은 조회되지 않아도 종료 정리까지 추적한다.
            if (!dropCell && currentCellId != d.spawnCellId && MapDeviceRuntime::IsActiveMapCell(d.spawnCellId)) {
                continue;
            }

            // 1. 주웠거나 사라짐 → 빛기둥 즉시 삭제 (Q-5: 셀 소속 상실 포함)
            if (!dropCell || dropRef->IsDeleted() || dropRef->IsDisabled()) {
                DeleteBeamMarker(d.beamRefFormId);
                toRemoveDropIds.push_back(d.dropRefFormId);
                continue;
            }

            // 2. 시간 초과·구역 이탈·플레이어 원거리 이탈(>4096u) → 시체로 회수
            // 결정 178: 활성 회차 셀의 드랍은 포털 왕복과 시간 초과에도 보존한다.
            const bool deferForActiveMap = MapDeviceRuntime::IsActiveMapCell(dropCell->GetFormID());
            const auto dp = dropRef->GetPosition();
            const float px = dp.x - playerPos.x;
            const float py = dp.y - playerPos.y;
            const float pz = dp.z - playerPos.z;
            const bool tooFar = player && ((px * px + py * py + pz * pz) > kFarReturnDistSq);
            if (!deferForActiveMap && (zoneChanged || tooFar || d.elapsedSec >= g_config->worldDropReturnSeconds)) {
                const auto outcome = TryReturnSingleDropToCorpse(d);
                if (outcome != ReturnOutcome::RetryLater ||
                    d.elapsedSec >= g_config->worldDropReturnSeconds * 2.0) {
                    toRemoveDropIds.push_back(d.dropRefFormId);
                } else if (d.beamRefFormId != 0) {
                    clearedBeamDropIds.push_back(d.dropRefFormId);
                }
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

        if (!toRemoveDropIds.empty() || !clearedBeamDropIds.empty()) {
            std::scoped_lock lock(g_dropMutex);
            for (auto& d : g_active) {
                if (std::find(clearedBeamDropIds.begin(), clearedBeamDropIds.end(), d.dropRefFormId) != clearedBeamDropIds.end()) {
                    d.beamRefFormId = 0;
                }
            }
            if (!toRemoveDropIds.empty()) {
                std::erase_if(g_active, [&](const ActiveWorldDrop& d) {
                    return std::find(toRemoveDropIds.begin(), toRemoveDropIds.end(), d.dropRefFormId) != toRemoveDropIds.end();
                });
            }
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

    WorldDropTracker::LegacyDropReturn WorldDropTracker::ReturnLegacyDrop(const SavedWorldDropEntry& entry)
    {
        if (entry.dropRefFormId == 0) {
            DeleteBeamMarker(entry.beamRefFormId);
            return LegacyDropReturn::Lost;
        }
        const ActiveWorldDrop drop{ .dropRefFormId = entry.dropRefFormId };
        const RE::NiPointer<RE::TESObjectREFR> ref{ ResolveDropRef(drop) };
        if (!ref || ref->IsDeleted() || ref->IsDisabled() || !ref->GetParentCell()) {
            DeleteBeamMarker(entry.beamRefFormId);
            return LegacyDropReturn::AlreadyGone;
        }
        const RE::NiPointer<RE::Actor> corpse{
            entry.corpseRefFormId ? RE::TESForm::LookupByID<RE::Actor>(entry.corpseRefFormId) : nullptr
        };
        if (entry.corpseRefFormId == 0 || (corpse && (corpse->IsDeleted() || corpse->IsDisabled() || !corpse->IsDead()))) {
            DeleteBeamMarker(entry.beamRefFormId);
            return LegacyDropReturn::Lost;
        }
        if (!corpse || !corpse->Is3DLoaded()) {
            return LegacyDropReturn::Deferred;
        }

        auto* base = ref->GetBaseObject();
        const auto count = std::max<std::int32_t>(1, ref->extraList.GetCount());
        const auto before = CountItemInContainer(corpse.get(), base);
        corpse->AddObjectToContainer(base, nullptr, count, nullptr);
        const auto after = CountItemInContainer(corpse.get(), base);
        const auto remaining = Core::RemainingLegacyDropCount(count, before, after);
        if (remaining != 0) {
            if (remaining < count) {
                try {
                    ref->extraList.SetCount(static_cast<std::uint16_t>(remaining));
                } catch (...) {
                    // Read and report the remaining stack even if the engine rejected the update.
                }
            }
            const auto actualRemaining = std::max<std::int32_t>(1, ref->extraList.GetCount());
            SKSE::log::warn("WorldDropTracker: legacy drop {:08X}, corpse {:08X}: expected increase {}, observed {}, "
                "remaining on ground {} (expected {}).", entry.dropRefFormId, entry.corpseRefFormId, count,
                static_cast<std::int64_t>(after) - before, actualRemaining, remaining);
            DeleteBeamMarker(entry.beamRefFormId);
            return LegacyDropReturn::Lost;
        }
        ref->Disable();
        ref->SetDelete(true);
        DeleteBeamMarker(entry.beamRefFormId);
        return LegacyDropReturn::Returned;
    }

    bool WorldDropTracker::DiscardLegacyRunDrop(const SavedWorldDropEntry& entry,
        std::span<const RE::FormID> corpseIds, RE::FormID runCellId, bool packOwnsCell)
    {
        return DiscardRunDrop({ .dropRefFormId = entry.dropRefFormId, .corpseRefFormId = entry.corpseRefFormId,
            .beamRefFormId = entry.beamRefFormId }, corpseIds, runCellId, packOwnsCell);
    }

    void WorldDropTracker::DiscardDropsFromCorpses(std::span<const RE::FormID> corpseIds, RE::FormID runCellId)
    {
        if (corpseIds.empty() && runCellId == 0) {
            return;
        }
        std::vector<ActiveWorldDrop> candidates;
        {
            std::scoped_lock lock(g_dropMutex);
            candidates = g_active;
        }
        std::vector<RE::FormID> finished;
        for (const auto& drop : candidates) {
            // 실제 삭제는 회차 셀 안으로 한정한다(FormID 재사용 방지).
            if (DiscardRunDrop(drop, corpseIds, runCellId, true)) {
                finished.push_back(drop.dropRefFormId);
            }
        }
        if (!finished.empty()) {
            std::scoped_lock lock(g_dropMutex);
            std::erase_if(g_active, [&](const ActiveWorldDrop& drop) {
                return std::ranges::find(finished, drop.dropRefFormId) != finished.end();
            });
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
                0,  // WDRP에는 생성 셀이 없으므로 상한 회수 때 셀 밖으로 취급한다.
                RE::ObjectRefHandle{},
                RE::ActorHandle{},
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
