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
        CurrencyOrb = 1,
        UniqueToken = 2,
        Jewel = 3,
        Charm = 4,
        SkillGem = 5,
        MapItem = 6,            // 6단계(결정 166): T1..T16 지도 아이템 (tier = 1..16)
        AscendancyTribute = 7   // 6단계(결정 18, 166): 4차 전직 결투장 공물
    };

    struct DropEntry
    {
        DropKind kind = DropKind::SealedToken;
        // SealedToken / UniqueToken 전용
        std::uint8_t tier = 1;  // 1..5 (MapItem: 1..16)
        ItemRarity rarity = ItemRarity::Magic;
        TokenSlotGroup slotGroup = TokenSlotGroup::OneHandWeapon;
        // CurrencyOrb 전용
        CurrencyKind currency = CurrencyKind::ScrollOfWisdom;
        // Jewel 전용 (0=Crimson, 1=Viridian, 2=Cobalt)
        std::uint8_t jewelColor = 0;
        // Charm 전용
        CharmKind charm = CharmKind::Ruby;
        // SkillGem 전용
        std::string gemId;
        // 결정 159: 바닥 물리 드랍 등급(0=낮음, 1=매직/중급오브, 2=희귀/고유/고가치오브)
        std::uint8_t worldDropTier = 0;
    };

    // 결정 16, 157, 158, 159, 164: 처치 보상 시 시체/바닥에 드랍할 봉인 토큰·화폐·고유 토큰·주얼·참·스킬 젬을 판정한다.
    class LootTable
    {
    public:
        [[nodiscard]] static std::vector<DropEntry> RollKillDrops(
            std::uint32_t effectiveMonsterLevel,
            bool isBoss,
            bool isDragon,
            const Config& config,
            const ItemData& itemData,
            std::mt19937_64& rng,
            MonsterRarity monsterRarity = MonsterRarity::Normal,
            std::uint8_t activeMapTier = 0,
            double bonusIiqPct = 0.0,
            double bonusIirPct = 0.0,
            bool guaranteeFirstTribute = false);
    };
}
