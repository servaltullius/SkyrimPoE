#pragma once

#include <cstdint>
#include <functional>
#include <unordered_set>

namespace SkyrimPoE::Runtime::UniqueIdUtil
{
    // 검수 C2: GemInventory와 ItemTracker 간 UID 대역 충돌을 원천 차단하는 공용 대역 분할
    inline constexpr std::uint16_t kInventoryUidMin = 0x7000;
    inline constexpr std::uint16_t kInventoryUidMax = 0x7EFF;
    inline constexpr std::uint16_t kGemWorldStampMin = 0x7F00;
    inline constexpr std::uint16_t kGemWorldStampMax = 0xBFFF;
    inline constexpr std::uint16_t kItemWorldStampMin = 0xC000;
    inline constexpr std::uint16_t kItemWorldStampMax = 0xFFFD;

    // CommonLibSSE-NG v16.4.0 듀얼 SE+AE 빌드 안전 ExtraDataList 할당/해제기(SE 0x18, AE 1.6.629+ 0x20).
    // new RE::ExtraDataList() 호출 금지 — 반드시 이 함수 쌍을 사용한다.
    [[nodiscard]] RE::ExtraDataList* CreateGameExtraDataList();
    void DestroyGameExtraDataList(RE::ExtraDataList* list);

    // 각 추적기(GemInventory, ItemTracker)가 자신이 사용 중인 인벤토리 UID를 보고하도록 콜백 등록
    void RegisterUsedInventoryUidCollector(std::function<void(std::unordered_set<std::uint16_t>&)> collector);

    // owner 인벤토리의 엔진 ExtraUniqueID + 등록된 모든 추적기의 인벤토리 UID를 피해 미사용 번호 할당
    [[nodiscard]] std::uint16_t AllocateInventoryUniqueId(RE::TESObjectREFR* owner);
}
