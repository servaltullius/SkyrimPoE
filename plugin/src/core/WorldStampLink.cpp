#include "core/WorldStampLink.h"

#include <unordered_map>

namespace SkyrimPoE::Core::WorldStampLink
{
    bool MatchesPickupEvent(
        const InventoryItemKey& worldKey,
        bool isWorld,
        std::string_view entryKindId,
        std::uint16_t eventStamp,
        std::string_view eventKindId,
        StampBand band) noexcept
    {
        return isWorld &&
            worldKey.IsValid() &&
            band.Contains(eventStamp) &&
            worldKey.uniqueId == eventStamp &&
            !eventKindId.empty() &&
            entryKindId == eventKindId;
    }

    bool MatchesWorldEntry(
        const InventoryItemKey& key,
        bool isWorld,
        std::uint32_t expectedGroundRefId,
        std::uint16_t expectedStamp,
        StampBand band) noexcept
    {
        return isWorld &&
            expectedGroundRefId != 0 &&
            band.Contains(expectedStamp) &&
            key.ownerFormId == expectedGroundRefId &&
            key.uniqueId == expectedStamp;
    }

    const WorldLinkCandidate* FindWorldLinkTarget(
        const std::vector<WorldLinkCandidate>& candidates,
        const InventoryItemKey& extraKey,
        std::string_view kindId,
        StampBand band) noexcept
    {
        if (!extraKey.IsValid() || !band.Contains(extraKey.uniqueId) || kindId.empty()) {
            return nullptr;
        }
        const WorldLinkCandidate* matched = nullptr;
        for (const auto& cand : candidates) {
            if (cand.worldKey == extraKey && cand.kindId == kindId) {
                if (matched != nullptr) {
                    // 동일 키·종류 후보가 둘 이상이면 모호하므로 연결하지 않는다.
                    return nullptr;
                }
                matched = &cand;
            }
        }
        return matched;
    }

    RelinkPlanResult PlanRelinks(
        const std::vector<WorldLinkCandidate>& candidates,
        const std::vector<ObservedInventoryExtra>& extras,
        StampBand band,
        std::uint32_t postLoadNormalizedOwnerId)
    {
        RelinkPlanResult result;
        if (candidates.empty() || extras.empty()) {
            return result;
        }

        struct CandidateHit
        {
            std::size_t extraIndex = 0;
            std::size_t candIndex = 0;
            InventoryItemKey observedExtraKey;
        };
        std::vector<CandidateHit> provisional;
        std::unordered_map<std::size_t, std::size_t> hitsPerCandidate;

        for (const auto& ex : extras) {
            if (!ex.extraKey.IsValid() || !band.Contains(ex.extraKey.uniqueId) || ex.kindId.empty()) {
                continue;
            }
            // 1차: (바닥 참조 FormID, 스탬프) 정확 일치 후보를 센다.
            std::size_t matchCount = 0;
            std::size_t matchedCandIdx = 0;
            for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
                if (candidates[ci].worldKey == ex.extraKey && candidates[ci].kindId == ex.kindId) {
                    ++matchCount;
                    matchedCandIdx = ci;
                }
            }
            // 2차(PostLoadInit 전용): .ess 로더가 삭제된 FF 바닥 참조 baseID를 컨테이너 소유자로 정규화한 경우
            // 스탬프 대역 uniqueID + kindId가 유일 일치할 때만 연결한다(런타임에서는 postLoadNormalizedOwnerId == 0).
            if (matchCount == 0 && postLoadNormalizedOwnerId != 0 &&
                ex.extraKey.ownerFormId == postLoadNormalizedOwnerId) {
                for (std::size_t ci = 0; ci < candidates.size(); ++ci) {
                    if (candidates[ci].worldKey.uniqueId == ex.extraKey.uniqueId &&
                        candidates[ci].kindId == ex.kindId) {
                        ++matchCount;
                        matchedCandIdx = ci;
                    }
                }
            }
            if (matchCount == 0) {
                continue;
            }
            // 검수 Minor 5: 동일 후보에 count>1 묶음과 count==1 묶음이 공존할 때도 모호성으로 보류되도록 먼저 카운트 반영
            if (matchCount == 1) {
                ++hitsPerCandidate[matchedCandIdx];
            }
            if (ex.count > 1) {
                result.heldCountGtOne.push_back(ex.extraIndex);
                continue;
            }
            if (matchCount > 1) {
                result.heldAmbiguous.push_back(ex.extraIndex);
                continue;
            }
            provisional.push_back(CandidateHit{ ex.extraIndex, matchedCandIdx, ex.extraKey });
        }

        for (const auto& hit : provisional) {
            if (hitsPerCandidate[hit.candIndex] > 1) {
                result.heldAmbiguous.push_back(hit.extraIndex);
                continue;
            }
            const auto& cand = candidates[hit.candIndex];
            result.planned.push_back(PlannedRelink{
                hit.extraIndex, cand.instanceId, cand.worldKey, hit.observedExtraKey });
        }
        return result;
    }

    bool CommitRelink(
        InventoryItemKey& key,
        bool& isWorld,
        const InventoryItemKey& expectedWorldKey,
        const InventoryItemKey& newInventoryKey) noexcept
    {
        if (!isWorld || !expectedWorldKey.IsValid() || key != expectedWorldKey || !newInventoryKey.IsValid()) {
            return false;
        }
        key = newInventoryKey;
        isWorld = false;
        return true;
    }
}
