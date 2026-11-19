#pragma once

#include "core/DataPack.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace SkyrimPoE::Core
{
    // GemTransaction이 엔진/모의 인벤토리와 통신하기 위한 추상 인터페이스(단위 테스트 가능)
    class IGemInventoryAdapter
    {
    public:
        virtual ~IGemInventoryAdapter() = default;

        // 추적 중인 데이터 젬(key.IsValid()) 또는 순정 젬(fallbackGemId) 1개를 인벤토리에서 제거하고 인스턴스를 돌려준다.
        virtual std::optional<GemInstance> TakeGem(
            const InventoryItemKey& key,
            std::string_view fallbackGemId,
            std::uint8_t defaultSockets,
            std::uint64_t& nextInstanceId) = 0;

        // 데이터 젬(레벨·보조·담긴 주문 보존)을 ExtraUniqueID와 함께 인벤토리로 반환한다.
        virtual std::optional<InventoryItemKey> ReturnGem(GemInstance gem) = 0;
    };

    struct GemTransactionResult
    {
        bool ok = false;
        std::string message;
        std::optional<InventoryItemKey> returnedKey;
    };

    // 결정 59, 105~108, 131, 132, 137(마), 145:
    // 주 갱신 한 틱 안에서 확인 → 인벤토리 제거/반환 → 슬롯 갱신 → 실패 시 롤백을 원자적으로 수행한다.
    class GemTransaction
    {
    public:
        // 메인 스킬 칸(0..8)에 젬 장착(2단계: Cyclone 또는 Trigger 젬 허용; 이미 차 있으면 기존 젬 반환)
        static GemTransactionResult EquipSkillGem(
            std::array<SkillSlot, kSkillSlotCount>& slots,
            std::uint64_t& nextInstanceId,
            std::uint8_t slotIndex,
            const InventoryItemKey& itemKey,
            std::string_view gemId,
            std::uint8_t defaultSockets,
            const DataPack& pack,
            IGemInventoryAdapter& inventory);

        // 메인 스킬 칸(0..8)의 젬 탈착(보조 젬·발동 안 주문/보조 함께 보관해 인벤토리로 반환)
        static GemTransactionResult UnequipSkillGem(
            std::array<SkillSlot, kSkillSlotCount>& slots,
            std::uint8_t slotIndex,
            IGemInventoryAdapter& inventory);

        // 소켓 장착:
        // - triggeredSpellIndex < 0: slots[slotIndex].gem 소켓에 장착(일반 스킬이면 보조, 발동 젬이면 주문)
        // - triggeredSpellIndex >= 0: 발동 젬 안의 주문 젬(socketedGems[triggeredSpellIndex]) 소켓에 보조 젬 장착
        static GemTransactionResult SocketSubGem(
            std::array<SkillSlot, kSkillSlotCount>& slots,
            std::uint64_t& nextInstanceId,
            std::uint8_t slotIndex,
            std::int32_t triggeredSpellIndex,
            const InventoryItemKey& itemKey,
            std::string_view gemId,
            std::uint8_t defaultSockets,
            const DataPack& pack,
            IGemInventoryAdapter& inventory);

        // 소켓 탈착: 보조 젬 또는 발동 안 주문 젬(자체 보조 포함)을 인벤토리로 반환
        static GemTransactionResult UnsocketSubGem(
            std::array<SkillSlot, kSkillSlotCount>& slots,
            std::uint8_t slotIndex,
            std::int32_t triggeredSpellIndex,
            std::size_t socketIndex,
            IGemInventoryAdapter& inventory);

        // 발동 젬의 연결 공격 칸 변경(결정 100, 131: 공격 1칸당 발동 젬 최대 1개; attackSlotIndex < 0이면 해제)
        static GemTransactionResult LinkTriggerAttack(
            std::array<SkillSlot, kSkillSlotCount>& slots,
            std::uint8_t triggerSlotIndex,
            std::int8_t attackSlotIndex,
            const DataPack& pack);
    };
}
