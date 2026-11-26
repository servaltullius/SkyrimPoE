#include "test_harness.h"

#include "core/DataPack.h"
#include "core/GemTransaction.h"
#include "core/SaveCapPolicy.h"
#include "core/Types.h"
#include "core/WorldStampLink.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace SkyrimPoE::Core;
using namespace SkyrimPoE::Core::WorldStampLink;

namespace
{
    constexpr StampBand kGemBand{ 0x7F00, 0xBFFF };
    constexpr StampBand kItemBand{ 0xC000, 0xFFFD };
    constexpr std::uint32_t kPlayer = 0x00000014;
    constexpr std::uint32_t kGroundRefA = 0xFF00370A;
    constexpr std::uint32_t kGroundRefB = 0xFF00370B;
    constexpr std::uint32_t kFollower = 0x000B9986;

    struct SimTrackedGem
    {
        InventoryItemKey key;
        bool isWorld = false;
        GemInstance gem;
    };

    struct SimExtra
    {
        InventoryItemKey extraKey;
        std::string gemId;
        std::int32_t count = 1;
    };

    GemInstance MakeNestedCycloneGem(std::uint64_t id, std::uint8_t level = 17)
    {
        GemInstance gem;
        gem.instanceId = id;
        gem.gemId = "Cyclone";
        gem.level = level;
        gem.quality = 20;
        gem.xp = 123456;
        gem.socketCount = 4;

        GemInstance sup;
        sup.instanceId = id + 1000;
        sup.gemId = "SupportMeleePhysicalDamage";
        sup.level = 12;
        sup.quality = 7;
        gem.socketedGems.push_back(sup);
        return gem;
    }

    // 재검수 R-5: 다시 잇기 적용 순서는 제품 함수 WorldStampLink::ExecuteRelinks를 그대로 쓴다.
    // 이 테스트가 흉내 내는 것은 엔진 묶음(SimExtra)과 장부(SimTrackedGem) 접근뿐이다.
    class SimRelinkExecutor final : public IRelinkExecutor
    {
    public:
        std::vector<SimTrackedGem>* ledger = nullptr;
        std::vector<SimExtra>* ownerExtras = nullptr;
        std::uint16_t* nextUid = nullptr;
        bool mutateExtraAfterCommit = false;  // 커밋과 고쳐 쓰기 사이에 다른 이벤트가 묶음을 바꾼 상황
        bool writeIsNoOp = false;             // 제품이 묶음 고쳐 쓰기를 빠뜨렸을 때를 흉내(기능 제거 점검)
        bool mutateAtWrite = false;           // 고쳐 쓰는 바로 그 순간 다른 이벤트가 묶음을 바꾼 상황(Codex 지적 4)
        std::size_t pendingMutateIndex = SIZE_MAX;

        std::optional<InventoryItemKey> ReadExtraKey(std::size_t i) override
        {
            if (i >= ownerExtras->size()) {
                return std::nullopt;
            }
            if (pendingMutateIndex == i) {
                (*ownerExtras)[i].extraKey = InventoryItemKey{ kFollower, 0x7099 };
                pendingMutateIndex = SIZE_MAX;
            }
            return (*ownerExtras)[i].extraKey;
        }
        std::int32_t ExtraCount(std::size_t i) override { return i < ownerExtras->size() ? (*ownerExtras)[i].count : 0; }
        std::uint16_t AllocateInventoryUid() override { return (*nextUid)++; }
        bool CommitLedger(const PlannedRelink& p, const InventoryItemKey& newKey) override
        {
            for (auto& e : *ledger) {
                if (e.gem.instanceId == p.instanceId) {
                    const bool ok = CommitRelink(e.key, e.isWorld, p.worldKey, newKey);
                    if (ok && mutateExtraAfterCommit) {
                        pendingMutateIndex = p.extraIndex;
                    }
                    return ok;
                }
            }
            return false;
        }
        void RevertLedger(const PlannedRelink& p, const InventoryItemKey& newKey) override
        {
            for (auto& e : *ledger) {
                if (e.gem.instanceId == p.instanceId && !e.isWorld && e.key == newKey) {
                    e.key = p.worldKey;
                    e.isWorld = true;
                    break;
                }
            }
        }
        bool WriteExtraKey(std::size_t i, const InventoryItemKey& expectedKey, const InventoryItemKey& newKey) override
        {
            if (mutateAtWrite && i < ownerExtras->size()) {
                (*ownerExtras)[i].extraKey = InventoryItemKey{ kFollower, 0x7098 };
            }
            if (i >= ownerExtras->size() || (*ownerExtras)[i].extraKey != expectedKey) {
                return false;
            }
            if (!writeIsNoOp) {
                (*ownerExtras)[i].extraKey = newKey;
            }
            return true;  // writeIsNoOp: 제품이 쓰기를 빠뜨린 결함을 흉내(성공이라 보고하지만 실물은 그대로)
        }
    };

    std::size_t RunProductRelinkPass(
        std::vector<SimTrackedGem>& ledger,
        std::vector<SimExtra>& ownerExtras,
        std::uint32_t ownerId,
        std::uint16_t& nextUid,
        std::uint16_t onlyStamp = 0,
        bool simulateMutateExtraBeforeRewrite = false,
        bool skipExtraRewriteForMutationCheck = false,
        std::uint32_t postLoadNormalizedOwnerId = 0)
    {
        std::vector<WorldLinkCandidate> cands;
        std::vector<InventoryItemKey> heldKeys;
        for (const auto& e : ledger) {
            if (e.isWorld && e.key.IsValid() && kGemBand.Contains(e.key.uniqueId) &&
                (onlyStamp == 0 || e.key.uniqueId == onlyStamp)) {
                cands.push_back(WorldLinkCandidate{ e.gem.instanceId, e.key, e.gem.gemId });
            } else if (!e.isWorld && e.key.ownerFormId == ownerId) {
                heldKeys.push_back(e.key);
            }
        }
        std::vector<ObservedInventoryExtra> obs;
        for (std::size_t i = 0; i < ownerExtras.size(); ++i) {
            obs.push_back(ObservedInventoryExtra{ i, ownerExtras[i].extraKey, ownerExtras[i].gemId, ownerExtras[i].count });
        }
        const auto plan = PlanRelinks(cands, obs, kGemBand, postLoadNormalizedOwnerId, heldKeys);
        SimRelinkExecutor exec;
        exec.ledger = &ledger;
        exec.ownerExtras = &ownerExtras;
        exec.nextUid = &nextUid;
        exec.mutateExtraAfterCommit = simulateMutateExtraBeforeRewrite;
        exec.writeIsNoOp = skipExtraRewriteForMutationCheck;
        return ExecuteRelinks(plan, ownerId, exec);
    }

    // 실제 GemTransaction::EquipSkillGem과 연동되는 인벤토리 어댑터(장부 키와 실물 Extra 키 일치 검증)
    class LedgerBackedGemAdapter final : public IGemInventoryAdapter
    {
    public:
        std::vector<SimTrackedGem>* ledger = nullptr;
        std::vector<SimExtra>* ownerExtras = nullptr;
        std::uint32_t ownerId = kPlayer;
        std::uint16_t nextUid = 0x7100;

        std::optional<GemInstance> TakeGem(
            const InventoryItemKey& key,
            std::string_view /*fallbackGemId*/,
            std::uint8_t /*defaultSockets*/,
            std::uint64_t& /*nextInstanceId*/) override
        {
            if (!key.IsValid()) {
                return std::nullopt;
            }
            for (auto it = ledger->begin(); it != ledger->end(); ++it) {
                if (!it->isWorld && it->key == key) {
                    // 실물 묶음도 정확히 같은 키를 들고 있어야만 회수 성공(고쳐 쓰기 누락 탐지!)
                    auto exIt = std::find_if(ownerExtras->begin(), ownerExtras->end(), [&](const SimExtra& ex) {
                        return ex.extraKey == key && ex.gemId == it->gem.gemId;
                    });
                    if (exIt == ownerExtras->end()) {
                        return std::nullopt;
                    }
                    GemInstance out = std::move(it->gem);
                    ledger->erase(it);
                    ownerExtras->erase(exIt);
                    return out;
                }
            }
            return std::nullopt;
        }

        std::optional<InventoryItemKey> ReturnGem(GemInstance gem) override
        {
            const InventoryItemKey key{ ownerId, nextUid++ };
            ownerExtras->push_back(SimExtra{ key, gem.gemId, 1 });
            ledger->push_back(SimTrackedGem{ key, false, std::move(gem) });
            return key;
        }
    };
}

POE_TEST("WorldStampLink: stamp band, drop, pickup, and deferred-stamp guard")
{
    POE_CHECK(!kGemBand.Contains(0));
    POE_CHECK(kGemBand.Contains(0x7F00));
    POE_CHECK(kGemBand.Contains(0xBFFF));
    POE_CHECK(!kGemBand.Contains(0x7EFF));
    POE_CHECK(!kGemBand.Contains(0xC000));

    POE_CHECK(NeedsWorldStamp(kGroundRefA, 0));
    POE_CHECK(!NeedsWorldStamp(kPlayer, 0x7001));
    POE_CHECK(!NeedsWorldStamp(0, 0));

    // itemCount <= 0, 컨테이너간 이동, 대역 밖, 스탬프 0 모두 제외
    POE_CHECK(IsStampedWorldPickup(0, kPlayer, 0x7F05, 1, kGemBand));
    POE_CHECK(!IsStampedWorldPickup(0, kPlayer, 0x7F05, 0, kGemBand));
    POE_CHECK(!IsStampedWorldPickup(0, kPlayer, 0x7F05, -1, kGemBand));
    POE_CHECK(!IsStampedWorldPickup(kFollower, kPlayer, 0x7F05, 1, kGemBand));
    POE_CHECK(!IsStampedWorldPickup(kPlayer, 0, 0x7F05, 1, kGemBand));
    POE_CHECK(!IsStampedWorldPickup(0, kPlayer, 0, 1, kGemBand));
    POE_CHECK(!IsStampedWorldPickup(0, kPlayer, 0xC001, 1, kGemBand));
    POE_CHECK(IsStampedWorldPickup(0, kPlayer, 0xC001, 1, kItemBand));

    // 보완 스탬프 실행 전 재검증(MatchesWorldEntry)
    POE_CHECK(MatchesWorldEntry(InventoryItemKey{ kGroundRefA, 0x7F05 }, true, kGroundRefA, 0x7F05, kGemBand));
    POE_CHECK(!MatchesWorldEntry(InventoryItemKey{ kPlayer, 0x7001 }, false, kGroundRefA, 0x7F05, kGemBand));
    POE_CHECK(!MatchesWorldEntry(InventoryItemKey{ kGroundRefB, 0x7F06 }, true, kGroundRefA, 0x7F05, kGemBand));
}

POE_TEST("WorldStampLink: Codex negative test - world gem X untouched when player holds same stamp on gem Y")
{
    // 젬 X는 바닥(kGroundRefA:0x7F10)에 있고, 같은 종류의 젬 Y가 플레이어 인벤토리에 (kPlayer:0x7F10)을 달고 있음
    std::vector<SimTrackedGem> ledger;
    ledger.push_back(SimTrackedGem{ InventoryItemKey{ kGroundRefA, 0x7F10 }, true, MakeNestedCycloneGem(10, 18) });
    ledger.push_back(SimTrackedGem{ InventoryItemKey{ kPlayer, 0x7F10 }, false, MakeNestedCycloneGem(20, 5) });

    std::vector<SimExtra> playerExtras;
    playerExtras.push_back(SimExtra{ InventoryItemKey{ kPlayer, 0x7F10 }, "Cyclone", 1 });

    std::uint16_t nextUid = 0x7010;
    const auto linked = RunProductRelinkPass(ledger, playerExtras, kPlayer, nextUid);
    POE_CHECK(linked == 0);
    // X(바닥)와 Y(플레이어) 모두 전혀 변하지 않아야 한다((소유자, 스탬프) 2순위 폴백 없음 검증)
    POE_CHECK(ledger[0].isWorld && (ledger[0].key == InventoryItemKey{ kGroundRefA, 0x7F10 }) && ledger[0].gem.level == 18);
    POE_CHECK(!ledger[1].isWorld && (ledger[1].key == InventoryItemKey{ kPlayer, 0x7F10 }) && ledger[1].gem.level == 5);
    POE_CHECK((playerExtras[0].extraKey == InventoryItemKey{ kPlayer, 0x7F10 }));
}

POE_TEST("WorldStampLink: save-boundary restore relinks once and re-equips via real GemTransaction preserving supports")
{
    DataPack pack;
    std::string err;
    const auto packPath = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "gen0" / "core.json";
    POE_CHECK(pack.LoadFromFile(packPath, &err));

    // 1. 젬 X를 바닥에 버려 (kGroundRefA:0x7F22) 스탬프 기록 후, 주운 직후(재바인딩 전) 저장·로드 경계 시뮬레이션
    const GemInstance orig = MakeNestedCycloneGem(42, 17);
    std::vector<SimTrackedGem> savedLedger;
    savedLedger.push_back(SimTrackedGem{ InventoryItemKey{ kGroundRefA, 0x7F22 }, true, orig });
    // 같은 종류의 다른 젬이 다른 바닥 참조에 남아 있음
    savedLedger.push_back(SimTrackedGem{ InventoryItemKey{ kGroundRefB, 0x7F23 }, true, MakeNestedCycloneGem(77, 3) });

    // 로드 후 복원된 상태 + 플레이어 인벤토리에 (kGroundRefA:0x7F22) 묶음이 그대로 들어 있음
    auto restoredLedger = savedLedger;
    std::vector<SimExtra> playerExtras;
    playerExtras.push_back(SimExtra{ InventoryItemKey{ kGroundRefA, 0x7F22 }, "Cyclone", 1 });

    // 로드 직후 보완 경로 실행
    std::uint16_t nextUid = 0x7020;
    const auto linked = RunProductRelinkPass(restoredLedger, playerExtras, kPlayer, nextUid);
    POE_CHECK(linked == 1);

    // 같은 instanceId가 목록 조건(!isWorld && owner==kPlayer)에 정확히 1회만 등장
    std::size_t listedCount = 0;
    InventoryItemKey relinkedKey{};
    for (const auto& e : restoredLedger) {
        if (!e.isWorld && e.key.ownerFormId == kPlayer && e.key.IsValid()) {
            ++listedCount;
            POE_CHECK(e.gem.instanceId == orig.instanceId);
            relinkedKey = e.key;
        }
    }
    POE_CHECK(listedCount == 1 && relinkedKey.IsValid());
    // 두 번째 보완 패스는 멱등(0건)
    POE_CHECK(RunProductRelinkPass(restoredLedger, playerExtras, kPlayer, nextUid) == 0);
    // 바닥의 다른 젬은 그대로 바닥에 유지
    POE_CHECK(restoredLedger[1].isWorld && (restoredLedger[1].key == InventoryItemKey{ kGroundRefB, 0x7F23 }));

    // 실제 GemTransaction::EquipSkillGem으로 재장착해 레벨·품질·중첩 보조 젬 보존 검증
    std::array<SkillSlot, kSkillSlotCount> slots{};
    std::uint64_t nextInstId = 100;
    LedgerBackedGemAdapter adapter;
    adapter.ledger = &restoredLedger;
    adapter.ownerExtras = &playerExtras;
    const auto eq = GemTransaction::EquipSkillGem(slots, nextInstId, 0, relinkedKey, "Cyclone", 3, pack, adapter);
    POE_CHECK(eq.ok);
    POE_CHECK(slots[0].occupied);
    POE_CHECK(slots[0].gem.instanceId == orig.instanceId);
    POE_CHECK(slots[0].gem.level == 17 && slots[0].gem.quality == 20 && slots[0].gem.xp == 123456);
    POE_CHECK(slots[0].gem.socketCount == 4);
    POE_CHECK(slots[0].gem.socketedGems.size() == 1);
    POE_CHECK(slots[0].gem.socketedGems[0].gemId == "SupportMeleePhysicalDamage");
    POE_CHECK(slots[0].gem.socketedGems[0].level == 12 && slots[0].gem.socketedGems[0].quality == 7);

    // 기능 제거 점검: 묶음 고쳐 쓰기를 건너뛰면 실제 EquipSkillGem이 실패함을 증명
    auto brokenLedger = savedLedger;
    std::vector<SimExtra> brokenExtras{ SimExtra{ InventoryItemKey{ kGroundRefA, 0x7F22 }, "Cyclone", 1 } };
    std::uint16_t brokenUid = 0x7050;
    POE_CHECK(RunProductRelinkPass(brokenLedger, brokenExtras, kPlayer, brokenUid, 0, false, true) == 1);
    std::array<SkillSlot, kSkillSlotCount> brokenSlots{};
    LedgerBackedGemAdapter brokenAdapter;
    brokenAdapter.ledger = &brokenLedger;
    brokenAdapter.ownerExtras = &brokenExtras;
    const auto brokenEq = GemTransaction::EquipSkillGem(
        brokenSlots, nextInstId, 0, brokenLedger[0].key, "Cyclone", 3, pack, brokenAdapter);
    POE_CHECK(!brokenEq.ok);
}

POE_TEST("WorldStampLink: duplicate extras, count>1, ref FormID reuse, redrop, and commit/pre-rewrite guard")
{
    // 1. 동일 후보에 정확히 맞는 묶음이 2개(동등 후보)이면 둘 다 보류하고 건드리지 않음
    {
        std::vector<WorldLinkCandidate> cands{
            WorldLinkCandidate{ 1, InventoryItemKey{ kGroundRefA, 0x7F30 }, "Cyclone" }
        };
        std::vector<ObservedInventoryExtra> obs{
            ObservedInventoryExtra{ 0, InventoryItemKey{ kGroundRefA, 0x7F30 }, "Cyclone", 1 },
            ObservedInventoryExtra{ 1, InventoryItemKey{ kGroundRefA, 0x7F30 }, "Cyclone", 1 },
        };
        const auto plan = PlanRelinks(cands, obs, kGemBand);
        POE_CHECK(plan.planned.empty());
        POE_CHECK(plan.heldAmbiguous.size() == 2);
    }

    // 2. 묶음 수량이 1보다 크면 보류
    {
        std::vector<WorldLinkCandidate> cands{
            WorldLinkCandidate{ 1, InventoryItemKey{ kGroundRefA, 0x7F31 }, "Cyclone" }
        };
        std::vector<ObservedInventoryExtra> obs{
            ObservedInventoryExtra{ 0, InventoryItemKey{ kGroundRefA, 0x7F31 }, "Cyclone", 2 }
        };
        const auto plan = PlanRelinks(cands, obs, kGemBand);
        POE_CHECK(plan.planned.empty());
        POE_CHECK(plan.heldCountGtOne.size() == 1);
    }

    // 3. 바닥 참조 FormID 재사용: 같은 kGroundRefA라도 스탬프가 다르면 정확히 구분
    {
        std::vector<WorldLinkCandidate> cands{
            WorldLinkCandidate{ 1, InventoryItemKey{ kGroundRefA, 0x7F40 }, "Cyclone" },
            WorldLinkCandidate{ 2, InventoryItemKey{ kGroundRefA, 0x7F41 }, "Cyclone" },
        };
        const auto* t1 = FindWorldLinkTarget(cands, InventoryItemKey{ kGroundRefA, 0x7F40 }, "Cyclone", kGemBand);
        const auto* t2 = FindWorldLinkTarget(cands, InventoryItemKey{ kGroundRefA, 0x7F41 }, "Cyclone", kGemBand);
        POE_CHECK(t1 && t1->instanceId == 1);
        POE_CHECK(t2 && t2->instanceId == 2);
        POE_CHECK(FindWorldLinkTarget(cands, InventoryItemKey{ kGroundRefA, 0x7F42 }, "Cyclone", kGemBand) == nullptr);
    }

    // 4. 다시 잇기 전 재드랍(isWorld=true 상태에서 새 바닥 참조:0 도착): NeedsWorldStamp가 참
    {
        SimTrackedGem pending{ InventoryItemKey{ kGroundRefA, 0x7F45 }, true, MakeNestedCycloneGem(9, 10) };
        POE_CHECK(pending.isWorld);
        POE_CHECK(NeedsWorldStamp(kGroundRefB, 0));
        pending.key = InventoryItemKey{ kGroundRefB, 0x7F46 };
        // 옛 예약(kGroundRefA:0x7F45)의 커밋은 거부돼야 함
        POE_CHECK(!CommitRelink(pending.key, pending.isWorld, InventoryItemKey{ kGroundRefA, 0x7F45 }, InventoryItemKey{ kPlayer, 0x7060 }));
        POE_CHECK(pending.isWorld && (pending.key == InventoryItemKey{ kGroundRefB, 0x7F46 }));
    }

    // 5. 고쳐 쓰기 직전 묶음 키가 바뀌어 있으면 장부 키를 원복하고 엔진 묶음을 덮어쓰지 않음(Codex A5)
    {
        std::vector<SimTrackedGem> ledger{
            SimTrackedGem{ InventoryItemKey{ kGroundRefA, 0x7F50 }, true, MakeNestedCycloneGem(50, 14) }
        };
        std::vector<SimExtra> extras{
            SimExtra{ InventoryItemKey{ kGroundRefA, 0x7F50 }, "Cyclone", 1 }
        };
        std::uint16_t nextUid = 0x7070;
        const auto linked = RunProductRelinkPass(ledger, extras, kPlayer, nextUid, 0, true, false);
        POE_CHECK(linked == 0);
        POE_CHECK(ledger[0].isWorld && (ledger[0].key == InventoryItemKey{ kGroundRefA, 0x7F50 }));
        POE_CHECK((extras[0].extraKey == InventoryItemKey{ kFollower, 0x7099 }));
    }

    // 6. 실게임 AE 1.6.1170 저장 경계(.ess 로더가 삭제된 FF 바닥 참조 baseID를 0x14로 정규화, 스탬프 uniqueID 보존):
    //    일반 런타임(postLoadNormalizedOwnerId=0)에서는 거부하고, PostLoadInit(postLoadNormalizedOwnerId=kPlayer)에서만
    //    단일 1:1 매칭을 허용하며 중복/count>1은 계속 보류한다.
    {
        std::vector<WorldLinkCandidate> cands{
            WorldLinkCandidate{ 12, InventoryItemKey{ kGroundRefA, 0x7F03 }, "Fireball" }
        };
        std::vector<ObservedInventoryExtra> obs{
            ObservedInventoryExtra{ 0, InventoryItemKey{ kPlayer, 0x7F03 }, "Fireball", 1 }
        };
        // 일반 런타임에서는 거부
        const auto runtimePlan = PlanRelinks(cands, obs, kGemBand, 0);
        POE_CHECK(runtimePlan.planned.empty());
        // PostLoadInit에서는 1건 연결 + observedExtraKey=(kPlayer, 0x7F03), worldKey=(kGroundRefA, 0x7F03)
        const auto postLoadPlan = PlanRelinks(cands, obs, kGemBand, kPlayer);
        POE_CHECK(postLoadPlan.planned.size() == 1);
        POE_CHECK(postLoadPlan.planned[0].instanceId == 12);
        POE_CHECK((postLoadPlan.planned[0].worldKey == InventoryItemKey{ kGroundRefA, 0x7F03 }));
        POE_CHECK((postLoadPlan.planned[0].observedExtraKey == InventoryItemKey{ kPlayer, 0x7F03 }));
        // count > 1이면 PostLoadInit에서도 보류
        obs[0].count = 2;
        const auto countPlan = PlanRelinks(cands, obs, kGemBand, kPlayer);
        POE_CHECK(countPlan.planned.empty() && countPlan.heldCountGtOne.size() == 1);
        // 같은 스탬프·종류 후보가 둘이면 PostLoadInit에서도 모호성 보류
        obs[0].count = 1;
        cands.push_back(WorldLinkCandidate{ 13, InventoryItemKey{ kGroundRefB, 0x7F03 }, "Fireball" });
        const auto ambigPlan = PlanRelinks(cands, obs, kGemBand, kPlayer);
        POE_CHECK(ambigPlan.planned.empty() && ambigPlan.heldAmbiguous.size() == 1);
    }
}

POE_TEST("SaveCapPolicy: over the cap, keep player-held first, then world, then others; newest within a rank (review R-3)")
{
    constexpr std::uint32_t kMerchant = 0xFF000123;
    constexpr std::uint32_t kGroundRef = 0xFF003115;
    // 0: 오래전에 얻어 지금도 가진 장비, 1~3: 판 물건, 4: 바닥, 5: 최근에 얻어 가진 것, 6: 최근에 판 것
    const std::vector<SaveCapCandidate> entries = {
        { kPlayer, false }, { kMerchant, false }, { kMerchant, false }, { kMerchant, false },
        { kGroundRef, true }, { kPlayer, false }, { kMerchant, false },
    };
    // 상한 안이면 전부 원래 순서로
    POE_CHECK((SelectEntriesWithinSaveCap(entries, 7, kPlayer) == std::vector<std::size_t>{ 0, 1, 2, 3, 4, 5, 6 }));
    // 상한 3: 플레이어 소지품 둘(0, 5) + 바닥(4). 예전 "최신 우선"이면 0이 빠졌다.
    POE_CHECK((SelectEntriesWithinSaveCap(entries, 3, kPlayer) == std::vector<std::size_t>{ 0, 4, 5 }));
    // 상한 4: 그 밖 소유자 중 최신(6)이 하나 더 남는다.
    POE_CHECK((SelectEntriesWithinSaveCap(entries, 4, kPlayer) == std::vector<std::size_t>{ 0, 4, 5, 6 }));
    // 상한 1: 플레이어 소지품 중 최신
    POE_CHECK((SelectEntriesWithinSaveCap(entries, 1, kPlayer) == std::vector<std::size_t>{ 5 }));
    POE_CHECK(SelectEntriesWithinSaveCap(entries, 0, kPlayer).empty());
}

POE_TEST("WorldStampLink: post-load owner-normalized match never takes another tracked gem's key (review R-4)")
{
    // 바닥 젬 X(스탬프 0x7F10)와, 장부상 플레이어가 가진 같은 종류 젬 Y의 키가 (플레이어, 0x7F10)으로 같은 경우.
    // 로드 직후(소유자 정규화 대조 켬)에도 X의 데이터가 Y의 묶음에 붙으면 안 된다.
    std::vector<SimTrackedGem> ledger;
    ledger.push_back(SimTrackedGem{ InventoryItemKey{ kGroundRefA, 0x7F10 }, true, MakeNestedCycloneGem(10) });
    ledger.push_back(SimTrackedGem{ InventoryItemKey{ kPlayer, 0x7F10 }, false, MakeNestedCycloneGem(20, 5) });
    std::vector<SimExtra> extras = { SimExtra{ InventoryItemKey{ kPlayer, 0x7F10 }, "Cyclone", 1 } };
    std::uint16_t nextUid = 0x7200;
    POE_CHECK(RunProductRelinkPass(ledger, extras, kPlayer, nextUid, 0, false, false, kPlayer) == 0);
    POE_CHECK(ledger[0].isWorld && ledger[0].key == (InventoryItemKey{ kGroundRefA, 0x7F10 }));
    POE_CHECK(!ledger[1].isWorld && ledger[1].key == (InventoryItemKey{ kPlayer, 0x7F10 }));
    POE_CHECK(extras[0].extraKey == (InventoryItemKey{ kPlayer, 0x7F10 }));

    // 같은 묶음이 장부 어디에도 없으면(정규화된 바닥 젬 X 자신) 로드 직후 경로가 잇는다.
    std::vector<SimTrackedGem> ledger2;
    ledger2.push_back(SimTrackedGem{ InventoryItemKey{ kGroundRefA, 0x7F11 }, true, MakeNestedCycloneGem(30) });
    std::vector<SimExtra> extras2 = { SimExtra{ InventoryItemKey{ kPlayer, 0x7F11 }, "Cyclone", 1 } };
    std::uint16_t nextUid2 = 0x7300;
    POE_CHECK(RunProductRelinkPass(ledger2, extras2, kPlayer, nextUid2, 0, false, false, kPlayer) == 1);
    POE_CHECK(!ledger2[0].isWorld && ledger2[0].key == (InventoryItemKey{ kPlayer, 0x7300 }));
    POE_CHECK(extras2[0].extraKey == (InventoryItemKey{ kPlayer, 0x7300 }));
    // 일반 런타임(정규화 대조 끔)에서는 같은 입력을 잇지 않는다.
    std::vector<SimTrackedGem> ledger3;
    ledger3.push_back(SimTrackedGem{ InventoryItemKey{ kGroundRefA, 0x7F12 }, true, MakeNestedCycloneGem(40) });
    std::vector<SimExtra> extras3 = { SimExtra{ InventoryItemKey{ kPlayer, 0x7F12 }, "Cyclone", 1 } };
    std::uint16_t nextUid3 = 0x7400;
    POE_CHECK(RunProductRelinkPass(ledger3, extras3, kPlayer, nextUid3) == 0);
    POE_CHECK(ledger3[0].isWorld);
}

POE_TEST("WorldStampLink: ExecuteRelinks counts only real rewrites and reverts the ledger when the write-time check fails (Codex 4)")
{
    std::vector<SimTrackedGem> ledger;
    ledger.push_back(SimTrackedGem{ InventoryItemKey{ kGroundRefA, 0x7F20 }, true, MakeNestedCycloneGem(50) });
    std::vector<SimExtra> extras = { SimExtra{ InventoryItemKey{ kGroundRefA, 0x7F20 }, "Cyclone", 1 } };
    std::vector<WorldLinkCandidate> cands = { WorldLinkCandidate{ 50, ledger[0].key, "Cyclone" } };
    std::vector<ObservedInventoryExtra> obs = { ObservedInventoryExtra{ 0, extras[0].extraKey, "Cyclone", 1 } };
    const auto plan = PlanRelinks(cands, obs, kGemBand);
    POE_CHECK(plan.planned.size() == 1);

    std::uint16_t nextUid = 0x7500;
    SimRelinkExecutor exec;
    exec.ledger = &ledger;
    exec.ownerExtras = &extras;
    exec.nextUid = &nextUid;
    exec.mutateAtWrite = true;
    POE_CHECK(ExecuteRelinks(plan, kPlayer, exec) == 0);
    // 장부는 바닥 키로 되돌아가고, 묶음은 다른 이벤트가 바꾼 값 그대로다(우리 번호로 덮지 않음).
    POE_CHECK(ledger[0].isWorld && ledger[0].key == (InventoryItemKey{ kGroundRefA, 0x7F20 }));
    POE_CHECK(extras[0].extraKey == (InventoryItemKey{ kFollower, 0x7098 }));

    // 개수가 1이 아닌 묶음(0 포함)은 건드리지 않는다.
    std::vector<SimTrackedGem> ledger2;
    ledger2.push_back(SimTrackedGem{ InventoryItemKey{ kGroundRefA, 0x7F21 }, true, MakeNestedCycloneGem(60) });
    std::vector<SimExtra> extras2 = { SimExtra{ InventoryItemKey{ kGroundRefA, 0x7F21 }, "Cyclone", 0 } };
    std::uint16_t nextUid2 = 0x7600;
    POE_CHECK(RunProductRelinkPass(ledger2, extras2, kPlayer, nextUid2) == 0);
    POE_CHECK(ledger2[0].isWorld);
}

POE_TEST("WorldStampLink: decision 235 picks only untracked numbered gems in generation one and later")
{
    // 정리 뒤 실물에만 남은 PoE1 번호(가방 0x7012, 상자에서 가져온 0x7030, 바닥 스탬프 0x7F40)와
    // 세대 1의 추적 젬(가방 0x7001), 주운 뒤 다시 잇기를 기다리는 세대 1 바닥 젬(장부는 바닥 참조, 묶음은 소유자 정규화).
    const std::vector<ObservedInventoryExtra> extras = {
        ObservedInventoryExtra{ 0, InventoryItemKey{ kPlayer, 0x7012 }, "gem.Spark", 1 },
        ObservedInventoryExtra{ 1, InventoryItemKey{ kPlayer, 0x7001 }, "gem.Spark", 1 },
        ObservedInventoryExtra{ 2, InventoryItemKey{ kPlayer, 0x7F21 }, "gem.Spark", 1 },
        ObservedInventoryExtra{ 3, InventoryItemKey{ kPlayer, 0x7030 }, "SupportChain", 2 },
        ObservedInventoryExtra{ 4, InventoryItemKey{ kPlayer, 0x7F40 }, "ShockNova", 1 },
        ObservedInventoryExtra{ 5, InventoryItemKey{ kPlayer, 0 }, "gem.Spark", 1 },
        ObservedInventoryExtra{ 6, InventoryItemKey{ kPlayer, 0x7044 }, "gem.Spark", 0 },
    };
    const std::vector<InventoryItemKey> ledger = {
        InventoryItemKey{ kPlayer, 0x7001 },
        InventoryItemKey{ kGroundRefA, 0x7F21 },
    };

    const auto picked = PlanOrphanNormalization(1, extras, ledger);
    POE_CHECK((picked == std::vector<std::size_t>{ 0, 3, 4 }));

    // 세대 0은 그대로 둔다.
    POE_CHECK(PlanOrphanNormalization(0, extras, ledger).empty());
    // 장부가 비어 있으면(정리 직후) 번호가 있고 개수가 양수인 묶음을 모두 고른다.
    POE_CHECK((PlanOrphanNormalization(1, extras, {}) == std::vector<std::size_t>{ 0, 1, 2, 3, 4 }));
    // 같은 번호를 다른 소유자 키로 쓰는 장부 항목도 지킨다(번호가 겹치면 보이지 않는 쪽으로 남긴다).
    POE_CHECK((PlanOrphanNormalization(1, extras, { InventoryItemKey{ kFollower, 0x7012 } }) == std::vector<std::size_t>{ 1, 2, 3, 4 }));

    // 같은 종류·같은 키의 묶음 둘은 번호로 다시 찾을 때 서로를 가리므로 둘 다 고르지 않는다(Codex 지적).
    // 소유자 0의 비정상 키도 고르지 않는다. 종류가 다르면 같은 키라도 따로 다룬다.
    const std::vector<ObservedInventoryExtra> tricky = {
        ObservedInventoryExtra{ 0, InventoryItemKey{ kPlayer, 0x7050 }, "gem.Spark", 1 },
        ObservedInventoryExtra{ 1, InventoryItemKey{ kPlayer, 0x7050 }, "gem.Spark", 1 },
        ObservedInventoryExtra{ 2, InventoryItemKey{ 0, 0x7051 }, "gem.Spark", 1 },
        ObservedInventoryExtra{ 3, InventoryItemKey{ kPlayer, 0x7052 }, "gem.Fireball", 1 },
        ObservedInventoryExtra{ 4, InventoryItemKey{ kPlayer, 0x7052 }, "gem.Spark", 1 },
    };
    POE_CHECK((PlanOrphanNormalization(1, tricky, {}) == std::vector<std::size_t>{ 3, 4 }));
}
