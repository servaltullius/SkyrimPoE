#pragma once

#include "core/Types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// 결정 59·123: 바닥을 거친 추적 아이템(데이터 젬·PoE 아이템)을 다시 잇는 엔진 비의존 판단 규칙.
// - 버릴 때: 고유 ID 변경 이벤트가 (가진 쪽:번호 → 바닥 참조:0)이면 바닥 참조에 전용 대역 스탬프를 남긴다.
// - 주울 때: 컨테이너 변경 이벤트(바닥 → 컨테이너, itemCount > 0)의 고유 ID 칸에 실린 스탬프와,
//   새 소유자 인벤토리의 (바닥 참조:스탬프) 묶음 및 종류(kindId)가 정확히 일치할 때만 새 인벤토리 번호로 옮긴다.
namespace SkyrimPoE::Core::WorldStampLink
{
    // 추적기 하나가 바닥 스탬프로 쓰는 고유 ID 대역(양 끝 포함). 0은 엔진의 "번호 없음"이라 항상 제외한다.
    struct StampBand
    {
        std::uint16_t min = 0;
        std::uint16_t max = 0;

        [[nodiscard]] constexpr bool Contains(std::uint16_t uniqueId) const noexcept
        {
            return uniqueId != 0 && min != 0 && min <= max && uniqueId >= min && uniqueId <= max;
        }
    };

    // 고유 ID 변경 이벤트의 새 키가 "바닥 참조:0"(버림)인가.
    // 주운 뒤 다시 잇기 전에 또 버린 경우(직전 키가 이미 바닥 키)에도 새 바닥 참조에 스탬프가 필요하다.
    [[nodiscard]] constexpr bool NeedsWorldStamp(std::uint32_t newBaseId, std::uint16_t newUniqueId) noexcept
    {
        return newBaseId != 0 && newUniqueId == 0;
    }

    // 컨테이너 변경 이벤트가 "바닥(0)에서 컨테이너로 들어온, 수량 > 0, 이 대역 스탬프를 단 묶음"인가.
    [[nodiscard]] constexpr bool IsStampedWorldPickup(
        std::uint32_t oldContainer,
        std::uint32_t newContainer,
        std::uint16_t uniqueId,
        std::int32_t itemCount,
        StampBand band) noexcept
    {
        return oldContainer == 0 && newContainer != 0 && itemCount > 0 && band.Contains(uniqueId);
    }

    // 주울 때 이벤트 자리의 대조: 바닥 항목의 스탬프와 종류가 이벤트와 같은가(이벤트에는 바닥 참조가 실려 오지 않음).
    [[nodiscard]] bool MatchesPickupEvent(
        const InventoryItemKey& worldKey,
        bool isWorld,
        std::string_view entryKindId,
        std::uint16_t eventStamp,
        std::string_view eventKindId,
        StampBand band) noexcept;

    // 보완 스탬프 실행 전 재검증: 항목이 아직 기대한 (바닥 참조, 스탬프) 바닥 상태에 있는가.
    [[nodiscard]] bool MatchesWorldEntry(
        const InventoryItemKey& key,
        bool isWorld,
        std::uint32_t expectedGroundRefId,
        std::uint16_t expectedStamp,
        StampBand band) noexcept;

    // 다시 잇기 후보: 바닥에 있는 추적 항목 하나.
    struct WorldLinkCandidate
    {
        std::uint64_t instanceId = 0;
        InventoryItemKey worldKey;  // (바닥 참조 FormID, 스탬프)
        std::string kindId;         // 젬이면 gemId, 아이템이면 베이스 FormID 식별 문자열
    };

    // 인벤토리 묶음 관측 레코드(엔진 ExtraDataList에서 추출).
    struct ObservedInventoryExtra
    {
        std::size_t extraIndex = 0;
        InventoryItemKey extraKey;
        std::string kindId;
        std::int32_t count = 1;
    };

    struct PlannedRelink
    {
        std::size_t extraIndex = 0;
        std::uint64_t instanceId = 0;
        InventoryItemKey worldKey;
        InventoryItemKey observedExtraKey;
    };

    struct RelinkPlanResult
    {
        std::vector<PlannedRelink> planned;
        std::vector<std::size_t> heldCountGtOne;
        std::vector<std::size_t> heldAmbiguous;
    };

    // 인벤토리 묶음의 ExtraUniqueID(extraKey)와 종류(kindId)에 정확히 맞는 바닥 후보를 찾는다.
    // 결정 59: 바닥 참조 FormID와 스탬프를 함께 대조하며, (소유자, 스탬프) 같은 2순위 폴백은 두지 않는다.
    [[nodiscard]] const WorldLinkCandidate* FindWorldLinkTarget(
        const std::vector<WorldLinkCandidate>& candidates,
        const InventoryItemKey& extraKey,
        std::string_view kindId,
        StampBand band) noexcept;

    // 후보 목록과 관측된 인벤토리 묶음 전체를 대조해 다시 잇기 계획을 만든다.
    // - 묶음 개수가 1보다 크면 보류(heldCountGtOne)
    // - 동일 후보에 맞는 묶음이 2개 이상이거나, 한 묶음에 맞는 후보가 2개 이상이면 모두 보류(heldAmbiguous)
    // - postLoadNormalizedOwnerId == 0(일반 런타임): (바닥 참조 FormID, 스탬프) 정확 일치만 허용(2순위 폴백 없음).
    // - postLoadNormalizedOwnerId != 0(세이브 로드 직후 PostLoadInit 전용): 실게임 AE 1.6.1170에서 확인된
    //   .ess 로더의 baseID 정규화(삭제된 FF 바닥 참조 → 소유자 FormID, 스탬프 대역 uniqueID 보존)에 한해
    //   정확 일치가 없을 때 (소유자 FormID, 스탬프 대역) + kindId 1:1 유일 일치를 허용한다.
    [[nodiscard]] RelinkPlanResult PlanRelinks(
        const std::vector<WorldLinkCandidate>& candidates,
        const std::vector<ObservedInventoryExtra>& extras,
        StampBand band,
        std::uint32_t postLoadNormalizedOwnerId = 0);

    // 바닥 키를 새 소유자의 인벤토리 키로 옮긴다. 항목이 아직 기대한 바닥 키일 때만 바꾸고 true를 반환한다.
    // 이미 이어졌거나 그사이 다른 이벤트로 키가 바뀐 경우, 새 키가 무효한 경우 아무것도 바꾸지 않는다.
    [[nodiscard]] bool CommitRelink(
        InventoryItemKey& key,
        bool& isWorld,
        const InventoryItemKey& expectedWorldKey,
        const InventoryItemKey& newInventoryKey) noexcept;
}
