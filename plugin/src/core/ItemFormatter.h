#pragma once

#include "core/ItemData.h"
#include "core/ItemTypes.h"

#include <string>
#include <vector>

namespace SkyrimPoE::Core
{
    struct FormattedAffixLine
    {
        AffixType type = AffixType::Prefix;
        std::uint32_t tier = 1;
        std::string tierBadge;  // "[접두 T2]" / "[접미 T1]"
        std::string textKo;
    };

    // 결정 160: UI 비종속 아이템 설명 블록(Prisma UI 즉시 사용 + 추후 Grid Inventory / SkyUI 재사용)
    struct FormattedItemCard
    {
        std::string title;
        ItemRarity rarity = ItemRarity::Normal;
        std::string rarityBadge;  // "[일반]" / "[매직]" / "[희귀]" (색상 단독 의존 금지 §17)
        std::uint32_t itemLevel = 1;
        std::string slotLabel;
        bool requirementMet = true;
        std::string requirementText;
        std::vector<std::string> baseStatLines;
        std::vector<FormattedAffixLine> affixLines;
    };

    class ItemFormatter
    {
    public:
        [[nodiscard]] static std::string RenderLineTemplate(
            std::string_view tpl,
            const std::vector<double>& values,
            std::size_t valOffset,
            const std::vector<AffixRange>& ranges);

        [[nodiscard]] static FormattedItemCard FormatItem(
            const PoEItemRecord* record,
            std::string_view baseItemName,
            EquipSlot slot,
            const EquippedSlotInput& computedSlot,
            bool requirementMet,
            const ItemData& data);
    };
}
