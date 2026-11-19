#pragma once

#include "core/ItemData.h"
#include "core/ItemTypes.h"
#include "core/Types.h"

#include <random>
#include <vector>

namespace SkyrimPoE::Core
{
    enum class DropKind : std::uint8_t
    {
        SealedToken = 0,
        CurrencyOrb = 1
    };

    struct DropEntry
    {
        DropKind kind = DropKind::SealedToken;
        // SealedToken 전용
        std::uint8_t tier = 1;  // 1..5
        ItemRarity rarity = ItemRarity::Magic;
        TokenSlotGroup slotGroup = TokenSlotGroup::OneHandWeapon;
        // CurrencyOrb 전용
        CurrencyKind currency = CurrencyKind::ScrollOfWisdom;
        // 결정 159: 바닥 물리 드랍 등급(0=낮음, 1=매직/중급오브, 2=희귀/고가치오브)
        std::uint8_t worldDropTier = 0;
    };

    // 결정 16, 157, 158, 159: 처치 보상 시 시체/바닥에 드랍할 무상태 봉인 토큰 및 12종 화폐 오브를 판정한다.
    class LootTable
    {
    public:
        [[nodiscard]] static std::vector<DropEntry> RollKillDrops(
            std::uint32_t effectiveMonsterLevel,
            bool isBoss,
            bool isDragon,
            const Config& config,
            const ItemData& itemData,
            std::mt19937_64& rng);
    };
}
