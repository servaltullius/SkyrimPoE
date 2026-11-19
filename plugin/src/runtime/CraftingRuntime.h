#pragma once

#include "core/DataPack.h"
#include "core/ItemData.h"
#include "core/ItemTypes.h"
#include "core/PassiveTree.h"
#include "core/PassiveTreeData.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimPoE::Runtime
{
    struct CraftActionResult
    {
        bool ok = false;
        std::string message;
        RE::FormID resultBaseFormId = 0;
        Core::InventoryItemKey resultKey{};
        Core::PoEItemRecord resultRecord{};
    };

    struct PlayerBagItemEntry
    {
        RE::FormID baseFormId = 0;
        std::uint16_t uniqueId = 0;  // 0이면 미추적 일반 스택 아이템
        std::int32_t count = 1;
        bool equipped = false;
        std::string displayName;
        Core::EquipSlot equipSlot = Core::EquipSlot::MainHand;
        Core::TokenSlotGroup slotGroup = Core::TokenSlotGroup::OneHandWeapon;
        Core::DefenseSubtype defenseSubtype = Core::DefenseSubtype::None;
        bool isAmulet = false;
        std::string weaponTag;
        bool hasPoERecord = false;
        Core::PoEItemRecord record{};
    };

    struct PlayerTokenStackEntry
    {
        RE::FormID localFormId = 0;
        std::uint8_t tier = 1;
        Core::ItemRarity rarity = Core::ItemRarity::Magic;
        Core::TokenSlotGroup slotGroup = Core::TokenSlotGroup::OneHandWeapon;
        std::int32_t count = 0;
        std::string displayName;
    };

    // 결정 16, 152, 157, 158, 통합 설계서 17장:
    // 모든 감정·오브 제작·젬 소켓 확장·후회의 오브 패시브 회수를 주 갱신 스레드 + RunSerializedTransaction 안에서
    // 원자적으로 수행한다(검증 통과 후에만 차감, 실패 시 롤백).
    class CraftingRuntime
    {
    public:
        static void Initialize(
            const Core::ItemData* itemData,
            const Core::DataPack* dataPack,
            const Core::PassiveTreeData* treeData,
            const Core::Config* config);

        // 화폐·토큰 인벤토리 헬퍼(주 갱신 스레드 전용)
        [[nodiscard]] static std::int32_t CountCurrency(RE::TESObjectREFR* owner, Core::CurrencyKind kind);
        [[nodiscard]] static std::array<std::int32_t, Core::kCurrencyKindCount> CountAllCurrencies(RE::TESObjectREFR* owner);
        static bool GrantCurrency(RE::TESObjectREFR* owner, Core::CurrencyKind kind, std::int32_t count);
        static bool ConsumeCurrency(RE::TESObjectREFR* owner, Core::CurrencyKind kind, std::int32_t count);
        static bool GrantToken(
            RE::TESObjectREFR* owner,
            std::uint8_t tier,
            Core::ItemRarity rarity,
            Core::TokenSlotGroup slotGroup,
            std::int32_t count);
        static std::size_t GiveStarterCurrency(RE::PlayerCharacter* player);

        // 플레이어 가방 내 제작 가능 장비(WEAP/ARMO) 및 보유 봉인 토큰 스택 열거(주 갱신 스레드 전용)
        [[nodiscard]] static std::vector<PlayerBagItemEntry> ListPlayerCraftableItems(RE::PlayerCharacter* player);
        [[nodiscard]] static std::vector<PlayerTokenStackEntry> ListPlayerTokens(RE::PlayerCharacter* player);

        // --- 원자 거래(주 갱신 스레드 전용) ---
        [[nodiscard]] static CraftActionResult IdentifyToken(
            std::uint8_t tier,
            Core::ItemRarity rarity,
            Core::TokenSlotGroup slotGroup);

        [[nodiscard]] static CraftActionResult ApplyOrbToItem(
            Core::CurrencyKind orb,
            RE::FormID baseFormId,
            std::uint16_t uniqueId,
            bool preferEquipped = false);

        // slotIndex의 주 젬 또는 발동 젬 내부 주문 젬(triggeredSpellInstanceId != 0)에 쥬얼러의 오브 적용
        [[nodiscard]] static CraftActionResult ApplyJewellerToGem(
            std::uint8_t slotIndex,
            std::uint64_t triggeredSpellInstanceId = 0);

        // 후회의 오브 연동 패시브 트리 회수/마스터리 교체(FreePassiveRefund=true면 비용 0)
        [[nodiscard]] static Core::PassiveTreeOpResult RefundPassiveNodeWithRegret(std::uint32_t nodeId);
        [[nodiscard]] static Core::PassiveTreeOpResult RefundAllPassivesWithRegret(bool ascendancyOnly);
        [[nodiscard]] static Core::PassiveTreeOpResult SelectMasteryWithRegret(
            std::uint32_t masteryNodeId,
            std::uint32_t effectId);
    };
}
