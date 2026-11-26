#pragma once

#include "core/PassiveTreeData.h"
#include "core/Types.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SkyrimPoE::Core
{
    struct PassiveTreeOpResult
    {
        bool ok = false;
        std::string message;
        std::size_t autoRefundedMasteries = 0;
        std::uint32_t regretOrbsConsumed = 0;  // 4단계(결정 152, 158): FreePassiveRefund=false 시 소비할 후회의 오브 수
    };

    struct PassiveNodeView
    {
        std::string_view name;
        std::span<const std::string> stats;
        std::string_view ascendancyId;
    };

    struct PassivePathPlan
    {
        std::vector<std::uint32_t> nodes;
        std::uint32_t targetNodeId = 0;
        std::uint32_t preferredAttrOptionId = 0;
        std::int32_t pointCost = 0;
    };

    // 결정 8, 10, 151~153:
    // 패시브 트리 규칙 엔진(엔진 비의존).
    // - 포인트는 부호 있는 int32_t 파생 계산을 단일 진실 원천으로 사용해 언더플로 차단.
    // - classStart는 시드 전용으로 분리해 타 클래스 시작점 우회·할당 차단.
    // - isMultipleChoice(7개 허브)는 0포인트, 자식 isMultipleChoiceOption 중 허브당 최대 1개만 할당.
    // - Scion Ascendant의 Path of the ... 6개 노드는 타 클래스 시작점 시드를 동적 추가.
    // - 마스터리는 그룹 주요 패시브 1개 이상 할당 시 해금, 전역 effectId 중복 금지, 근거 주요 패시브 전량 회수 시 자동 회수.
    class PassiveTree
    {
    public:
        [[nodiscard]] static std::int32_t ComputeEarnedAscendancyPoints(
            const PassiveTreeState& state,
            const Config& config) noexcept;

        [[nodiscard]] static std::int32_t ComputeGrantedPassivePoints(
            const PassiveTreeData& data,
            const PassiveTreeState& state) noexcept;

        [[nodiscard]] static std::int32_t ComputeSpentNormalPoints(
            const PassiveTreeData& data,
            const PassiveTreeState& state) noexcept;

        [[nodiscard]] static std::int32_t ComputeSpentAscendancyPoints(
            const PassiveTreeData& data,
            const PassiveTreeState& state) noexcept;

        [[nodiscard]] static std::int32_t ComputeAvailableNormalPoints(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            std::uint32_t exileLevel,
            const Config& config,
            std::uint16_t atlasCompletedTierMask = 0) noexcept;

        [[nodiscard]] static std::int32_t ComputeAvailableAscendancyPoints(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            const Config& config) noexcept;

        // 호버 미리보기·클릭 일괄 할당용 최단 경로(반환값에는 실제 소비 노드 ID만 포함, 시작점·0점 허브 제외)
        [[nodiscard]] static std::vector<std::uint32_t> ComputeShortestPath(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            std::uint32_t targetNodeId);

        // 주어진 노드 경로를 현재 상태에서 할당할 때 필요한 일반 패시브 점수 비용(freeAlloc·이미 할당된 노드·중복은 0점).
        [[nodiscard]] static std::int32_t ComputePathNormalPointCost(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            std::span<const std::uint32_t> nodeIds);

        // 현재 상태에서 일반 패시브 점수 비용이 가장 적은 미할당 능력치 선택 노드와 그 경로·비용을 구한다.
        [[nodiscard]] static PassivePathPlan FindNearestAttributeChoicePath(
            const PassiveTreeData& data,
            const PassiveTreeState& state);

        // 마스터리 노드 해금 여부(해당 그룹의 주요 패시브가 1개 이상 할당되어 있어야 함)
        [[nodiscard]] static bool IsMasteryUnlocked(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            std::uint32_t masteryNodeId) noexcept;

        [[nodiscard]] static PassiveNodeView ResolveNode(
            const PassiveNodeDef& node, const PassiveTreeState& state) noexcept;
        [[nodiscard]] static bool IsAllocatedJewelSocket(
            const PassiveTreeData& data, const PassiveTreeState& state, std::uint32_t nodeId) noexcept;
        static constexpr std::uint32_t kPhylacteryNodeId = 17788;
        [[nodiscard]] static bool IsPhylactery(std::uint32_t generation, std::uint32_t nodeId) noexcept;
        [[nodiscard]] static unsigned JewelEffectCopies(
            const PassiveTreeData& data, const PassiveTreeState& state, std::uint32_t nodeId) noexcept;
        [[nodiscard]] static std::vector<std::uint32_t> RefundDependents(
            const PassiveTreeData& data, const PassiveTreeState& state, std::uint32_t nodeId);

        // 클래스 선택/재선택 (미선택이거나 소비 노드 0개일 때만 허용, 전직 함께 초기화)
        static PassiveTreeOpResult SelectClass(
            const PassiveTreeData& data,
            PassiveTreeState& state,
            std::uint8_t classId);
        static PassiveTreeOpResult SelectClass(
            const PassiveTreeData& data, PassiveTreeState& state, std::string_view classContentId);

        // 전직 선택 (클래스 선택됨, 전직 포인트 >= 2, 소속 전직, 기존 전직 소비 노드 0개)
        static PassiveTreeOpResult SelectAscendancy(
            const PassiveTreeData& data,
            PassiveTreeState& state,
            std::string_view ascendancyId,
            const Config& config);

        // 전직 해제 (전직 소비 노드 0개일 때)
        static PassiveTreeOpResult ClearAscendancy(
            const PassiveTreeData& data,
            PassiveTreeState& state);

        // 단일 노드 또는 경로 일괄 할당 (원자 처리)
        static PassiveTreeOpResult AllocatePath(
            const PassiveTreeData& data,
            PassiveTreeState& state,
            const std::vector<std::uint32_t>& nodeIds,
            std::uint32_t exileLevel,
            const Config& config,
            std::uint16_t atlasCompletedTierMask = 0,
            const std::unordered_map<std::uint32_t, std::uint32_t>& attributeChoices = {});

        static PassiveTreeOpResult ChangeAttribute(
            const PassiveTreeData& data, PassiveTreeState& state, std::uint32_t nodeId,
            std::uint32_t optionId, const Config& config, std::uint32_t availableRegretOrbs = 0);

        // 4단계(결정 152, 158): 후회의 오브 필요 비용 조회(FreePassiveRefund=true면 0)
        [[nodiscard]] static std::uint32_t GetRefundNodeCost(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            std::uint32_t nodeId,
            const Config& config);

        [[nodiscard]] static std::uint32_t GetRefundAllCost(
            const PassiveTreeData& data,
            const PassiveTreeState& state,
            bool ascendancyOnly,
            const Config& config);

        [[nodiscard]] static std::uint32_t GetSelectMasterySwapCost(
            const PassiveTreeState& state,
            std::uint32_t masteryNodeId,
            std::uint32_t effectId,
            const Config& config) noexcept;

        // 마스터리 효과 선택 (그룹 주요 패시브 해금 + 전역 effectId 중복 금지; 교체 시 후회의 오브 1개)
        static PassiveTreeOpResult SelectMastery(
            const PassiveTreeData& data,
            PassiveTreeState& state,
            std::uint32_t masteryNodeId,
            std::uint32_t effectId,
            std::uint32_t exileLevel,
            const Config& config,
            std::uint32_t availableRegretOrbs = 0,
            std::uint16_t atlasCompletedTierMask = 0);

        // 단일 노드 또는 마스터리 회수 (연결성 BFS + 근거 주요 패시브 상실 마스터리 자동 회수 + grantedPts 검증)
        static PassiveTreeOpResult RefundNode(
            const PassiveTreeData& data,
            PassiveTreeState& state,
            std::uint32_t nodeId,
            std::uint32_t exileLevel,
            const Config& config,
            std::uint32_t availableRegretOrbs = 0,
            std::uint16_t atlasCompletedTierMask = 0);

        // 전체 회수 또는 전직만 회수 (ascendancyOnly=true 시 일반 트리 연결성과 포인트 잔액 검증)
        static PassiveTreeOpResult RefundAll(
            const PassiveTreeData& data,
            PassiveTreeState& state,
            bool ascendancyOnly,
            std::uint32_t exileLevel,
            const Config& config,
            std::uint32_t availableRegretOrbs = 0,
            std::uint16_t atlasCompletedTierMask = 0);

        // 코세이브 로드 후 미존재 노드·중복 마스터리·단절 노드 정리
        static bool SanitizeLoadedState(
            const PassiveTreeData& data,
            PassiveTreeState& state);
    };
}
