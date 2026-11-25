#include "core/LootTable.h"
#include "core/GemForms.h"

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
        std::mt19937_64& rng,
        MonsterRarity monsterRarity,
        std::uint8_t activeMapTier,
        double bonusIiqPct,
        double bonusIirPct,
        bool guaranteeFirstTribute,
        std::uint32_t gemGeneration)
    {
        std::vector<DropEntry> drops;
        if (!config.enableLootDrops) {
            return drops;
        }

        const std::uint8_t tier = ItemLevelToTier(std::max<std::uint32_t>(1, effectiveMonsterLevel));
        const double safeIiq = std::isfinite(bonusIiqPct) ? std::clamp(bonusIiqPct, 0.0, 300.0) : 0.0;
        const double safeIir = std::isfinite(bonusIirPct) ? std::clamp(bonusIirPct, 0.0, 300.0) : 0.0;
        const double iiqMult = 1.0 + safeIiq / 100.0;
        const double iirMult = 1.0 + safeIir / 100.0;
        const double bossMult = (isBoss || isDragon) ? std::max(1.0, config.bossDropMultiplier) : 1.0;
        const double rarityMult = (monsterRarity == MonsterRarity::Rare) ? 3.0
                                : (monsterRarity == MonsterRarity::Magic) ? 1.6
                                : 1.0;
        const double mult = bossMult * rarityMult * iiqMult;
        const int rollPasses = isDragon ? 3 : ((isBoss || monsterRarity == MonsterRarity::Rare) ? 2 : 1);

        std::uniform_real_distribution<double> u01(0.0, 1.0);
        std::uniform_int_distribution<int> slotDist(0, static_cast<int>(kDroppableTokenSlotGroupCount) - 1);

        for (int pass = 0; pass < rollPasses; ++pass) {
            // 1) 봉인 토큰 판정 (0..8: 기존 8부위 + 5단계 벨트)
            const double tokChance = std::clamp(config.tokenDropChance * (pass == 0 ? mult : 0.75 * iiqMult), 0.0, 1.0);
            if (u01(rng) < tokChance) {
                DropEntry e;
                e.kind = DropKind::SealedToken;
                e.tier = tier;
                const double rareBoost = ((isBoss || isDragon || monsterRarity == MonsterRarity::Rare) ? 1.8 : 1.0) * iirMult;
                const double rareProb = std::clamp(config.rareTokenRatio * rareBoost, 0.0, 0.95);
                e.rarity = (u01(rng) < rareProb) ? ItemRarity::Rare : ItemRarity::Magic;
                e.slotGroup = static_cast<TokenSlotGroup>(slotDist(rng));
                e.worldDropTier = (e.rarity == ItemRarity::Rare) ? 2 : 1;
                drops.push_back(e);
            }

            // 2) 화폐 오브 판정
            const double curChance = std::clamp(config.currencyDropChance * (pass == 0 ? mult : 0.75 * iiqMult), 0.0, 1.0);
            if (u01(rng) < curChance) {
                DropEntry e;
                e.kind = DropKind::CurrencyOrb;
                e.currency = RollWeightedCurrency(itemData, rng, e.worldDropTier);
                drops.push_back(e);
            }
        }

        // 3) 5단계(결정 164·C-18): 고유 봉인 토큰은 레어·보스·드래곤 처치 시에만 드랍(일반·매직 제외)
        const double uniqMult = isDragon ? 6.0
                              : (isBoss || monsterRarity == MonsterRarity::Rare) ? 3.0
                              : 0.0;
        const double uniqChance = std::clamp(config.uniqueDropChance * uniqMult * iirMult, 0.0, 0.95);
        if (uniqChance > 0.0 && !itemData.AllUniques().empty() && u01(rng) < uniqChance) {
            DropEntry e;
            e.kind = DropKind::UniqueToken;
            e.tier = tier;
            e.rarity = ItemRarity::Unique;
            e.worldDropTier = 2;
            drops.push_back(e);
        }

        // 4) 5단계 주얼·참·스킬 젬 드랍(매직+/보스/드래곤 우선)
        if (monsterRarity != MonsterRarity::Normal || isBoss || isDragon) {
            if (u01(rng) < std::clamp(0.06 * mult, 0.0, 0.45)) {
                DropEntry e;
                e.kind = DropKind::Jewel;
                e.tier = tier;
                e.rarity = ItemRarity::Normal;
                e.slotGroup = TokenSlotGroup::Jewel;
                std::uniform_int_distribution<int> cDist(0, 2);
                e.jewelColor = static_cast<std::uint8_t>(cDist(rng));
                e.worldDropTier = 1;
                drops.push_back(e);
            }
            if (config.enableCharms && u01(rng) < std::clamp(0.05 * mult, 0.0, 0.35)) {
                DropEntry e;
                e.kind = DropKind::Charm;
                std::uniform_int_distribution<int> chDist(0, static_cast<int>(kCharmKindCount) - 1);
                e.charm = static_cast<CharmKind>(chDist(rng));
                e.worldDropTier = 1;
                drops.push_back(e);
            }
            const auto gemIds = GemForms::DropIds(gemGeneration);
            if (!gemIds.empty() && u01(rng) < std::clamp(0.04 * mult, 0.0, 0.30)) {
                DropEntry e;
                e.kind = DropKind::SkillGem;
                std::uniform_int_distribution<std::size_t> gDist(0, gemIds.size() - 1);
                e.gemId = gemIds[gDist(rng)];
                e.worldDropTier = 1;
                drops.push_back(e);
            }
        }

        // 5) 6단계(결정 166): 지도 아이템(T1..T16) 및 4차 전직 공물 드랍
        if (activeMapTier >= 1 && activeMapTier <= 16) {
            // 지도 내부: 보스는 최소 1장 확정 드랍(지도 유지 보장), 일반/매직/레어도 IIQ 비례 드랍
            const double mapChance = isBoss ? 1.0
                : std::clamp((monsterRarity == MonsterRarity::Rare ? 0.22 : (monsterRarity == MonsterRarity::Magic ? 0.10 : 0.04)) * iiqMult, 0.0, 0.85);
            if (u01(rng) < mapChance) {
                DropEntry e;
                e.kind = DropKind::MapItem;
                e.slotGroup = TokenSlotGroup::Map;
                e.rarity = ItemRarity::Normal;
                const int minT = std::max(1, static_cast<int>(activeMapTier) - 1);
                const int maxT = std::min(16, static_cast<int>(activeMapTier) + (isBoss ? 1 : 0));
                std::uniform_int_distribution<int> tDist(minT, maxT);
                e.tier = static_cast<std::uint8_t>(tDist(rng));
                e.worldDropTier = 2;
                drops.push_back(e);
            }
            // T6+ 지도 보스 처치 시 4차 전직 공물 드랍(첫 T6+ 보스 확정 1회)
            if (isBoss && activeMapTier >= 6) {
                const double tribChance = guaranteeFirstTribute ? 1.0 : (activeMapTier >= 11 ? 0.50 : 0.25);
                if (u01(rng) < tribChance) {
                    DropEntry e;
                    e.kind = DropKind::AscendancyTribute;
                    e.tier = activeMapTier;
                    e.rarity = ItemRarity::Unique;
                    e.worldDropTier = 2;
                    drops.push_back(e);
                }
            }
        } else if (activeMapTier == 0 && effectiveMonsterLevel >= 50 && (isBoss || isDragon || monsterRarity == MonsterRarity::Rare)) {
            // 오픈월드 Lv50+ 보스/드래곤/레어에서 T1 입문 지도 드랍
            const double openMapChance = std::clamp(((isBoss || isDragon) ? 0.35 : 0.12) * iiqMult, 0.0, 0.90);
            if (u01(rng) < openMapChance) {
                DropEntry e;
                e.kind = DropKind::MapItem;
                e.slotGroup = TokenSlotGroup::Map;
                e.rarity = ItemRarity::Normal;
                e.tier = 1;
                e.worldDropTier = 2;
                drops.push_back(e);
            }
        }

        return drops;
    }
}
