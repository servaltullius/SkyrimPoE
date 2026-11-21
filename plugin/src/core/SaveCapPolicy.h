#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

namespace SkyrimPoE::Core
{
    // 재검수 R-3: 코세이브 목록 상한을 넘었을 때 남길 항목 고르기.
    // 추적 장부는 만든 순서로 쌓이고, 판 물건·남에게 준 물건도 지워지지 않는다. 그래서 "최신 우선"으로 자르면
    // 오래 들고 있던(지금 착용 중인) 장비가 먼저 빠진다. 대신 다음 순서로 남긴다.
    //   1. 플레이어가 가진 것(바닥이 아님)
    //   2. 바닥에 놓인 것(다시 주울 수 있음)
    //   3. 그 밖의 소유자(상인·상자·NPC·시체)
    // 같은 순위 안에서는 최신 항목(뒤쪽)을 남긴다. 결과는 원래 순서(오름차순)의 인덱스 목록이다.
    struct SaveCapCandidate
    {
        std::uint32_t ownerFormId = 0;
        bool isWorld = false;
    };

    [[nodiscard]] inline std::vector<std::size_t> SelectEntriesWithinSaveCap(
        std::span<const SaveCapCandidate> entries,
        std::size_t cap,
        std::uint32_t playerFormId)
    {
        std::vector<std::size_t> idx(entries.size());
        std::iota(idx.begin(), idx.end(), std::size_t{ 0 });
        if (entries.size() <= cap) {
            return idx;
        }
        const auto rank = [&](std::size_t i) -> int {
            const auto& e = entries[i];
            if (!e.isWorld && e.ownerFormId == playerFormId) {
                return 0;
            }
            if (e.isWorld) {
                return 1;
            }
            return 2;
        };
        std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
            const int ra = rank(a);
            const int rb = rank(b);
            if (ra != rb) {
                return ra < rb;
            }
            return a > b;  // 같은 순위면 최신(뒤쪽) 먼저
        });
        idx.resize(cap);
        std::sort(idx.begin(), idx.end());
        return idx;
    }
}
