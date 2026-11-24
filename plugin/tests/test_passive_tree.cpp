#include "test_harness.h"

#include "core/PassiveTree.h"
#include "core/PassiveTreeData.h"

#include <algorithm>
#include <filesystem>

namespace
{
    using namespace SkyrimPoE::Core;

    const PassiveTreeData& LoadRealTree()
    {
        static PassiveTreeData tree;
        static bool loaded = false;
        if (!loaded) {
            std::string err;
            const std::filesystem::path p = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "tree_3_30.json";
            POE_CHECK(tree.LoadFromFile(p, &err));
            loaded = true;
        }
        return tree;
    }

    // 지정 노드가 할당될 때까지 최단 경로를 구해 일괄 할당하는 테스트 도우미
    bool AllocateTo(
        const PassiveTreeData& tree,
        PassiveTreeState& state,
        std::uint32_t targetNodeId,
        std::uint32_t exileLevel,
        const Config& cfg)
    {
        const auto path = PassiveTree::ComputeShortestPath(tree, state, targetNodeId);
        if (path.empty()) {
            return false;
        }
        return PassiveTree::AllocatePath(tree, state, path, exileLevel, cfg).ok;
    }
}

POE_TEST("PassiveTreeData: loads real tree_3_30.json with 7 classes, 19 ascendancies, 2678 nodes, 353 masteries")
{
    const auto& tree = LoadRealTree();
    POE_CHECK(tree.IsLoaded());
    POE_CHECK(tree.Classes().size() == 7);
    POE_CHECK(tree.NodeCount() == 2678);
    POE_CHECK(tree.MasteryEffectCount() == 353);

    std::size_t totalAsc = 0;
    for (const auto& cls : tree.Classes()) {
        totalAsc += cls.ascendancies.size();
        POE_CHECK(!cls.startNeighbors.empty());
    }
    POE_CHECK(totalAsc == 19);

    // Ranger의 Raider ID -> 표시명 Warden (3.23 리그 Warden of the Maji 제외 확인)
    const auto* warden = tree.FindAscendancy("Raider");
    POE_CHECK(warden != nullptr);
    POE_CHECK(warden->name == "Warden");
    POE_CHECK(warden->classId == 2);
    POE_CHECK(tree.FindAscendancy("Warden") == nullptr);
    POE_CHECK(tree.FindAscendancy("Reliquarian") == nullptr);
    POE_CHECK(tree.FindAscendancy("Luminary") == nullptr);

    // Scion 시작점(58833)은 in-only 간선 복원으로 6개 인접 노드 보유
    const auto* scion = tree.FindClass(0);
    POE_CHECK(scion != nullptr);
    POE_CHECK(scion->startNodeId == 58833);
    POE_CHECK(scion->startNeighbors.size() == 6);

    // PassiveTreeData 복사본에서 FindAscendancy가 댕글링 포인터 없이 동작(검수 Major #3)
    PassiveTreeData treeCopy = tree;
    const auto* copiedWarden = treeCopy.FindAscendancy("Raider");
    POE_CHECK(copiedWarden != nullptr && copiedWarden->name == "Warden");
}

POE_TEST("PassiveTree: class selection, start neighbors, cross-class start block, signed int32 point math")
{
    const auto& tree = LoadRealTree();
    Config cfg;
    PassiveTreeState state;

    // 클래스 미선택 시 할당 차단
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, { 31628 }, 10, cfg).ok);

    // Marauder(1) 선택
    POE_CHECK(PassiveTree::SelectClass(tree, state, 1).ok);
    POE_CHECK(state.classSelected && state.classId == 1);

    // 레벨 5 -> 4 포인트
    POE_CHECK(PassiveTree::ComputeAvailableNormalPoints(tree, state, 5, cfg) == 4);

    // Marauder 시작 인접 31628 할당
    POE_CHECK(PassiveTree::AllocatePath(tree, state, { 31628 }, 5, cfg).ok);
    POE_CHECK(PassiveTree::ComputeSpentNormalPoints(tree, state) == 1);
    POE_CHECK(PassiveTree::ComputeAvailableNormalPoints(tree, state, 5, cfg) == 3);

    // 노드가 1개라도 할당된 상태에서는 클래스 변경 불가
    POE_CHECK(!PassiveTree::SelectClass(tree, state, 2).ok);

    // 타 클래스 시작점(Duelist 50986)과 타 클래스 시작 인접(39725)은 직접 할당 불가
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, { 50986 }, 100, cfg).ok);
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, { 39725 }, 100, cfg).ok);
    POE_CHECK(PassiveTree::ComputeShortestPath(tree, state, 50986).empty());

    // 포인트 부족 시 일괄 할당 원자적 거절(상태 불변)
    const auto beforeSize = state.allocatedNodes.size();
    const auto farPath = PassiveTree::ComputeShortestPath(tree, state, 11455);  // Chaos Inoculation
    POE_CHECK(farPath.size() > 10);
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, farPath, 5, cfg).ok);
    POE_CHECK(state.allocatedNodes.size() == beforeSize);

    // 보너스 포인트 하향 시 부호 있는 int32 음수 잔액 확인(언더플로 금지)
    // 검수 Critical #1 (C7): 음수 잔액에서 신규 할당은 거절하되, 일반 리프 노드 개별 회수와 부여 포인트 없는 전직 회수는 허용
    cfg.bonusPassivePoints = 5;
    POE_CHECK(PassiveTree::AllocatePath(tree, state, { 50904 }, 1, cfg).ok);  // 31628 + 50904 = 2 spent
    POE_CHECK(PassiveTree::ComputeAvailableNormalPoints(tree, state, 1, cfg) == 3);
    cfg.bonusAscendancyPoints = 2;  // 결정 166: 이정표 비트는 일반 패시브 +2점도 주므로 순수 전직 포인트만 부여해 음수 잔액 검증
    POE_CHECK(PassiveTree::SelectAscendancy(tree, state, "Juggernaut", cfg).ok);
    cfg.bonusPassivePoints = 0;
    POE_CHECK(PassiveTree::ComputeAvailableNormalPoints(tree, state, 1, cfg) == -2);
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, { tree.FindNode(31628)->adjacency.front() }, 1, cfg).ok);
    POE_CHECK(PassiveTree::RefundNode(tree, state, 50904, 1, cfg).ok);  // -2 -> -1 허용!
    POE_CHECK(PassiveTree::ComputeAvailableNormalPoints(tree, state, 1, cfg) == -1);
    POE_CHECK(PassiveTree::RefundAll(tree, state, true, 1, cfg).ok);   // 부여 포인트 0 전직 회수 허용!
    POE_CHECK(state.ascendancyId.empty());

    // 모두 회수 후 클래스 재선택 가능
    POE_CHECK(PassiveTree::RefundAll(tree, state, false, 1, cfg).ok);
    POE_CHECK(PassiveTree::SelectClass(tree, state, 6).ok);  // Shadow
    POE_CHECK(state.classId == 6);
}

POE_TEST("PassiveTree: 0-1 BFS normal shortest path treats allocated nodes as cost 0 and unallocated seeds as cost 1")
{
    const auto& tree = LoadRealTree();
    Config cfg;
    PassiveTreeState state;
    POE_CHECK(PassiveTree::SelectClass(tree, state, 1).ok);  // Marauder: seeds = {31628, 50904}

    // 31628 할당 후, 31628의 인접 노드는 비용 1이어야 하며 미할당 50904를 거쳐 비용 2가 되면 안 됨
    POE_CHECK(PassiveTree::AllocatePath(tree, state, { 31628 }, 20, cfg).ok);
    const auto* n31628 = tree.FindNode(31628);
    POE_CHECK(n31628 != nullptr && !n31628->adjacency.empty());
    const auto nb = n31628->adjacency.front();
    const auto path = PassiveTree::ComputeShortestPath(tree, state, nb);
    POE_CHECK(path.size() == 1 && path[0] == nb);
}

POE_TEST("PassiveTree: Assassin isMultipleChoice hub (46676) enforces 0-pt hub, max 1 option, and downstream lock")
{
    const auto& tree = LoadRealTree();
    Config cfg;
    PassiveTreeState state;
    POE_CHECK(PassiveTree::SelectClass(tree, state, 6).ok);  // Shadow

    // 이정표 0개 -> 전직 선택 거절
    POE_CHECK(!PassiveTree::SelectAscendancy(tree, state, "Assassin", cfg).ok);

    // 1~3차 이정표(6pt) + 보너스 2pt = 8pt
    state.ascendancyMilestonesBitmask = 0x7u;
    cfg.bonusAscendancyPoints = 2;
    POE_CHECK(PassiveTree::ComputeEarnedAscendancyPoints(state, cfg) == 8);
    POE_CHECK(PassiveTree::SelectAscendancy(tree, state, "Assassin", cfg).ok);

    // 46676은 0점 허브라 단독 할당 불가
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, { 46676 }, 50, cfg).ok);
    POE_CHECK(PassiveTree::ComputeShortestPath(tree, state, 46676).empty());

    // 6064만 할당한 상태에서는 허브 46676이 미해금이므로 후속 9014 도달 불가
    POE_CHECK(PassiveTree::AllocatePath(tree, state, { 6064 }, 50, cfg).ok);
    POE_CHECK(PassiveTree::ComputeShortestPath(tree, state, 9014).empty());
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, { 9014 }, 50, cfg).ok);

    // 옵션 18335("For the Jugular") 할당 성공, 동일 허브의 두 번째 옵션 21264("Knife in the Back") 차단
    const auto optPath = PassiveTree::ComputeShortestPath(tree, state, 18335);
    POE_CHECK(optPath.size() == 1 && optPath[0] == 18335);
    POE_CHECK(PassiveTree::AllocatePath(tree, state, optPath, 50, cfg).ok);
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, { 21264 }, 50, cfg).ok);
    POE_CHECK(PassiveTree::ComputeShortestPath(tree, state, 21264).empty());

    // 허브가 해금되었으므로 후속 9014 할당 가능
    const auto afterHubPath = PassiveTree::ComputeShortestPath(tree, state, 9014);
    POE_CHECK(afterHubPath.size() == 1 && afterHubPath[0] == 9014);
    POE_CHECK(PassiveTree::AllocatePath(tree, state, afterHubPath, 50, cfg).ok);

    // 9014가 남아있는 동안 옵션 18335 회수 불가(허브 잠김 -> 9014 단절)
    POE_CHECK(!PassiveTree::RefundNode(tree, state, 18335, 50, cfg).ok);

    // 9014 먼저 회수하면 18335 회수 가능 -> 그 뒤 21264 선택 가능
    POE_CHECK(PassiveTree::RefundNode(tree, state, 9014, 50, cfg).ok);
    POE_CHECK(PassiveTree::RefundNode(tree, state, 18335, 50, cfg).ok);
    POE_CHECK(PassiveTree::AllocatePath(tree, state, { 21264 }, 50, cfg).ok);
}

POE_TEST("PassiveTree: Scion Ascendant Path of the Marauder (24755) grants start seeds, passive points, and refund guards")
{
    const auto& tree = LoadRealTree();
    Config cfg;
    PassiveTreeState state;
    cfg.bonusAscendancyPoints = 8;  // 8 asc pts (이정표 일반 패시브 보너스와 분리해 부여 4pt 가드만 검증)

    POE_CHECK(PassiveTree::SelectClass(tree, state, 0).ok);  // Scion
    POE_CHECK(PassiveTree::SelectAscendancy(tree, state, "Ascendant", cfg).ok);

    // 경로: 193 -> 33875(+1pt) -> (hub 61437) -> 61072(Juggernaut) -> 30690(+1pt) -> 24755(Path of the Marauder, +2pt, grantStart=1)
    POE_CHECK(AllocateTo(tree, state, 61072, 1, cfg));  // 193, 33875, 61072 (3 asc pts)
    POE_CHECK(PassiveTree::ComputeSpentAscendancyPoints(tree, state) == 3);
    POE_CHECK(PassiveTree::ComputeGrantedPassivePoints(tree, state) == 1);

    POE_CHECK(AllocateTo(tree, state, 24755, 1, cfg));  // 30690, 24755 (2 asc pts)
    POE_CHECK(PassiveTree::ComputeSpentAscendancyPoints(tree, state) == 5);
    POE_CHECK(PassiveTree::ComputeGrantedPassivePoints(tree, state) == 4);
    POE_CHECK(PassiveTree::ComputeAvailableNormalPoints(tree, state, 1, cfg) == 4);

    // 이제 Scion이 Marauder 시작 인접(31628)을 직접 할당 가능!
    POE_CHECK(PassiveTree::AllocatePath(tree, state, { 31628 }, 1, cfg).ok);

    // 31628이 남아있는 동안 24755 회수 거절(일반 트리 단절) + RefundAll(ascendancyOnly=true) 거절
    POE_CHECK(!PassiveTree::RefundNode(tree, state, 24755, 1, cfg).ok);
    POE_CHECK(!PassiveTree::RefundAll(tree, state, true, 1, cfg).ok);

    // 31628 회수 후 Scion 본인 시작 인접 4개 할당(레벨 1에서 부여 4포인트 전부 소비)
    POE_CHECK(PassiveTree::RefundNode(tree, state, 31628, 1, cfg).ok);
    POE_CHECK(PassiveTree::AllocatePath(tree, state, { 2151, 15144, 47062, 48828 }, 1, cfg).ok);
    POE_CHECK(PassiveTree::ComputeAvailableNormalPoints(tree, state, 1, cfg) == 0);

    // 부여 포인트를 다 썼으므로 24755 단독 회수 및 전직만 전체 회수 모두 거절(포인트 음수 방지)
    POE_CHECK(!PassiveTree::RefundNode(tree, state, 24755, 1, cfg).ok);
    POE_CHECK(!PassiveTree::RefundAll(tree, state, true, 1, cfg).ok);

    // 일반 노드 2개 회수하면 24755(2pt 부여) 단독 회수는 허용, 전직 전체 회수(4pt 회수)는 여전히 거절
    POE_CHECK(PassiveTree::RefundNode(tree, state, 48828, 1, cfg).ok);
    POE_CHECK(PassiveTree::RefundNode(tree, state, 47062, 1, cfg).ok);
    POE_CHECK(PassiveTree::RefundNode(tree, state, 24755, 1, cfg).ok);
    POE_CHECK(!PassiveTree::RefundAll(tree, state, true, 1, cfg).ok);

    // 나머지 2개도 회수하면 전직만 전체 회수 성공
    POE_CHECK(PassiveTree::RefundNode(tree, state, 15144, 1, cfg).ok);
    POE_CHECK(PassiveTree::RefundNode(tree, state, 2151, 1, cfg).ok);
    POE_CHECK(PassiveTree::RefundAll(tree, state, true, 1, cfg).ok);
    POE_CHECK(state.ascendancyId.empty());
    POE_CHECK(PassiveTree::ComputeSpentAscendancyPoints(tree, state) == 0);
}

POE_TEST("PassiveTree: Mastery unlock, global effectId dedup, and auto-refund on last notable removal")
{
    const auto& tree = LoadRealTree();
    Config cfg;
    cfg.bonusPassivePoints = 100;
    PassiveTreeState state;
    POE_CHECK(PassiveTree::SelectClass(tree, state, 6).ok);  // Shadow

    // Mine Mastery 89 (group 726, notable 45608) & 10166 (group 661, notable 6233), 공유 효과 29161, 47823
    POE_CHECK(!PassiveTree::IsMasteryUnlocked(tree, state, 89));
    POE_CHECK(!PassiveTree::SelectMastery(tree, state, 89, 29161, 100, cfg).ok);

    // notable 45608까지 경로 할당 -> 마스터리 89 해금
    POE_CHECK(AllocateTo(tree, state, 45608, 100, cfg));
    POE_CHECK(PassiveTree::IsMasteryUnlocked(tree, state, 89));
    const auto spentBeforeMastery = PassiveTree::ComputeSpentNormalPoints(tree, state);
    POE_CHECK(PassiveTree::SelectMastery(tree, state, 89, 29161, 100, cfg).ok);
    POE_CHECK(PassiveTree::ComputeSpentNormalPoints(tree, state) == spentBeforeMastery + 1);

    // 두 번째 그룹 notable 6233까지 경로 할당 -> 마스터리 10166 해금
    POE_CHECK(AllocateTo(tree, state, 6233, 100, cfg));
    POE_CHECK(PassiveTree::IsMasteryUnlocked(tree, state, 10166));

    // 같은 effectId(29161) 전역 중복 선택 차단
    POE_CHECK(!PassiveTree::SelectMastery(tree, state, 10166, 29161, 100, cfg).ok);
    // 다른 effectId(47823) 선택 허용
    POE_CHECK(PassiveTree::SelectMastery(tree, state, 10166, 47823, 100, cfg).ok);

    // 45608(그룹 726의 유일한 주요 패시브)이 리프 노드인지 확인 후 회수 시 마스터리 89 자동 회수
    // (경로가 45608을 통과하지 않도록 10166을 먼저 회수하고 6233만 남겨 확인, 또는 45608이 리프가 아니면 6233 쪽으로 검증)
    const auto refundRes = PassiveTree::RefundNode(tree, state, 6233, 100, cfg);
    if (refundRes.ok) {
        POE_CHECK(refundRes.autoRefundedMasteries == 1);
        POE_CHECK(!state.masterySelections.contains(10166));
        POE_CHECK(state.masterySelections.contains(89));
    } else {
        // 6233이 브리지라면 45608 회수로 자동 회수 검증
        const auto r2 = PassiveTree::RefundNode(tree, state, 45608, 100, cfg);
        POE_CHECK(r2.ok);
        POE_CHECK(r2.autoRefundedMasteries == 1);
        POE_CHECK(!state.masterySelections.contains(89));
    }
}

POE_TEST("PassiveTree: SanitizeLoadedState cleans invalid, disconnected, duplicate hub options, and duplicate masteries")
{
    const auto& tree = LoadRealTree();
    PassiveTreeState state;
    state.classSelected = true;
    state.classId = 6;  // Shadow
    state.ascendancyId = "Assassin";
    state.allocatedNodes = {
        999999,  // 미존재 노드
        38129,   // Shadow 정상 시작 인접
        31628,   // Marauder 시작 인접 (Shadow에서는 단절)
        6064,    // Assassin 시작 인접
        18335,   // hub 46676 옵션 1
        21264,   // hub 46676 옵션 2 (중복 -> 제거돼야 함)
        9014     // hub 46676 후속 (옵션 1이 유지되므로 연결 유지)
    };
    state.masterySelections[89] = 29161;     // 근거 notable 미할당 -> 제거
    state.masterySelections[10166] = 29161;  // 중복 + 미할당 -> 제거

    PassiveTree::SanitizeLoadedState(tree, state);

    const auto has = [&](std::uint32_t id) {
        return std::binary_search(state.allocatedNodes.begin(), state.allocatedNodes.end(), id);
    };
    POE_CHECK(!has(999999));
    POE_CHECK(has(38129));
    POE_CHECK(!has(31628));
    POE_CHECK(has(6064));
    POE_CHECK(has(18335) != has(21264));  // 허브당 정확히 1개만 유지
    POE_CHECK(has(9014));
    POE_CHECK(state.masterySelections.empty());
}
