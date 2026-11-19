#include "core/LootTable.h"

#include <algorithm>

namespace SkyrimPoE::Core
{
    namespace
    {
        CurrencyKind RollWeightedCurrency(const ItemData& itemData, std::mt19937_64& rng, std::uint8_t& outWorldTier)
        {
            const auto& currencies = itemData.AllCurrencies();
            std::uint64_t total = 0;
            for (const auto& c : currencies) {
                total += c.dropWeight;
            }
            if (total == 0 || currencies.empty()) {
                outWorldTier = 0;
                return CurrencyKind::ScrollOfWisdom;
            }
            std::uniform_int_distribution<std::uint64_t> dist(0, total - 1);
            std::uint64_t pick = dist(rng);
            for (const auto& c : currencies) {
                if (pick < c.dropWeight) {
                    outWorldTier = c.worldDropTier;
                    return c.kind;
                }
                pick -= c.dropWeight;
            }
            outWorldTier = currencies.back().worldDropTier;
            return currencies.back().kind;
        }
    }

    std::vector<DropEntry> LootTable::RollKillDrops(
        std::uint32_t effectiveMonsterLevel,
        bool isBoss,
        bool isDragon,
        const Config& config,
        const ItemData& itemData,
        std::mt19937_64& rng)
    {
        std::vector<DropEntry> drops;
        if (!config.enableLootDrops) {
            return drops;
        }

        const std::uint8_t tier = ItemLevelToTier(std::max<std::uint32_t>(1, effectiveMonsterLevel));
        const double mult = (isBoss || isDragon) ? std::max(1.0, config.bossDropMultiplier) : 1.0;
        const int rollPasses = isDragon ? 3 : (isBoss ? 2 : 1);

        std::uniform_real_distribution<double> u01(0.0, 1.0);
        std::uniform_int_distribution<int> slotDist(0, static_cast<int>(kTokenSlotGroupCount) - 1);

        for (int pass = 0; pass < rollPasses; ++pass) {
            // 1) 봉인 토큰 판정
            const double tokChance = std::clamp(config.tokenDropChance * (pass == 0 ? mult : 0.75), 0.0, 1.0);
            if (u01(rng) < tokChance) {
                DropEntry e;
                e.kind = DropKind::SealedToken;
                e.tier = tier;
                const double rareProb = std::clamp(config.rareTokenRatio * (isBoss || isDragon ? 1.8 : 1.0), 0.0, 0.95);
                e.rarity = (u01(rng) < rareProb) ? ItemRarity::Rare : ItemRarity::Magic;
                e.slotGroup = static_cast<TokenSlotGroup>(slotDist(rng));
                e.worldDropTier = (e.rarity == ItemRarity::Rare) ? 2 : 1;
                drops.push_back(e);
            }

            // 2) 화폐 오브 판정
            const double curChance = std::clamp(config.currencyDropChance * (pass == 0 ? mult : 0.75), 0.0, 1.0);
            if (u01(rng) < curChance) {
                DropEntry e;
                e.kind = DropKind::CurrencyOrb;
                e.currency = RollWeightedCurrency(itemData, rng, e.worldDropTier);
                drops.push_back(e);
            }
        }

        return drops;
    }
}
