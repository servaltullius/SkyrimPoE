#pragma once

#include "core/CharmSystem.h"
#include "core/DataPack.h"
#include "core/ItemData.h"
#include "core/ItemTypes.h"
#include "core/PassiveTree.h"
#include "core/PassiveTreeData.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
#include <span>
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
        bool isVirtualEquipped = false;  // 가상 반지2 또는 가상 벨트 장착 여부
        std::string displayName;
        Core::EquipSlot equipSlot = Core::EquipSlot::MainHand;
        Core::TokenSlotGroup slotGroup = Core::TokenSlotGroup::OneHandWeapon;
        Core::DefenseSubtype defenseSubtype = Core::DefenseSubtype::None;
        bool isAmulet = false;
        std::uint8_t jewelColor = 0;
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
        bool isUniqueToken = false;
        std::int32_t count = 0;
        std::string displayName;
    };

    // 결정 16, 152, 157, 158, 161~164, 통합 설계서 17장:
    // 모든 감정·오브 제작·젬 소켓 확장·후회의 오브 회수·가상 장착·참·주얼·오라 토글을
    // 주 갱신 스레드 + RunSerializedTransaction 안에서 원자적으로 수행한다(검증 통과 후에만 차감, 실패 시 롤백).
    class CraftingRuntime
    {
    public:
        static void Initialize(
            const Core::ItemData* itemData,
            const Core::DataPack* dataPack,
            const Core::PassiveTreeData* treeData,
            const Core::Config* config,
            Core::CharmSystem* charmSystem = nullptr,
            Core::StatEngine* statEngine = nullptr);

        // 화폐·토큰·5단계 아이템 인벤토리 헬퍼(주 갱신 스레드 전용)
        [[nodiscard]] static std::int32_t CountCurrency(RE::TESObjectREFR* owner, Core::CurrencyKind kind);
        [[nodiscard]] static std::array<std::int32_t, Core::kCurrencyKindCount> CountAllCurrencies(RE::TESObjectREFR* owner);
        [[nodiscard]] static std::int32_t CountUniqueTokens(RE::TESObjectREFR* owner);
        [[nodiscard]] static std::array<std::int32_t, Core::kCharmKindCount> CountAllCharms(RE::TESObjectREFR* owner);
        static bool GrantCurrency(RE::TESObjectREFR* owner, Core::CurrencyKind kind, std::int32_t count);

        // 시작 화폐 지급 목록(GiveStarterCurrency와 AutoTest가 같은 표를 쓴다)
        struct StarterCurrencyGrant
        {
            Core::CurrencyKind kind;
            std::int32_t count;
        };
        [[nodiscard]] static std::span<const StarterCurrencyGrant> StarterCurrencyGrants() noexcept;
        static bool ConsumeCurrency(RE::TESObjectREFR* owner, Core::CurrencyKind kind, std::int32_t count);
        static bool GrantToken(
            RE::TESObjectREFR* owner,
            std::uint8_t tier,
            Core::ItemRarity rarity,
            Core::TokenSlotGroup slotGroup,
            std::int32_t count);
        static bool GrantUniqueToken(RE::TESObjectREFR* owner, std::int32_t count);
        static bool GrantJewel(RE::TESObjectREFR* owner, std::uint8_t jewelColor, std::int32_t count);
        static bool GrantCharm(RE::TESObjectREFR* owner, Core::CharmKind kind, std::int32_t count);
        static bool GrantBeltBase(RE::TESObjectREFR* owner, RE::FormID localBeltFormId, std::int32_t count);
        static std::size_t GiveStarterCurrency(RE::PlayerCharacter* player);

        // 플레이어 가방 내 제작 가능 장비(WEAP/ARMO/벨트/주얼) 및 보유 봉인 토큰 스택 열거(주 갱신 스레드 전용)
        [[nodiscard]] static std::vector<PlayerBagItemEntry> ListPlayerCraftableItems(RE::PlayerCharacter* player);
        [[nodiscard]] static std::vector<PlayerTokenStackEntry> ListPlayerTokens(RE::PlayerCharacter* player);

        // --- 원자 거래(주 갱신 스레드 전용) ---
        [[nodiscard]] static CraftActionResult IdentifyToken(
            std::uint8_t tier,
            Core::ItemRarity rarity,
            Core::TokenSlotGroup slotGroup);

        // 5단계(결정 164): 고유 봉인 토큰 + 감정 주문서로 16종 PoE1 고유 아이템 중 1종 감정(forcedUniqueId 비우면 랜덤)
        [[nodiscard]] static CraftActionResult IdentifyUniqueToken(std::string_view forcedUniqueId = {});

        [[nodiscard]] static CraftActionResult ApplyOrbToItem(
            Core::CurrencyKind orb,
            RE::FormID baseFormId,
            std::uint16_t uniqueId,
            bool preferEquipped = false);

        // slotIndex의 주 젬 또는 발동 젬 내부 주문 젬(triggeredSpellInstanceId != 0)에 쥬얼러 오브 적용
        [[nodiscard]] static CraftActionResult ApplyJewellerToGem(
            std::uint8_t slotIndex,
            std::uint64_t triggeredSpellInstanceId = 0);

        // 5단계(결정 161~164): 가상 반지2·벨트 장착, 참 장착/탈착, 패시브 주얼 소켓, 정신력 오라 토글
        [[nodiscard]] static CraftActionResult EquipVirtualRing2(RE::FormID baseFormId, std::uint16_t uniqueId);
        [[nodiscard]] static CraftActionResult UnequipVirtualRing2();
        [[nodiscard]] static CraftActionResult EquipVirtualBelt(RE::FormID baseFormId, std::uint16_t uniqueId);
        [[nodiscard]] static CraftActionResult UnequipVirtualBelt();
        [[nodiscard]] static CraftActionResult EquipCharm(std::uint8_t slotIndex, Core::CharmKind kind);
        [[nodiscard]] static CraftActionResult UnequipCharm(std::uint8_t slotIndex);
        [[nodiscard]] static CraftActionResult SocketPassiveJewel(
            std::uint32_t nodeId,
            RE::FormID baseFormId,
            std::uint16_t uniqueId);
        [[nodiscard]] static CraftActionResult UnsocketPassiveJewel(std::uint32_t nodeId);
        [[nodiscard]] static CraftActionResult ToggleAuraSlot(std::uint8_t slotIndex);

        // 후회의 오브 연동 패시브 트리 회수/마스터리 교체(FreePassiveRefund=true면 비용 0, 소켓 주얼 자동 반환)
        [[nodiscard]] static Core::PassiveTreeOpResult RefundPassiveNodeWithRegret(std::uint32_t nodeId);
        [[nodiscard]] static Core::PassiveTreeOpResult RefundAllPassivesWithRegret(bool ascendancyOnly);
        [[nodiscard]] static Core::PassiveTreeOpResult SelectMasteryWithRegret(
            std::uint32_t masteryNodeId,
            std::uint32_t effectId);
    };
}
