#pragma once

#include "core/ItemData.h"
#include "core/ItemTypes.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace SkyrimPoE::Runtime
{
    struct EquippedSlotMeta
    {
        bool occupied = false;
        std::uint32_t baseFormId = 0;
        Core::InventoryItemKey key{};
        std::string displayName;
        Core::TokenSlotGroup slotGroup = Core::TokenSlotGroup::OneHandWeapon;
        Core::DefenseSubtype defenseSubtype = Core::DefenseSubtype::None;
        bool isAmulet = false;
        std::string weaponTag;
        bool hasPoERecord = false;
        Core::PoEItemRecord record{};
    };

    // 결정 156, 검수 C5/M3:
    // 플레이어가 착용한 WEAP/ARMO를 ExtraWorn/ExtraWornLeft로 직접 스캔해 논리 8칸으로 단일 귀속한다.
    // SaveLoad::SyncStateToStatEngine()이 GetCurrentGearSnapshot()을 병합하는 단일 조합 지점을 사용한다.
    class EquipmentSync
    {
    public:
        static void Initialize(const Core::ItemData* itemData);

        // 방어구·무기 분류 공용 헬퍼(UiBridge·CraftingRuntime 공용)
        [[nodiscard]] static std::optional<Core::EquipSlot> ClassifyArmorEquipSlot(const RE::TESObjectARMO* armor) noexcept;
        [[nodiscard]] static Core::TokenSlotGroup EquipSlotToTokenGroup(Core::EquipSlot slot, bool isTwoHandedWeapon) noexcept;
        [[nodiscard]] static Core::DefenseSubtype ClassifyDefenseSubtype(const RE::TESObjectARMO* armor) noexcept;
        [[nodiscard]] static std::string ClassifyWeaponTag(const RE::TESObjectWEAP* weapon);
        [[nodiscard]] static bool IsTwoHandedWeapon(const RE::TESObjectWEAP* weapon) noexcept;

        // 개별 장비 1건(착용 또는 가방)의 EquippedSlotInput 산출
        [[nodiscard]] static Core::EquippedSlotInput BuildSlotInput(
            RE::TESBoundObject* baseObj,
            const Core::PoEItemRecord* poeRecord,
            std::uint32_t exileLevel,
            const Core::ItemData& itemData);

        // 주 갱신 스레드 전용: 플레이어 착용 장비 재스캔 후 SaveLoad::SyncStateToStatEngine 호출
        static void MarkDirty() noexcept;
        static void FlushIfDirty();
        static void RefreshFromPlayerNow();

        // 스레드 안전 스냅샷 조회(SaveLoad::BuildLayer1FromState 및 UiBridge용)
        [[nodiscard]] static std::array<Core::EquippedSlotInput, Core::kEquipSlotCount> GetCurrentGearSnapshot();
        [[nodiscard]] static std::array<EquippedSlotMeta, Core::kEquipSlotCount> GetCurrentGearMeta();
        static void Clear();
    };
}
