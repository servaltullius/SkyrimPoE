#include "core/PassiveTree.h"

#include <algorithm>
#include <bit>
#include <deque>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace SkyrimPoE::Core
{
    namespace
    {
        [[nodiscard]] bool IsAllocatable(const PassiveNodeDef& node) noexcept
        {
            return node.kind != PassiveNodeKind::ClassStart && node.kind != PassiveNodeKind::AscendancyStart &&
                node.kind != PassiveNodeKind::Mastery && !node.isMultipleChoice && node.alias.empty();
        }

        [[nodiscard]] const PassiveNodeVariant* FindVariant(
            const PassiveNodeDef& node, const PassiveTreeState& state) noexcept
        {
            auto variant = node.variants.find(state.classContentId);
            if (variant == node.variants.end()) {
                variant = node.variants.find(state.ascendancyId);
            }
            return variant != node.variants.end() ? &variant->second : nullptr;
        }

        [[nodiscard]] std::string_view AscendancyOf(const PassiveNodeDef& node, const PassiveTreeState& state) noexcept
        {
            const auto* variant = FindVariant(node, state);
            return variant && !variant->ascendancyId.empty() ? variant->ascendancyId : node.ascendancyId;
        }

        [[nodiscard]] bool HasUnlockRequirements(
            const PassiveNodeDef& node, const std::unordered_set<std::uint32_t>& alloc)
        {
            return std::ranges::all_of(node.unlock, [&](auto id) { return alloc.contains(id); });
        }

        void PruneAttributeSelections(PassiveTreeState& state, const std::unordered_set<std::uint32_t>& alloc)
        {
            std::erase_if(state.attributeSelections, [&](const auto& selection) {
                return !alloc.contains(selection.first);
            });
        }

        [[nodiscard]] bool HasAllocatedAscendancyNodes(const PassiveTreeData& data, const PassiveTreeState& state)
        {
            return std::ranges::any_of(state.allocatedNodes, [&](auto id) {
                const auto* node = data.FindNode(id);
                return node && !AscendancyOf(*node, state).empty();
            });
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

        // 전직 시드와 전직에서 열린 일반 시작점까지 따라가며 할당된 노드의 도달 가능성을 검사한다.
        [[nodiscard]] std::unordered_set<std::uint32_t> FindReachableAllocated(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            const std::unordered_set<std::uint32_t>& alloc,
            const std::unordered_set<std::uint32_t>& unlockedBy)
        {
            std::unordered_set<std::uint32_t> kept;
            std::unordered_set<std::uint32_t> visited;
            std::queue<std::uint32_t> queue;
            const auto enqueue = [&](std::uint32_t id, std::string_view ascendancyId) {
                if (visited.contains(id)) return;
                const auto* node = data.FindNode(id);
                if (!node || AscendancyOf(*node, state) != ascendancyId ||
                    !HasUnlockRequirements(*node, unlockedBy)) return;
                if (node->isMultipleChoice && !ascendancyId.empty() && IsHubUnlocked(data, id, alloc)) {
                    visited.insert(id);
                    queue.push(id);
                } else if (alloc.contains(id) && IsAllocatable(*node)) {
                    visited.insert(id);
                    kept.insert(id);
                    queue.push(id);
                }
            };

            if (const auto* asc = data.FindAscendancy(state.ascendancyId)) {
                if (const auto* start = data.FindNode(asc->startNodeId)) {
                    for (const auto id : start->adjacency) {
                        enqueue(id, asc->id);
                    }
                }
                while (!queue.empty()) {
                    const auto id = queue.front();
                    queue.pop();
                    for (const auto neighbor : data.FindNode(id)->adjacency) {
                        enqueue(neighbor, asc->id);
                    }
                }
            }

            for (const auto id : BuildActiveNormalSeeds(data, state, kept)) {
                enqueue(id, {});
            }
            while (!queue.empty()) {
                const auto id = queue.front();
                queue.pop();
                for (const auto neighbor : data.FindNode(id)->adjacency) {
                    enqueue(neighbor, {});
                }
            }
            return kept;
        }

        [[nodiscard]] bool VerifyConnectivity(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            const std::unordered_set<std::uint32_t>& alloc)
        {
            return FindReachableAllocated(data, state, alloc, alloc).size() == alloc.size();
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

            if (!node->alias.empty() || !HasUnlockRequirements(*node, alloc)) {
                reasonOut = "이 노드의 할당 조건을 아직 충족하지 않았습니다.";
                return false;
            }
            const auto ascendancyId = AscendancyOf(*node, state);
            if (ascendancyId.empty()) {
                const auto seeds = BuildActiveNormalSeeds(data, state, alloc);
                if (seeds.contains(nodeId)) {
                    return true;
                }
                for (const auto nb : node->adjacency) {
                    const auto* neighbor = data.FindNode(nb);
                    if (alloc.contains(nb) && neighbor && AscendancyOf(*neighbor, state).empty()) {
                        return true;
                    }
                }
                reasonOut = "현재 할당된 패시브와 연결되어 있지 않습니다.";
                return false;
            }

            // 전직 노드
            if (state.ascendancyId.empty() || ascendancyId != state.ascendancyId) {
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
                if (!hub || !hub->isMultipleChoice || !HasUnlockRequirements(*hub, alloc)) {
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
                const auto* nbNode = data.FindNode(nb);
                if (nb == ascDef->startNodeId || (nbNode && AscendancyOf(*nbNode, state) == state.ascendancyId &&
                    (alloc.contains(nb) || (nbNode->isMultipleChoice && HasUnlockRequirements(*nbNode, alloc) &&
                        IsHubUnlocked(data, nb, alloc))))) {
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
            if (const auto* node = data.FindNode(nid); node && IsAllocatable(*node) && AscendancyOf(*node, state).empty() && !node->freeAlloc) {
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
            if (const auto* node = data.FindNode(nid); node && IsAllocatable(*node) && !AscendancyOf(*node, state).empty() && !node->freeAlloc) {
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
        if (!state.classSelected || state.generation != data.Generation()) {
            return {};
        }
        const auto* target = data.FindNode(targetNodeId);
        const auto alloc = BuildAllocatedSet(state);
        if (!target || !IsAllocatable(*target) || alloc.contains(targetNodeId) ||
            !HasUnlockRequirements(*target, alloc)) {
            return {};
        }

        const auto ascendancyId = AscendancyOf(*target, state);
        const auto* ascendancy = ascendancyId.empty() ? nullptr : data.FindAscendancy(state.ascendancyId);
        if (!ascendancyId.empty() && (!ascendancy || ascendancyId != ascendancy->id || !ascendancy->startNodeId)) {
            return {};
        }

        std::unordered_map<std::uint32_t, std::uint32_t> parent;
        std::unordered_map<std::uint32_t, std::uint32_t> distance;
        std::deque<std::uint32_t> queue;
        const auto enqueue = [&](std::uint32_t id, std::uint32_t from, std::uint32_t cost) {
            const auto* node = data.FindNode(id);
            if (!node || AscendancyOf(*node, state) != ascendancyId || !HasUnlockRequirements(*node, alloc)) {
                return;
            }
            const bool isStart = ascendancy && id == ascendancy->startNodeId;
            if (!IsAllocatable(*node) && !isStart && !(ascendancy && node->isMultipleChoice)) {
                return;
            }
            const std::uint32_t step = isStart || node->isMultipleChoice || node->freeAlloc || alloc.contains(id) ? 0u : 1u;
            const auto next = cost + step;
            if (const auto old = distance.find(id); old != distance.end() && old->second <= next) {
                return;
            }
            distance[id] = next;
            parent[id] = from;
            if (step == 0) {
                queue.push_front(id);
            } else {
                queue.push_back(id);
            }
        };

        for (const auto id : alloc) {
            enqueue(id, 0, 0);
        }
        if (ascendancy) {
            enqueue(ascendancy->startNodeId, 0, 0);
        } else {
            for (const auto id : BuildActiveNormalSeeds(data, state, alloc)) {
                enqueue(id, 0, 0);
            }
        }

        while (!queue.empty()) {
            const auto id = queue.front();
            queue.pop_front();
            if (id == targetNodeId) {
                std::vector<std::uint32_t> path;
                for (auto current = id; current != 0; current = parent.at(current)) {
                    if (IsAllocatable(*data.FindNode(current)) && !alloc.contains(current)) {
                        path.push_back(current);
                    }
                }
                std::reverse(path.begin(), path.end());
                return path;
            }

            const auto& node = *data.FindNode(id);
            for (const auto neighborId : node.adjacency) {
                const auto* neighbor = data.FindNode(neighborId);
                if (!neighbor) continue;
                if (node.isMultipleChoice) {
                    const bool unlocked = IsHubUnlocked(data, id, alloc);
                    if (unlocked == neighbor->isMultipleChoiceOption) continue;
                }
                if (node.isMultipleChoiceOption && neighbor->isMultipleChoice) continue;
                enqueue(neighborId, id, distance.at(id));
            }
        }
        return {};
    }

    std::int32_t PassiveTree::ComputePathNormalPointCost(
        const PassiveTreeData& data,
        const PassiveTreeState& state,
        std::span<const std::uint32_t> nodeIds)
    {
        const auto alloc = BuildAllocatedSet(state);
        std::unordered_set<std::uint32_t> counted;
        std::int32_t cost = 0;
        for (const auto nid : nodeIds) {
            if (alloc.contains(nid) || !counted.insert(nid).second) {
                continue;
            }
            const auto* node = data.FindNode(nid);
            if (!node || !IsAllocatable(*node) || !AscendancyOf(*node, state).empty() ||
                node->isMultipleChoice || node->freeAlloc) {
                continue;
            }
            ++cost;
        }
        return cost;
    }

    PassivePathPlan PassiveTree::FindNearestAttributeChoicePath(
        const PassiveTreeData& data,
        const PassiveTreeState& state)
    {
        PassivePathPlan plan;
        if (!state.classSelected || state.generation != data.Generation()) {
            return plan;
        }
        const auto alloc = BuildAllocatedSet(state);
        std::unordered_map<std::uint32_t, std::uint32_t> parent;
        std::unordered_map<std::uint32_t, std::uint32_t> distance;
        std::deque<std::uint32_t> queue;
        const auto enqueue = [&](std::uint32_t id, std::uint32_t from, std::uint32_t cost) {
            const auto* node = data.FindNode(id);
            if (!node || !AscendancyOf(*node, state).empty() || !HasUnlockRequirements(*node, alloc)) {
                return;
            }
            if (!IsAllocatable(*node) && !alloc.contains(id) && !node->isMultipleChoice) {
                return;
            }
            const std::uint32_t step = node->isMultipleChoice || node->freeAlloc || alloc.contains(id) ? 0u : 1u;
            const auto next = cost + step;
            if (const auto old = distance.find(id); old != distance.end() && old->second <= next) {
                return;
            }
            distance[id] = next;
            parent[id] = from;
            if (step == 0) {
                queue.push_front(id);
            } else {
                queue.push_back(id);
            }
        };

        for (const auto id : alloc) {
            enqueue(id, 0, 0);
        }
        for (const auto id : BuildActiveNormalSeeds(data, state, alloc)) {
            enqueue(id, 0, 0);
        }

        while (!queue.empty()) {
            const auto id = queue.front();
            queue.pop_front();
            const auto* node = data.FindNode(id);
            if (!node) {
                continue;
            }
            if (!alloc.contains(id) && IsAllocatable(*node) && !node->attrOptions.empty()) {
                for (auto current = id; current != 0; current = parent.at(current)) {
                    const auto* cur = data.FindNode(current);
                    if (cur && IsAllocatable(*cur) && !alloc.contains(current)) {
                        plan.nodes.push_back(current);
                    }
                }
                std::reverse(plan.nodes.begin(), plan.nodes.end());
                plan.targetNodeId = id;
                plan.preferredAttrOptionId = node->attrOptions.back().id;
                plan.pointCost = ComputePathNormalPointCost(data, state, plan.nodes);
                return plan;
            }
            for (const auto neighborId : node->adjacency) {
                const auto* neighbor = data.FindNode(neighborId);
                if (!neighbor) continue;
                if (node->isMultipleChoice) {
                    const bool unlocked = IsHubUnlocked(data, id, alloc);
                    if (unlocked == neighbor->isMultipleChoiceOption) continue;
                }
                if (node->isMultipleChoiceOption && neighbor->isMultipleChoice) continue;
                enqueue(neighborId, id, distance.at(id));
            }
        }
        return plan;
    }

    bool PassiveTree::IsMasteryUnlocked(
        const PassiveTreeData& data,
        const PassiveTreeState& state,
        std::uint32_t masteryNodeId) noexcept
    {
        if (!state.classSelected || state.generation != data.Generation()) {
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

    PassiveNodeView PassiveTree::ResolveNode(const PassiveNodeDef& node, const PassiveTreeState& state) noexcept
    {
        if (const auto choice = state.attributeSelections.find(node.id); choice != state.attributeSelections.end()) {
            if (const auto* option = node.FindAttributeOption(choice->second)) {
                return { option->name, option->stats, node.ascendancyId };
            }
        }
        if (const auto* variant = FindVariant(node, state)) {
            return { variant->name, variant->stats,
                variant->ascendancyId.empty() ? node.ascendancyId : variant->ascendancyId };
        }
        return { node.name, node.stats, node.ascendancyId };
    }

    bool PassiveTree::IsAllocatedJewelSocket(
        const PassiveTreeData& data, const PassiveTreeState& state, std::uint32_t nodeId) noexcept
    {
        const auto* node = data.FindNode(nodeId);
        return state.generation == data.Generation() && node && node->alias.empty() &&
            (node->kind == PassiveNodeKind::Socket || node->jewelSocket) && state.IsAllocated(nodeId);
    }

    bool PassiveTree::IsPhylactery(std::uint32_t generation, std::uint32_t nodeId) noexcept
    {
        return generation == 1 && nodeId == kPhylacteryNodeId;
    }

    unsigned PassiveTree::JewelEffectCopies(
        const PassiveTreeData& data, const PassiveTreeState& state, std::uint32_t nodeId) noexcept
    {
        if (!IsAllocatedJewelSocket(data, state, nodeId)) {
            return 0;
        }
        return IsPhylactery(data.Generation(), nodeId) ? 2 : 1;
    }

    std::vector<std::uint32_t> PassiveTree::RefundDependents(
        const PassiveTreeData& data, const PassiveTreeState& state, std::uint32_t nodeId)
    {
        std::vector<std::uint32_t> dependents;
        if (data.Generation() != state.generation) {
            return dependents;
        }
        for (const auto id : state.allocatedNodes) {
            const auto* node = data.FindNode(id);
            if (node && std::ranges::find(node->unlock, nodeId) != node->unlock.end()) {
                dependents.push_back(id);
            }
        }
        return dependents;
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
        if (!state.allocatedNodes.empty() || !state.masterySelections.empty()) {
            return { false, "할당한 노드를 모두 회수해야 클래스를 변경할 수 있습니다.", 0 };
        }
        state.classSelected = true;
        state.classId = classId;
        state.classContentId = cls->contentId;
        state.generation = data.Generation();
        state.ascendancyId.clear();
        state.attributeSelections.clear();
        return { true, cls->name + " 클래스를 선택했습니다.", 0 };
    }

    PassiveTreeOpResult PassiveTree::SelectClass(
        const PassiveTreeData& data, PassiveTreeState& state, std::string_view classContentId)
    {
        const auto* cls = data.FindClassByContentId(classContentId);
        if (!cls) {
            return { false, "잘못된 클래스 ID입니다.", 0 };
        }
        return SelectClass(data, state, cls->id);
    }

    PassiveTreeOpResult PassiveTree::SelectAscendancy(
        const PassiveTreeData& data,
        PassiveTreeState& state,
        std::string_view ascendancyId,
        const Config& config)
    {
        if (!state.classSelected || state.generation != data.Generation()) {
            return { false, "먼저 시작 클래스를 선택해야 합니다.", 0 };
        }
        const auto* asc = data.FindAscendancy(ascendancyId);
        if (!asc || asc->classId != state.classId) {
            return { false, "현재 클래스에서 선택할 수 없는 전직입니다.", 0 };
        }
        if (ComputeEarnedAscendancyPoints(state, config) < 2) {
            return { false, "전직 포인트가 부족합니다(1차 보스 이정표 달성 필요).", 0 };
        }
        if (state.ascendancyId != asc->id && HasAllocatedAscendancyNodes(data, state)) {
            return { false, "할당한 전직 노드를 모두 회수해야 전직을 변경할 수 있습니다.", 0 };
        }
        state.ascendancyId = asc->id;
        return { true, asc->name + " 전직을 선택했습니다.", 0 };
    }

    PassiveTreeOpResult PassiveTree::ClearAscendancy(
        const PassiveTreeData& data,
        PassiveTreeState& state)
    {
        if (state.generation != data.Generation() || HasAllocatedAscendancyNodes(data, state)) {
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
        std::uint16_t atlasCompletedTierMask,
        const std::unordered_map<std::uint32_t, std::uint32_t>& attributeChoices)
    {
        if (!state.classSelected || state.generation != data.Generation()) {
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

            if (node->kind == PassiveNodeKind::Attribute) {
                const auto choice = attributeChoices.find(nid);
                if (choice == attributeChoices.end() || !node->FindAttributeOption(choice->second)) {
                    return { false, "할당할 노드의 능력치를 선택해야 합니다.", 0 };
                }
                working.attributeSelections[nid] = choice->second;
            }

            if (!node->freeAlloc) {
                const bool isAsc = !AscendancyOf(*node, working).empty();
                WriteSortedAllocated(working, alloc);
                const auto available = isAsc ? ComputeAvailableAscendancyPoints(data, working, config)
                    : ComputeAvailableNormalPoints(data, working, exileLevel, config, atlasCompletedTierMask);
                if (available < 1) {
                    return { false, isAsc ? "전직 포인트가 부족합니다." : "패시브 포인트가 부족합니다.", 0 };
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

    PassiveTreeOpResult PassiveTree::ChangeAttribute(
        const PassiveTreeData& data, PassiveTreeState& state, std::uint32_t nodeId,
        std::uint32_t optionId, const Config& config, std::uint32_t availableRegretOrbs)
    {
        const auto* node = data.FindNode(nodeId);
        const auto current = state.attributeSelections.find(nodeId);
        if (!state.classSelected || state.generation != data.Generation() || !node ||
            node->kind != PassiveNodeKind::Attribute || !node->FindAttributeOption(optionId) ||
            !state.IsAllocated(nodeId) || current == state.attributeSelections.end()) {
            return { false, "할당한 능력치 노드의 선택지만 변경할 수 있습니다." };
        }
        // PoE2는 일반 회수보다 낮은 금화 비용을 받는다. 여기서는 최소 결제 단위가 오브 1개다.
        const std::uint32_t cost = config.freePassiveRefund || current->second == optionId ? 0u : 1u;
        if (availableRegretOrbs < cost) {
            return { false, "능력치를 변경하려면 후회의 오브가 1개 필요합니다." };
        }
        current->second = optionId;
        return { true, "능력치 선택을 변경했습니다.", 0, cost };
    }

    std::uint32_t PassiveTree::GetRefundNodeCost(
        const PassiveTreeData& data,
        const PassiveTreeState& state,
        std::uint32_t nodeId,
        const Config& config)
    {
        if (config.freePassiveRefund || !state.classSelected || state.generation != data.Generation()) {
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
        if (!node || node->freeAlloc) {
            return 0;
        }
        if (!AscendancyOf(*node, state).empty()) {
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
        if (config.freePassiveRefund || !state.classSelected || state.generation != data.Generation()) {
            return 0;
        }
        const std::uint32_t ascUnit = std::max<std::uint32_t>(1, config.ascendancyRefundCost);
        std::uint32_t cost = 0;
        for (const auto nid : state.allocatedNodes) {
            const auto* node = data.FindNode(nid);
            if (!node || node->freeAlloc) continue;
            if (AscendancyOf(*node, state).empty()) {
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
        if (!state.classSelected || state.generation != data.Generation()) {
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
        if (!state.classSelected || state.generation != data.Generation()) {
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
        if (!RefundDependents(data, state, nodeId).empty()) {
            return { false, "이 노드가 해금한 패시브를 먼저 회수해야 합니다.", 0, 0 };
        }
        alloc.erase(nodeId);
        working.attributeSelections.erase(nodeId);

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
        if (!config.freePassiveRefund && !targetNode->freeAlloc) {
            if (!AscendancyOf(*targetNode, state).empty()) {
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
        if (!state.classSelected || state.generation != data.Generation()) {
            return { false, "선택된 클래스가 없습니다.", 0, 0 };
        }

        const std::uint32_t orbCost = GetRefundAllCost(data, state, ascendancyOnly, config);
        if (!ascendancyOnly) {
            if (!config.freePassiveRefund && orbCost > 0 && availableRegretOrbs < orbCost) {
                return { false, "후회의 오브가 부족합니다 (필요: " + std::to_string(orbCost) + "개).", 0, 0 };
            }
            state.allocatedNodes.clear();
            state.attributeSelections.clear();
            state.masterySelections.clear();
            state.ascendancyId.clear();
            return { true, "모든 패시브 노드를 회수했습니다.", 0, orbCost };
        }

        // 검수 M3, m5: 전직만 회수 시 연결성·부여 포인트 검증을 먼저 수행한 뒤 후회의 오브 잔량 확인
        PassiveTreeState working = state;
        std::unordered_set<std::uint32_t> remaining;
        for (const auto nid : working.allocatedNodes) {
            const auto* node = data.FindNode(nid);
            if (node && AscendancyOf(*node, working).empty()) {
                remaining.insert(nid);
            }
        }
        working.ascendancyId.clear();
        if (!VerifyConnectivity(data, working, remaining)) {
            return { false, "전직 시작점에 연결된 일반 노드를 먼저 회수하거나 전체 회수를 사용해 주세요.", 0, 0 };
        }
        WriteSortedAllocated(working, remaining);
        PruneAttributeSelections(working, remaining);
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

    bool PassiveTree::SanitizeLoadedState(
        const PassiveTreeData& data,
        PassiveTreeState& state)
    {
        if (!data.IsLoaded() || state.generation != data.Generation()) {
            return false;
        }
        const PassiveClassDef* cls = nullptr;
        if (state.classSelected) {
            cls = state.classContentId.empty() && state.generation == 0 ? data.FindClass(state.classId)
                : data.FindClassByContentId(state.classContentId);
        }
        if (!cls) {
            state.classSelected = false;
            state.classId = 0;
            state.classContentId.clear();
            state.ascendancyId.clear();
            state.allocatedNodes.clear();
            state.attributeSelections.clear();
            state.masterySelections.clear();
            return true;
        }
        state.classId = cls->id;
        state.classContentId = cls->contentId;
        const auto* asc = data.FindAscendancy(state.ascendancyId);
        state.ascendancyId = asc && asc->classId == state.classId ? asc->id : std::string{};

        std::unordered_set<std::uint32_t> alloc;
        std::unordered_set<std::uint32_t> usedHubs;
        for (const auto nid : state.allocatedNodes) {
            const auto* node = data.FindNode(nid);
            if (!node || alloc.contains(nid) || !IsAllocatable(*node)) continue;
            const auto ascendancyId = AscendancyOf(*node, state);
            if (!ascendancyId.empty() && ascendancyId != state.ascendancyId) continue;
            if (node->kind == PassiveNodeKind::Attribute) {
                const auto choice = state.attributeSelections.find(nid);
                if (choice == state.attributeSelections.end() || !node->FindAttributeOption(choice->second)) continue;
            }
            if (node->isMultipleChoiceOption &&
                (node->choiceParentId == 0 || !usedHubs.insert(node->choiceParentId).second)) {
                continue;
            }
            alloc.insert(nid);
        }

        // Only reachable prerequisites can open a locked node, including across the two trees.
        std::unordered_set<std::uint32_t> kept;
        while (true) {
            auto next = FindReachableAllocated(data, state, alloc, kept);
            if (next == kept) break;
            kept = std::move(next);
        }
        WriteSortedAllocated(state, kept);
        std::erase_if(state.attributeSelections, [&](const auto& selection) {
            return !kept.contains(selection.first) || data.FindNode(selection.first)->kind != PassiveNodeKind::Attribute;
        });

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
        return true;
    }
}
