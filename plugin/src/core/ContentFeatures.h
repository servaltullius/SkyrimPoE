#pragma once

#include <cstdint>

namespace SkyrimPoE::Core
{
    // 설치된 패키지의 콘텐츠 세대로 정해지는 기능(7단계 G1, 결정 239·244·245·248·249).
    // 세대 1 패키지는 P5·P6b 전까지 아이템·제작·지도·참·몬스터 승격을 끈다. "새로 만드는" 지점만 막고,
    // 경험치·보류 중인 구판 드롭 반환·지도 탈출·안전 제거·트리 회수는 그대로 둔다(결정 248).
    struct ContentFeatures
    {
        bool itemDrops = true;         // 처치 전리품·공물 지급
        bool crafting = true;          // 감정·화폐·쥬얼러·고유 감정·반지·벨트·참 장착
        bool equipmentStats = true;    // 장비의 PoE 수치와 PoE1 쌍수 고유 보너스(결정 244)
        bool mapEntry = true;          // 새 지도 입장·결투장·재입장(탈출·포기·귀환·안전 제거는 허용)
        bool charms = true;            // 참의 발동·충전·효과
        bool monsterPromotion = true;  // 자연 승격 추첨(결정 245)
        bool jewelSocketing = true;    // 패시브 주얼 장착(회수는 허용, 결정 249)
        bool starterCurrency = true;   // PoE1 시작 화폐(결정 240)
    };

    [[nodiscard]] constexpr ContentFeatures ContentFeaturesFor(std::uint32_t packageGeneration) noexcept
    {
        const bool legacy = packageGeneration == 0;
        return { legacy, legacy, legacy, legacy, legacy, legacy, legacy, legacy };
    }

    // 콘텐츠 검사가 통과한 뒤 main이 한 번 정한다. 정하기 전(단위 시험 포함)은 세대 0이다.
    void SetActiveContentGeneration(std::uint32_t packageGeneration) noexcept;
    [[nodiscard]] std::uint32_t ActiveContentGeneration() noexcept;
    [[nodiscard]] ContentFeatures ActiveContentFeatures() noexcept;
}
