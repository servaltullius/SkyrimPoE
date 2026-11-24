#include "core/PassiveTree.h"

#include <algorithm>
#include <bit>
#include <deque>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace SkyrimPoE::Core
{
    namespace
    {
        [[nodiscard]] bool IsNormalSpendable(const PassiveNodeDef& node) noexcept
        {
            return node.ascendancyId.empty() &&
                node.kind != PassiveNodeKind::ClassStart &&
                node.kind != PassiveNodeKind::Mastery &&
                !node.isMultipleChoice;
        }

        [[nodiscard]] bool IsAscendancySpendable(const PassiveNodeDef& node) noexcept
        {
            return !node.ascendancyId.empty() &&
                node.kind != PassiveNodeKind::AscendancyStart &&
                node.kind != PassiveNodeKind::Mastery &&
                !node.isMultipleChoice;
        }

        [[nodiscard]] std::unordered_set<std::uint32_t> BuildAllocatedSet(const PassiveTreeState& state)
        {
            return std::unordered_set<std::uint32_t>(state.allocatedNodes.begin(), state.allocatedNodes.end());
        }

        void WriteSortedAllocated(PassiveTreeState& state, const std::unordered_set<std::uint32_t>& alloc)
        {
            state.allocatedNodes.assign(alloc.begin(), alloc.end());
            std::sort(state.allocatedNodes.begin(), state.allocatedNodes.end());
        }

        // 활성 일반 트리 출발 시드 = 주 클래스 시작 인접 + Scion Ascendant Path of the ... 로 열린 타 클래스 시작 인접
        [[nodiscard]] std::unordered_set<std::uint32_t> BuildActiveNormalSeeds(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            const std::unordered_set<std::uint32_t>& alloc)
        {
            std::unordered_set<std::uint32_t> seeds;
            if (!state.classSelected) {
                return seeds;
            }
            if (const auto* cls = data.FindClass(state.classId)) {
                for (const auto nb : cls->startNeighbors) {
                    seeds.insert(nb);
                }
            }
            for (const auto nid : alloc) {
                const auto* node = data.FindNode(nid);
                if (node && node->grantsClassStart >= 0) {
                    if (const auto* extraCls = data.FindClass(static_cast<std::uint8_t>(node->grantsClassStart))) {
                        for (const auto nb : extraCls->startNeighbors) {
                            seeds.insert(nb);
                        }
                    }
                }
            }
            return seeds;
        }

        // isMultipleChoice 허브가 현재 할당 집합에서 선택지 1개를 통해 해금되어 있는지 확인
        [[nodiscard]] bool IsHubUnlocked(
            const PassiveTreeData& data,
            std::uint32_t hubId,
            const std::unordered_set<std::uint32_t>& alloc)
        {
            const auto* opts = data.FindHubOptions(hubId);
            if (!opts) {
                return false;
            }
            for (const auto optId : *opts) {
                if (alloc.contains(optId)) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::uint32_t CountHubAllocatedOptions(
            const PassiveTreeData& data,
            std::uint32_t hubId,
            const std::unordered_set<std::uint32_t>& alloc)
        {
            const auto* opts = data.FindHubOptions(hubId);
            if (!opts) {
                return 0;
            }
            std::uint32_t count = 0;
            for (const auto optId : *opts) {
                if (alloc.contains(optId)) {
                    ++count;
                }
            }
            return count;
        }

        // 모든 남은 일반 노드가 활성 일반 시드에서 도달 가능하고, 모든 남은 전직 노드가 전직 시작점에서 도달 가능한지 검증
        [[nodiscard]] bool VerifyConnectivity(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            const std::unordered_set<std::uint32_t>& alloc)
        {
            // 1. 일반 트리 연결성
            const auto normalSeeds = BuildActiveNormalSeeds(data, state, alloc);
            std::unordered_set<std::uint32_t> visitedNormal;
            std::queue<std::uint32_t> q;
            for (const auto s : normalSeeds) {
                if (alloc.contains(s) && visitedNormal.insert(s).second) {
                    q.push(s);
                }
            }
            while (!q.empty()) {
                const auto cur = q.front();
                q.pop();
                const auto* node = data.FindNode(cur);
                if (!node) continue;
                for (const auto nb : node->adjacency) {
                    if (!alloc.contains(nb) || visitedNormal.contains(nb)) continue;
                    const auto* nbNode = data.FindNode(nb);
                    if (nbNode && IsNormalSpendable(*nbNode)) {
                        visitedNormal.insert(nb);
                        q.push(nb);
                    }
                }
            }
            for (const auto nid : alloc) {
                const auto* node = data.FindNode(nid);
                if (!node) return false;
                if (IsNormalSpendable(*node) && !visitedNormal.contains(nid)) {
                    return false;
                }
            }

            // 2. 전직 트리 연결성 (isMultipleChoice 허브는 해금된 경우에만 통과)
            std::size_t ascNodeCount = 0;
            for (const auto nid : alloc) {
                const auto* node = data.FindNode(nid);
                if (node && IsAscendancySpendable(*node)) {
                    ++ascNodeCount;
                    if (node->ascendancyId != state.ascendancyId) {
                        return false;
                    }
                }
            }
            if (ascNodeCount == 0) {
                return true;
            }

            const auto* ascDef = data.FindAscendancy(state.ascendancyId);
            if (!ascDef || ascDef->startNodeId == 0) {
                return false;
            }

            std::unordered_set<std::uint32_t> visitedAsc;
            std::queue<std::uint32_t> aq;
            visitedAsc.insert(ascDef->startNodeId);
            aq.push(ascDef->startNodeId);
            while (!aq.empty()) {
                const auto cur = aq.front();
                aq.pop();
                const auto* node = data.FindNode(cur);
                if (!node) continue;
                for (const auto nb : node->adjacency) {
                    if (visitedAsc.contains(nb)) continue;
                    const auto* nbNode = data.FindNode(nb);
                    if (!nbNode || nbNode->ascendancyId != state.ascendancyId) continue;

                    if (nbNode->isMultipleChoice) {
                        // 허브는 선택지 1개가 할당된 경우에만 진입·통과 가능
                        if (IsHubUnlocked(data, nb, alloc)) {
                            visitedAsc.insert(nb);
                            aq.push(nb);
                        }
                    } else if (alloc.contains(nb)) {
                        visitedAsc.insert(nb);
                        aq.push(nb);
                    }
                }
            }

            for (const auto nid : alloc) {
                const auto* node = data.FindNode(nid);
                if (node && IsAscendancySpendable(*node) && !visitedAsc.contains(nid)) {
                    return false;
                }
            }
            return true;
        }

        // 할당 가능 단일 단계 검증(이미 alloc에 포함된 집합에 nodeId 1개 추가)
        [[nodiscard]] bool CanAllocateSingleStep(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            const std::unordered_set<std::uint32_t>& alloc,
            std::uint32_t nodeId,
            std::string& reasonOut)
        {
            const auto* node = data.FindNode(nodeId);
            if (!node) {
                reasonOut = "존재하지 않는 패시브 노드입니다.";
                return false;
            }
            if (alloc.contains(nodeId)) {
                reasonOut = "이미 할당된 노드입니다.";
                return false;
            }
            if (node->kind == PassiveNodeKind::ClassStart || node->kind == PassiveNodeKind::AscendancyStart) {
                reasonOut = "시작 노드는 자동으로 활성화됩니다.";
                return false;
            }
            if (node->kind == PassiveNodeKind::Mastery) {
                reasonOut = "마스터리는 효과 선택으로 할당해야 합니다.";
                return false;
            }
            if (node->isMultipleChoice) {
                reasonOut = "전직 선택 허브에서는 하위 선택지 중 하나를 골라야 합니다.";
                return false;
            }

            if (node->ascendancyId.empty()) {
                const auto seeds = BuildActiveNormalSeeds(data, state, alloc);
                if (seeds.contains(nodeId)) {
                    return true;
                }
                for (const auto nb : node->adjacency) {
                    if (alloc.contains(nb)) {
                        return true;
                    }
                }
                reasonOut = "현재 할당된 패시브와 연결되어 있지 않습니다.";
                return false;
            }

            // 전직 노드
            if (state.ascendancyId.empty() || node->ascendancyId != state.ascendancyId) {
                reasonOut = "현재 선택한 전직의 노드가 아닙니다.";
                return false;
            }
            const auto* ascDef = data.FindAscendancy(state.ascendancyId);
            if (!ascDef || ascDef->startNodeId == 0) {
                reasonOut = "전직 시작 노드를 찾을 수 없습니다.";
                return false;
            }

            if (node->isMultipleChoiceOption) {
                const auto hubId = node->choiceParentId;
                const auto* hub = data.FindNode(hubId);
                if (!hub || !hub->isMultipleChoice) {
                    reasonOut = "잘못된 다중 선택지 노드입니다.";
                    return false;
                }
                if (CountHubAllocatedOptions(data, hubId, alloc) >= 1) {
                    reasonOut = "이 전직 분기에서는 선택지 1개만 고를 수 있습니다.";
                    return false;
                }
                // 허브가 현재 할당된 전직 노드(또는 전직 시작점)에 닿아 있어야 함
                for (const auto nb : hub->adjacency) {
                    if (nb == ascDef->startNodeId) {
                        return true;
                    }
                    if (alloc.contains(nb)) {
                        const auto* nbNode = data.FindNode(nb);
                        if (nbNode && !nbNode->isMultipleChoiceOption) {
                            return true;
                        }
                    }
                }
                reasonOut = "전직 분기 허브가 아직 연결되어 있지 않습니다.";
                return false;
            }

            // 일반 전직 노드: 인접 노드가 전직 시작점, 할당된 전직 노드, 또는 해금된 isMultipleChoice 허브여야 함
            for (const auto nb : node->adjacency) {
                if (nb == ascDef->startNodeId || alloc.contains(nb)) {
                    return true;
                }
                const auto* nbNode = data.FindNode(nb);
                if (nbNode && nbNode->isMultipleChoice && IsHubUnlocked(data, nb, alloc)) {
                    return true;
                }
            }
            reasonOut = "현재 전직 경로와 연결되어 있지 않습니다.";
            return false;
        }

        // 결정 153 ①: 근거 주요 패시브가 0개가 된 마스터리 자동 회수
        std::size_t PruneOrphanedMasteries(
            const PassiveTreeData& data,
            PassiveTreeState& state,
            const std::unordered_set<std::uint32_t>& alloc)
        {
            std::size_t removed = 0;
            for (auto it = state.masterySelections.begin(); it != state.masterySelections.end();) {
                const auto* mNode = data.FindNode(it->first);
                bool hasNotable = false;
                if (mNode && mNode->kind == PassiveNodeKind::Mastery) {
                    if (const auto* notables = data.FindGroupNotables(mNode->groupId)) {
                        for (const auto notId : *notables) {
                            if (alloc.contains(notId)) {
                                hasNotable = true;
                                break;
                            }
                        }
                    }
                }
                if (!hasNotable) {
                    it = state.masterySelections.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            return removed;
        }
    }

    std::int32_t PassiveTree::ComputeEarnedAscendancyPoints(
        const PassiveTreeState& state,
        const Config& config) noexcept
    {
        const auto milestoneBits = state.ascendancyMilestonesBitmask & 0xFu;  // 1~4차 이정표 (bit 0..3, 결정 166)
        const std::int32_t fromMilestones = static_cast<std::int32_t>(std::popcount(milestoneBits)) * 2;
        const std::int32_t bonus = std::max<std::int32_t>(0, config.bonusAscendancyPoints);
        return std::min<std::int32_t>(static_cast<std::int32_t>(kMaxAscendancyPoints), fromMilestones + bonus);
    }

    std::int32_t PassiveTree::ComputeGrantedPassivePoints(
        const PassiveTreeData& data,
        const PassiveTreeState& state) noexcept
    {
        std::int32_t total = 0;
        for (const auto nid : state.allocatedNodes) {
            if (const auto* node = data.FindNode(nid)) {
                total += static_cast<std::int32_t>(node->grantedPassivePoints);
            }
        }
        return total;
    }

    std::int32_t PassiveTree::ComputeSpentNormalPoints(
        const PassiveTreeData& data,
        const PassiveTreeState& state) noexcept
    {
        std::int32_t spent = static_cast<std::int32_t>(state.masterySelections.size());
        for (const auto nid : state.allocatedNodes) {
            if (const auto* node = data.FindNode(nid); node && IsNormalSpendable(*node)) {
                ++spent;
            }
        }
        return spent;
    }

    std::int32_t PassiveTree::ComputeSpentAscendancyPoints(
        const PassiveTreeData& data,
        const PassiveTreeState& state) noexcept
    {
        std::int32_t spent = 0;
        for (const auto nid : state.allocatedNodes) {
            if (const auto* node = data.FindNode(nid); node && IsAscendancySpendable(*node)) {
                ++spent;
            }
        }
        return spent;
    }

    std::int32_t PassiveTree::ComputeAvailableNormalPoints(
        const PassiveTreeData& data,
        const PassiveTreeState& state,
        std::uint32_t exileLevel,
        const Config& config,
        std::uint16_t atlasCompletedTierMask) noexcept
    {
        const auto lv = std::clamp<std::uint32_t>(exileLevel, 1, kMaxExileLevel);
        const std::int32_t fromLevel = static_cast<std::int32_t>(lv - 1);
        // 결정 17, 166: 아틀라스 T1~T16 첫 클리어(최대 +16점) + 1~4차 보스/결투장 이정표(최대 +8점) = +24점 (총 123점)
        const std::int32_t fromAtlas = static_cast<std::int32_t>(std::popcount(atlasCompletedTierMask));
        const std::int32_t fromMilestones = static_cast<std::int32_t>(
            std::popcount(static_cast<std::uint8_t>(state.ascendancyMilestonesBitmask & 0xFu))) * 2;
        const std::int32_t bonus = std::max<std::int32_t>(0, config.bonusPassivePoints);
        const std::int32_t granted = ComputeGrantedPassivePoints(data, state);
        const std::int32_t spent = ComputeSpentNormalPoints(data, state);
        return fromLevel + fromAtlas + fromMilestones + bonus + granted - spent;
    }

    std::int32_t PassiveTree::ComputeAvailableAscendancyPoints(
        const PassiveTreeData& data,
        const PassiveTreeState& state,
        const Config& config) noexcept
    {
        return ComputeEarnedAscendancyPoints(state, config) - ComputeSpentAscendancyPoints(data, state);
    }

    std::vector<std::uint32_t> PassiveTree::ComputeShortestPath(
        const PassiveTreeData& data,
        const PassiveTreeState& state,
        std::uint32_t targetNodeId)
    {
        if (!state.classSelected) {
            return {};
        }
        const auto* target = data.FindNode(targetNodeId);
        if (!target || target->kind == PassiveNodeKind::ClassStart ||
            target->kind == PassiveNodeKind::AscendancyStart ||
            target->kind == PassiveNodeKind::Mastery ||
            target->isMultipleChoice) {
            return {};
        }

        const auto alloc = BuildAllocatedSet(state);
        if (alloc.contains(targetNodeId)) {
            return {};
        }

        std::unordered_map<std::uint32_t, std::uint32_t> parent;

        if (target->ascendancyId.empty()) {
            // 일반 트리 최단 경로 0-1 BFS:
            // 이미 할당된 노드는 비용 0, 미할당 시작 인접(seed)과 미할당 일반 노드는 비용 1.
            std::unordered_map<std::uint32_t, std::uint32_t> dist;
            std::deque<std::uint32_t> dq;
            const auto seeds = BuildActiveNormalSeeds(data, state, alloc);

            for (const auto nid : alloc) {
                const auto* n = data.FindNode(nid);
                if (n && IsNormalSpendable(*n)) {
                    dist[nid] = 0;
                    parent[nid] = 0;
                    dq.push_front(nid);
                }
            }
            for (const auto s : seeds) {
                if (!dist.contains(s)) {
                    dist[s] = 1;
                    parent[s] = 0;
                    dq.push_back(s);
                }
            }

            while (!dq.empty()) {
                const auto cur = dq.front();
                dq.pop_front();
                if (cur == targetNodeId) {
                    break;
                }
                const auto curDist = dist[cur];
                const auto* node = data.FindNode(cur);
                if (!node) continue;
                for (const auto nb : node->adjacency) {
                    const auto* nbNode = data.FindNode(nb);
                    if (!nbNode || !IsNormalSpendable(*nbNode)) continue;
                    const std::uint32_t stepCost = alloc.contains(nb) ? 0u : 1u;
                    const std::uint32_t nextDist = curDist + stepCost;
                    const auto it = dist.find(nb);
                    if (it != dist.end() && it->second <= nextDist) {
                        continue;
                    }
                    dist[nb] = nextDist;
                    parent[nb] = cur;
                    if (stepCost == 0u) {
                        dq.push_front(nb);
                    } else {
                        dq.push_back(nb);
                    }
                }
            }
            if (!dist.contains(targetNodeId)) {
                return {};
            }
            std::vector<std::uint32_t> path;
            for (std::uint32_t cur = targetNodeId; cur != 0; cur = parent[cur]) {
                if (!alloc.contains(cur)) {
                    path.push_back(cur);
                }
            }
            std::reverse(path.begin(), path.end());
            return path;
        }

        // 전직 트리 최단 경로 0-1 BFS (전직 시작점·할당 노드·0점 허브는 비용 0, 미할당 소비 노드는 비용 1)
        if (state.ascendancyId.empty() || target->ascendancyId != state.ascendancyId) {
            return {};
        }
        const auto* ascDef = data.FindAscendancy(state.ascendancyId);
        if (!ascDef || ascDef->startNodeId == 0) {
            return {};
        }

        std::unordered_map<std::uint32_t, std::uint32_t> dist;
        std::deque<std::uint32_t> dq;
        dist[ascDef->startNodeId] = 0;
        parent[ascDef->startNodeId] = 0;
        dq.push_front(ascDef->startNodeId);
        for (const auto nid : alloc) {
            const auto* n = data.FindNode(nid);
            if (n && IsAscendancySpendable(*n) && n->ascendancyId == state.ascendancyId) {
                if (!dist.contains(nid)) {
                    dist[nid] = 0;
                    parent[nid] = 0;
                    dq.push_front(nid);
                }
            }
        }

        while (!dq.empty()) {
            const auto cur = dq.front();
            dq.pop_front();
            if (cur == targetNodeId) {
                break;
            }
            const auto curDist = dist[cur];
            const auto* node = data.FindNode(cur);
            if (!node) continue;

            for (const auto nb : node->adjacency) {
                const auto* nbNode = data.FindNode(nb);
                if (!nbNode || nbNode->ascendancyId != state.ascendancyId) continue;

                if (node->isMultipleChoice) {
                    // 허브에서 나가는 간선:
                    // - 이미 해금된 허브: 비선택지 후속 노드(예: Passive Point)로만 진행(중복 옵션 차단)
                    // - 미해금 허브: 선택지 옵션(isMultipleChoiceOption)으로만 진행(옵션 없이 통과 차단)
                    const bool unlocked = IsHubUnlocked(data, cur, alloc);
                    if (unlocked && nbNode->isMultipleChoiceOption) continue;
                    if (!unlocked && !nbNode->isMultipleChoiceOption) continue;
                }

                // 선택지 옵션 노드에서는 허브를 통해 되돌아가 다른 옵션으로 새지 않도록 차단
                if (node->isMultipleChoiceOption && nbNode->isMultipleChoice) {
                    continue;
                }

                const std::uint32_t stepCost =
                    (nbNode->isMultipleChoice || nb == ascDef->startNodeId || alloc.contains(nb)) ? 0u : 1u;
                const std::uint32_t nextDist = curDist + stepCost;
                const auto it = dist.find(nb);
                if (it != dist.end() && it->second <= nextDist) {
                    continue;
                }
                dist[nb] = nextDist;
                parent[nb] = cur;
                if (stepCost == 0u) {
                    dq.push_front(nb);
                } else {
                    dq.push_back(nb);
                }
            }
        }

        if (!dist.contains(targetNodeId)) {
            return {};
        }
        std::vector<std::uint32_t> path;
        for (std::uint32_t cur = targetNodeId; cur != 0; cur = parent[cur]) {
            const auto* n = data.FindNode(cur);
            if (n && IsAscendancySpendable(*n) && !alloc.contains(cur)) {
                path.push_back(cur);
            }
        }
        std::reverse(path.begin(), path.end());
        return path;
    }

    bool PassiveTree::IsMasteryUnlocked(
        const PassiveTreeData& data,
        const PassiveTreeState& state,
        std::uint32_t masteryNodeId) noexcept
    {
        if (!state.classSelected) {
            return false;
        }
        const auto* node = data.FindNode(masteryNodeId);
        if (!node || node->kind != PassiveNodeKind::Mastery) {
            return false;
        }
        const auto* notables = data.FindGroupNotables(node->groupId);
        if (!notables || notables->empty()) {
            return false;
        }
        for (const auto notId : *notables) {
            if (std::binary_search(state.allocatedNodes.begin(), state.allocatedNodes.end(), notId)) {
                return true;
            }
        }
        return false;
    }

    PassiveTreeOpResult PassiveTree::SelectClass(
        const PassiveTreeData& data,
        PassiveTreeState& state,
        std::uint8_t classId)
    {
        const auto* cls = data.FindClass(classId);
        if (!cls) {
            return { false, "잘못된 클래스 번호입니다.", 0 };
        }
        if (state.classSelected &&
            (ComputeSpentNormalPoints(data, state) > 0 || ComputeSpentAscendancyPoints(data, state) > 0)) {
            return { false, "할당한 노드를 모두 회수해야 클래스를 변경할 수 있습니다.", 0 };
        }
        state.classSelected = true;
        state.classId = classId;
        state.ascendancyId.clear();
        state.allocatedNodes.clear();
        state.masterySelections.clear();
        return { true, cls->name + " 클래스를 선택했습니다.", 0 };
    }

    PassiveTreeOpResult PassiveTree::SelectAscendancy(
        const PassiveTreeData& data,
        PassiveTreeState& state,
        std::string_view ascendancyId,
        const Config& config)
    {
        if (!state.classSelected) {
            return { false, "먼저 시작 클래스를 선택해야 합니다.", 0 };
        }
        const auto* asc = data.FindAscendancy(ascendancyId);
        if (!asc || asc->classId != state.classId) {
            return { false, "현재 클래스에서 선택할 수 없는 전직입니다.", 0 };
        }
        if (ComputeEarnedAscendancyPoints(state, config) < 2) {
            return { false, "전직 포인트가 부족합니다(1차 보스 이정표 달성 필요).", 0 };
        }
        if (ComputeSpentAscendancyPoints(data, state) > 0 && state.ascendancyId != asc->id) {
            return { false, "할당한 전직 노드를 모두 회수해야 전직을 변경할 수 있습니다.", 0 };
        }
        state.ascendancyId = asc->id;
        return { true, asc->name + " 전직을 선택했습니다.", 0 };
    }

    PassiveTreeOpResult PassiveTree::ClearAscendancy(
        const PassiveTreeData& data,
        PassiveTreeState& state)
    {
        if (ComputeSpentAscendancyPoints(data, state) > 0) {
            return { false, "할당한 전직 노드를 모두 회수해야 전직을 해제할 수 있습니다.", 0 };
        }
        state.ascendancyId.clear();
        return { true, "전직 선택을 해제했습니다.", 0 };
    }

    PassiveTreeOpResult PassiveTree::AllocatePath(
        const PassiveTreeData& data,
        PassiveTreeState& state,
        const std::vector<std::uint32_t>& nodeIds,
        std::uint32_t exileLevel,
        const Config& config,
        std::uint16_t atlasCompletedTierMask)
    {
        if (!state.classSelected) {
            return { false, "먼저 시작 클래스를 선택해야 합니다.", 0 };
        }
        if (nodeIds.empty()) {
            return { false, "할당할 노드가 비어 있습니다.", 0 };
        }

        PassiveTreeState working = state;
        auto alloc = BuildAllocatedSet(working);

        for (const auto nid : nodeIds) {
            if (alloc.contains(nid)) {
                continue;
            }
            const auto* node = data.FindNode(nid);
            if (!node) {
                return { false, "존재하지 않는 패시브 노드입니다.", 0 };
            }
            if (node->isMultipleChoice) {
                continue;  // 0점 허브는 경로 배열에 섞여 와도 무시(하위 옵션으로 처리)
            }

            std::string reason;
            if (!CanAllocateSingleStep(data, working, alloc, nid, reason)) {
                return { false, std::move(reason), 0 };
            }

            const bool isAsc = !node->ascendancyId.empty();
            WriteSortedAllocated(working, alloc);
            if (isAsc) {
                if (ComputeAvailableAscendancyPoints(data, working, config) < 1) {
                    return { false, "전직 포인트가 부족합니다.", 0 };
                }
            } else {
                if (ComputeAvailableNormalPoints(data, working, exileLevel, config, atlasCompletedTierMask) < 1) {
                    return { false, "패시브 포인트가 부족합니다.", 0 };
                }
            }
            alloc.insert(nid);
        }

        WriteSortedAllocated(working, alloc);
        if (working.allocatedNodes.size() == state.allocatedNodes.size()) {
            return { false, "새로 할당할 노드가 없습니다.", 0 };
        }
        if (ComputeAvailableNormalPoints(data, working, exileLevel, config, atlasCompletedTierMask) < 0 ||
            ComputeAvailableAscendancyPoints(data, working, config) < 0) {
            return { false, "가용 포인트가 부족합니다.", 0 };
        }
        if (!VerifyConnectivity(data, working, alloc)) {
            return { false, "트리 연결성 검증에 실패했습니다.", 0 };
        }

        state = std::move(working);
        return { true, "패시브 노드를 할당했습니다.", 0 };
    }

    std::uint32_t PassiveTree::GetRefundNodeCost(
        const PassiveTreeData& data,
        const PassiveTreeState& state,
        std::uint32_t nodeId,
        const Config& config)
    {
        if (config.freePassiveRefund || !state.classSelected) {
            return 0;
        }
        if (state.masterySelections.contains(nodeId)) {
            return 1;
        }
        const auto alloc = BuildAllocatedSet(state);
        if (!alloc.contains(nodeId)) {
            return 0;
        }
        const auto* node = data.FindNode(nodeId);
        if (!node) {
            return 0;
        }
        if (!IsNormalSpendable(*node)) {
            return std::max<std::uint32_t>(1, config.ascendancyRefundCost);
        }
        auto nextAlloc = alloc;
        nextAlloc.erase(nodeId);
        PassiveTreeState temp = state;
        const auto orphaned = PruneOrphanedMasteries(data, temp, nextAlloc);
        return static_cast<std::uint32_t>(1 + orphaned);
    }

    std::uint32_t PassiveTree::GetRefundAllCost(
        const PassiveTreeData& data,
        const PassiveTreeState& state,
        bool ascendancyOnly,
        const Config& config)
    {
        if (config.freePassiveRefund || !state.classSelected) {
            return 0;
        }
        const std::uint32_t ascUnit = std::max<std::uint32_t>(1, config.ascendancyRefundCost);
        std::uint32_t cost = 0;
        for (const auto nid : state.allocatedNodes) {
            const auto* node = data.FindNode(nid);
            if (!node) continue;
            if (IsNormalSpendable(*node)) {
                if (!ascendancyOnly) {
                    cost += 1;
                }
            } else {
                cost += ascUnit;
            }
        }
        if (!ascendancyOnly) {
            cost += static_cast<std::uint32_t>(state.masterySelections.size());
        }
        return cost;
    }

    std::uint32_t PassiveTree::GetSelectMasterySwapCost(
        const PassiveTreeState& state,
        std::uint32_t masteryNodeId,
        std::uint32_t effectId,
        const Config& config) noexcept
    {
        if (config.freePassiveRefund) {
            return 0;
        }
        const auto it = state.masterySelections.find(masteryNodeId);
        if (it != state.masterySelections.end() && it->second != effectId) {
            return 1;
        }
        return 0;
    }

    PassiveTreeOpResult PassiveTree::SelectMastery(
        const PassiveTreeData& data,
        PassiveTreeState& state,
        std::uint32_t masteryNodeId,
        std::uint32_t effectId,
        std::uint32_t exileLevel,
        const Config& config,
        std::uint32_t availableRegretOrbs,
        std::uint16_t atlasCompletedTierMask)
    {
        if (!state.classSelected) {
            return { false, "먼저 시작 클래스를 선택해야 합니다.", 0, 0 };
        }
        const auto* node = data.FindNode(masteryNodeId);
        if (!node || node->kind != PassiveNodeKind::Mastery) {
            return { false, "마스터리 노드가 아닙니다.", 0, 0 };
        }
        if (!IsMasteryUnlocked(data, state, masteryNodeId)) {
            return { false, "같은 그룹의 주요 패시브를 먼저 1개 이상 할당해야 합니다.", 0, 0 };
        }
        const bool validEffect = std::find(node->masteryEffectIds.begin(), node->masteryEffectIds.end(), effectId) !=
            node->masteryEffectIds.end();
        if (!validEffect || !data.FindMasteryEffect(effectId)) {
            return { false, "이 마스터리에서 선택할 수 없는 효과입니다.", 0, 0 };
        }

        // 결정 153 ①: 같은 마스터리 효과(effectId) 전역 중복 금지
        for (const auto& [otherNodeId, otherEffectId] : state.masterySelections) {
            if (otherNodeId != masteryNodeId && otherEffectId == effectId) {
                return { false, "이미 다른 마스터리에서 선택한 효과입니다.", 0, 0 };
            }
        }

        const bool alreadyAllocated = state.masterySelections.contains(masteryNodeId);
        std::uint32_t orbCost = 0;
        if (!alreadyAllocated) {
            if (ComputeAvailableNormalPoints(data, state, exileLevel, config, atlasCompletedTierMask) < 1) {
                return { false, "패시브 포인트가 부족합니다.", 0, 0 };
            }
        } else {
            orbCost = GetSelectMasterySwapCost(state, masteryNodeId, effectId, config);
            if (orbCost > 0 && availableRegretOrbs < orbCost) {
                return { false, "마스터리 효과를 교체하려면 후회의 오브가 1개 필요합니다.", 0, 0 };
            }
        }

        state.masterySelections[masteryNodeId] = effectId;
        return { true, "마스터리 효과를 적용했습니다.", 0, orbCost };
    }

    PassiveTreeOpResult PassiveTree::RefundNode(
        const PassiveTreeData& data,
        PassiveTreeState& state,
        std::uint32_t nodeId,
        std::uint32_t exileLevel,
        const Config& config,
        std::uint32_t availableRegretOrbs,
        std::uint16_t atlasCompletedTierMask)
    {
        if (!state.classSelected) {
            return { false, "선택된 클래스가 없습니다.", 0, 0 };
        }

        // 마스터리 회수
        if (state.masterySelections.contains(nodeId)) {
            const std::uint32_t cost = config.freePassiveRefund ? 0u : 1u;
            if (cost > 0 && availableRegretOrbs < cost) {
                return { false, "후회의 오브가 부족합니다 (필요: 1개).", 0, 0 };
            }
            state.masterySelections.erase(nodeId);
            return { true, "마스터리 선택을 회수했습니다.", 0, cost };
        }

        PassiveTreeState working = state;
        auto alloc = BuildAllocatedSet(working);
        if (!alloc.contains(nodeId)) {
            return { false, "할당되어 있지 않은 노드입니다.", 0, 0 };
        }
        const auto* targetNode = data.FindNode(nodeId);
        if (!targetNode) {
            return { false, "존재하지 않는 노드입니다.", 0, 0 };
        }
        alloc.erase(nodeId);

        if (!VerifyConnectivity(data, working, alloc)) {
            return { false, "다른 할당 노드와의 연결이 끊어져 회수할 수 없습니다.", 0, 0 };
        }

        // 결정 153 ①: 근거 주요 패시브 전량 회수 시 연결 마스터리 자동 회수
        const auto autoRefunded = PruneOrphanedMasteries(data, working, alloc);
        WriteSortedAllocated(working, alloc);

        // Ascendant grantedPassivePoints 노드를 빼서 일반 포인트 잔액이 기존보다 줄어들며 음수가 되는 경우만 거절
        const std::int32_t availBefore = ComputeAvailableNormalPoints(data, state, exileLevel, config, atlasCompletedTierMask);
        const std::int32_t availAfter = ComputeAvailableNormalPoints(data, working, exileLevel, config, atlasCompletedTierMask);
        if (availAfter < availBefore && availAfter < 0) {
            return { false, "이 노드가 부여한 패시브 포인트를 먼저 회수해야 합니다.", 0, 0 };
        }

        std::uint32_t orbCost = 0;
        if (!config.freePassiveRefund) {
            if (!IsNormalSpendable(*targetNode)) {
                orbCost = std::max<std::uint32_t>(1, config.ascendancyRefundCost);
            } else {
                orbCost = static_cast<std::uint32_t>(1 + autoRefunded);
            }
            if (availableRegretOrbs < orbCost) {
                return { false, "후회의 오브가 부족합니다 (필요: " + std::to_string(orbCost) + "개).", 0, 0 };
            }
        }

        state = std::move(working);
        return {
            true,
            autoRefunded > 0 ? "노드와 연결된 마스터리를 함께 회수했습니다." : "노드를 회수했습니다.",
            autoRefunded,
            orbCost
        };
    }

    PassiveTreeOpResult PassiveTree::RefundAll(
        const PassiveTreeData& data,
        PassiveTreeState& state,
        bool ascendancyOnly,
        std::uint32_t exileLevel,
        const Config& config,
        std::uint32_t availableRegretOrbs,
        std::uint16_t atlasCompletedTierMask)
    {
        if (!state.classSelected) {
            return { false, "선택된 클래스가 없습니다.", 0, 0 };
        }

        const std::uint32_t orbCost = GetRefundAllCost(data, state, ascendancyOnly, config);
        if (!ascendancyOnly) {
            if (!config.freePassiveRefund && orbCost > 0 && availableRegretOrbs < orbCost) {
                return { false, "후회의 오브가 부족합니다 (필요: " + std::to_string(orbCost) + "개).", 0, 0 };
            }
            state.allocatedNodes.clear();
            state.masterySelections.clear();
            state.ascendancyId.clear();
            return { true, "모든 패시브 노드를 회수했습니다.", 0, orbCost };
        }

        // 검수 M3, m5: 전직만 회수 시 연결성·부여 포인트 검증을 먼저 수행한 뒤 후회의 오브 잔량 확인
        PassiveTreeState working = state;
        std::unordered_set<std::uint32_t> remaining;
        for (const auto nid : working.allocatedNodes) {
            const auto* node = data.FindNode(nid);
            if (node && IsNormalSpendable(*node)) {
                remaining.insert(nid);
            }
        }
        working.ascendancyId.clear();
        if (!VerifyConnectivity(data, working, remaining)) {
            return { false, "전직 시작점에 연결된 일반 노드를 먼저 회수하거나 전체 회수를 사용해 주세요.", 0, 0 };
        }
        WriteSortedAllocated(working, remaining);
        const std::int32_t availBefore = ComputeAvailableNormalPoints(data, state, exileLevel, config, atlasCompletedTierMask);
        const std::int32_t availAfter = ComputeAvailableNormalPoints(data, working, exileLevel, config, atlasCompletedTierMask);
        if (availAfter < availBefore && availAfter < 0) {
            return { false, "전직이 부여한 패시브 포인트를 먼저 회수하거나 전체 회수를 사용해 주세요.", 0, 0 };
        }
        if (!config.freePassiveRefund && orbCost > 0 && availableRegretOrbs < orbCost) {
            return { false, "후회의 오브가 부족합니다 (필요: " + std::to_string(orbCost) + "개).", 0, 0 };
        }

        state = std::move(working);
        return { true, "전직 노드를 모두 회수했습니다.", 0, orbCost };
    }

    void PassiveTree::SanitizeLoadedState(
        const PassiveTreeData& data,
        PassiveTreeState& state)
    {
        if (!data.IsLoaded()) {
            return;
        }
        if (!state.classSelected) {
            state.allocatedNodes.clear();
            state.masterySelections.clear();
            state.ascendancyId.clear();
            return;
        }
        if (!data.FindClass(state.classId)) {
            state.classSelected = false;
            state.classId = 0;
            state.ascendancyId.clear();
            state.allocatedNodes.clear();
            state.masterySelections.clear();
            return;
        }
        if (!state.ascendancyId.empty()) {
            const auto* asc = data.FindAscendancy(state.ascendancyId);
            if (!asc || asc->classId != state.classId) {
                state.ascendancyId.clear();
            }
        }

        // 유효 노드만 필터링 + 허브당 옵션 최대 1개 유지
        std::unordered_set<std::uint32_t> alloc;
        std::unordered_set<std::uint32_t> usedHubs;
        for (const auto nid : state.allocatedNodes) {
            const auto* node = data.FindNode(nid);
            if (!node) continue;
            if (!IsNormalSpendable(*node) && !IsAscendancySpendable(*node)) continue;
            if (!node->ascendancyId.empty() && node->ascendancyId != state.ascendancyId) continue;
            if (node->isMultipleChoiceOption) {
                if (node->choiceParentId == 0 || !usedHubs.insert(node->choiceParentId).second) {
                    continue;
                }
            }
            alloc.insert(nid);
        }

        // 도달 가능한 부분집합만 유지(전직 먼저 -> 활성 일반 시드 확정 -> 일반 BFS)
        std::unordered_set<std::uint32_t> kept;
        if (const auto* asc = data.FindAscendancy(state.ascendancyId); asc && asc->startNodeId != 0) {
            std::unordered_set<std::uint32_t> vis;
            std::queue<std::uint32_t> q;
            vis.insert(asc->startNodeId);
            q.push(asc->startNodeId);
            while (!q.empty()) {
                const auto cur = q.front();
                q.pop();
                const auto* node = data.FindNode(cur);
                if (!node) continue;
                for (const auto nb : node->adjacency) {
                    if (vis.contains(nb)) continue;
                    const auto* nbNode = data.FindNode(nb);
                    if (!nbNode || nbNode->ascendancyId != state.ascendancyId) continue;
                    if (nbNode->isMultipleChoice) {
                        if (IsHubUnlocked(data, nb, alloc)) {
                            vis.insert(nb);
                            q.push(nb);
                        }
                    } else if (alloc.contains(nb)) {
                        vis.insert(nb);
                        kept.insert(nb);
                        q.push(nb);
                    }
                }
            }
        }

        const auto normalSeeds = BuildActiveNormalSeeds(data, state, kept);
        std::unordered_set<std::uint32_t> visN;
        std::queue<std::uint32_t> qn;
        for (const auto s : normalSeeds) {
            if (alloc.contains(s) && visN.insert(s).second) {
                kept.insert(s);
                qn.push(s);
            }
        }
        while (!qn.empty()) {
            const auto cur = qn.front();
            qn.pop();
            const auto* node = data.FindNode(cur);
            if (!node) continue;
            for (const auto nb : node->adjacency) {
                if (!alloc.contains(nb) || visN.contains(nb)) continue;
                const auto* nbNode = data.FindNode(nb);
                if (nbNode && IsNormalSpendable(*nbNode)) {
                    visN.insert(nb);
                    kept.insert(nb);
                    qn.push(nb);
                }
            }
        }

        WriteSortedAllocated(state, kept);

        // 마스터리 유효성 + 전역 effectId 중복 제거 + 근거 주요 패시브 존재 검증
        std::unordered_set<std::uint32_t> usedEffects;
        std::vector<std::uint32_t> mKeys;
        mKeys.reserve(state.masterySelections.size());
        for (const auto& [mId, _] : state.masterySelections) {
            mKeys.push_back(mId);
        }
        std::sort(mKeys.begin(), mKeys.end());
        for (const auto mId : mKeys) {
            const auto effId = state.masterySelections[mId];
            const auto* mNode = data.FindNode(mId);
            const bool validNode = mNode && mNode->kind == PassiveNodeKind::Mastery &&
                std::find(mNode->masteryEffectIds.begin(), mNode->masteryEffectIds.end(), effId) != mNode->masteryEffectIds.end() &&
                data.FindMasteryEffect(effId) != nullptr;
            if (!validNode || !usedEffects.insert(effId).second) {
                state.masterySelections.erase(mId);
            }
        }
        (void)PruneOrphanedMasteries(data, state, kept);
    }
}
