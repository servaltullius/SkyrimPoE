#include "core/ItemCrafting.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace SkyrimPoE::Core
{
    namespace
    {
        struct AffixCounts
        {
            std::size_t prefixes = 0;
            std::size_t suffixes = 0;
            std::unordered_set<std::string> usedGroups;
        };

        AffixCounts CountAffixes(const PoEItemRecord& item, const ItemData& data)
        {
            AffixCounts c;
            for (const auto& rolled : item.affixes) {
                if (const auto* def = data.FindAffix(rolled.affixId)) {
                    if (def->type == AffixType::Prefix) {
                        ++c.prefixes;
                    } else {
                        ++c.suffixes;
                    }
                    if (!def->group.empty()) {
                        c.usedGroups.insert(def->group);
                    }
                }
            }
            return c;
        }

        double RollSingleRange(const AffixRange& r, std::mt19937_64& rng)
        {
            if (r.maxVal <= r.minVal) {
                return r.minVal;
            }
            if (!r.isFloat) {
                const auto lo = static_cast<std::int64_t>(std::llround(r.minVal));
                const auto hi = static_cast<std::int64_t>(std::llround(r.maxVal));
                std::uniform_int_distribution<std::int64_t> dist(lo, hi);
                return static_cast<double>(dist(rng));
            }
            // 소수점 첫째 자리(0.1 단위) 정수 격자로 균일 추첨
            const auto lo10 = static_cast<std::int64_t>(std::llround(r.minVal * 10.0));
            const auto hi10 = static_cast<std::int64_t>(std::llround(r.maxVal * 10.0));
            std::uniform_int_distribution<std::int64_t> dist(lo10, hi10);
            return static_cast<double>(dist(rng)) / 10.0;
        }

        RolledAffix RollAffixValues(const AffixDefinition& def, std::mt19937_64& rng)
        {
            RolledAffix out;
            out.affixId = def.id;
            for (const auto& ln : def.lines) {
                for (const auto& r : ln.ranges) {
                    out.values.push_back(RollSingleRange(r, rng));
                }
            }
            return out;
        }

        bool HasAnyVariableRange(const PoEItemRecord& item, const ItemData& data)
        {
            for (const auto& rolled : item.affixes) {
                const auto* def = data.FindAffix(rolled.affixId);
                if (!def) continue;
                for (const auto& ln : def->lines) {
                    for (const auto& r : ln.ranges) {
                        if (r.maxVal > r.minVal) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        bool TryAddOneAffix(
            PoEItemRecord& item,
            std::size_t maxPrefixes,
            std::size_t maxSuffixes,
            const std::unordered_set<std::string>& itemTags,
            const ItemData& data,
            std::mt19937_64& rng)
        {
            const auto counts = CountAffixes(item, data);
            const bool canPrefix = counts.prefixes < maxPrefixes;
            const bool canSuffix = counts.suffixes < maxSuffixes;
            if (!canPrefix && !canSuffix) {
                return false;
            }

            auto pCands = canPrefix
                ? data.CollectCandidates(AffixType::Prefix, item.itemLevel, itemTags, counts.usedGroups)
                : std::vector<std::pair<const AffixDefinition*, std::uint32_t>>{};
            auto sCands = canSuffix
                ? data.CollectCandidates(AffixType::Suffix, item.itemLevel, itemTags, counts.usedGroups)
                : std::vector<std::pair<const AffixDefinition*, std::uint32_t>>{};

            if (pCands.empty() && sCands.empty()) {
                return false;
            }

            // U6 확정(2026-11-20, PoB 미명시): SkyrimPoE 확정 추첨 규칙 — 양쪽 풀이 모두 열려 있으면 50:50으로 접두/접미를 먼저 고른 뒤 가중 추첨
            const auto* chosenPool = &pCands;
            if (pCands.empty()) {
                chosenPool = &sCands;
            } else if (!sCands.empty()) {
                std::uniform_int_distribution<int> coin(0, 1);
                chosenPool = (coin(rng) == 0) ? &pCands : &sCands;
            }

            std::uint64_t totalWeight = 0;
            for (const auto& [_, w] : *chosenPool) {
                totalWeight += w;
            }
            if (totalWeight == 0) {
                return false;
            }

            std::uniform_int_distribution<std::uint64_t> dist(0, totalWeight - 1);
            std::uint64_t pick = dist(rng);
            const AffixDefinition* chosen = chosenPool->back().first;
            for (const auto& [def, w] : *chosenPool) {
                if (pick < w) {
                    chosen = def;
                    break;
                }
                pick -= w;
            }

            item.affixes.push_back(RollAffixValues(*chosen, rng));
            return true;
        }

        // U6 확정(2026-11-20, PoB 미명시): SkyrimPoE 확정 어픽스 개수 분포:
        // 일반 장비: 매직 1(50%)/2(50%), 희귀 4(60%)/5(30%)/6(10%)
        // 주얼(5단계): 매직 1(50%)/2(50%), 희귀 3(70%)/4(30%) (최대 2P/2S)
        std::size_t RollTargetAffixCount(ItemRarity rarity, bool isJewel, std::mt19937_64& rng)
        {
            std::uniform_int_distribution<int> d100(0, 99);
            const int r = d100(rng);
            if (rarity == ItemRarity::Magic) {
                return r < 50 ? 1 : 2;
            }
            if (rarity == ItemRarity::Rare) {
                if (isJewel) {
                    return r < 70 ? 3 : 4;
                }
                if (r < 60) return 4;
                if (r < 90) return 5;
                return 6;
            }
            return 0;
        }

        bool RollFreshAffixes(
            PoEItemRecord& item,
            ItemRarity targetRarity,
            const std::unordered_set<std::string>& itemTags,
            const ItemData& data,
            std::mt19937_64& rng)
        {
            item.rarity = targetRarity;
            item.affixes.clear();

            const bool isJewel = (item.slotGroup == TokenSlotGroup::Jewel);
            const std::size_t maxPerSide = (targetRarity == ItemRarity::Magic) ? 1 : (isJewel ? 2 : 3);
            const std::size_t targetCount = RollTargetAffixCount(targetRarity, isJewel, rng);

            for (std::size_t i = 0; i < targetCount; ++i) {
                if (!TryAddOneAffix(item, maxPerSide, maxPerSide, itemTags, data, rng)) {
                    break;  // 검수 M5: 저레벨에서 가용 고유 그룹이 targetCount보다 적으면 무한 루프 없이 정상 종료
                }
            }
            return !item.affixes.empty();
        }
    }

    PoEItemRecord ItemCrafting::IdentifyToken(
        std::uint8_t tier,
        ItemRarity rarity,
        TokenSlotGroup slotGroup,
        DefenseSubtype defenseSubtype,
        std::string_view weaponTag,
        bool isAmulet,
        std::uint32_t baseFormId,
        std::uint32_t exileLevel,
        const ItemData& data,
        std::mt19937_64& rng,
        std::string_view implicitAffixId,
        std::uint8_t jewelColor)
    {
        PoEItemRecord rec;
        rec.baseFormId = baseFormId;
        rec.slotGroup = slotGroup;
        rec.defenseSubtype = defenseSubtype;
        rec.isAmulet = isAmulet;
        rec.weaponTag = std::string(weaponTag);
        rec.jewelColor = jewelColor;

        // 결정 157: iLvl은 반드시 티어 구간 [tierMin, tierMax] 내부로 고정
        const auto [lo, hi] = TierLevelBounds(tier);
        rec.itemLevel = std::clamp(exileLevel, lo, hi);

        if (!implicitAffixId.empty()) {
            if (const auto* impDef = data.FindAffix(implicitAffixId)) {
                rec.implicitAffixes.push_back(RollAffixValues(*impDef, rng));
            }
        }

        const ItemRarity effectiveRarity = (rarity == ItemRarity::Rare) ? ItemRarity::Rare : ItemRarity::Magic;
        const auto tags = ItemData::BuildItemTags(slotGroup, defenseSubtype, weaponTag, isAmulet, jewelColor);
        RollFreshAffixes(rec, effectiveRarity, tags, data, rng);
        return rec;
    }

    PoEItemRecord ItemCrafting::RollUniqueItem(
        const UniqueDefinition& def,
        std::uint32_t resolvedBaseFormId,
        std::uint32_t itemLevel,
        const ItemData& data,
        std::mt19937_64& rng)
    {
        PoEItemRecord rec;
        rec.baseFormId = resolvedBaseFormId;
        rec.rarity = ItemRarity::Unique;
        rec.itemLevel = std::max(itemLevel, def.reqLevel);
        rec.slotGroup = def.slotGroup;
        rec.defenseSubtype = def.defenseSubtype;
        rec.isAmulet = def.isAmulet;
        rec.weaponTag = def.weaponTag;
        rec.uniqueId = def.id;

        for (const auto& impId : def.implicitAffixIds) {
            if (const auto* impDef = data.FindAffix(impId)) {
                rec.implicitAffixes.push_back(RollAffixValues(*impDef, rng));
            }
        }
        for (const auto& expId : def.explicitAffixIds) {
            if (const auto* expDef = data.FindAffix(expId)) {
                rec.affixes.push_back(RollAffixValues(*expDef, rng));
            }
        }
        return rec;
    }

    std::vector<RolledAffix> ItemCrafting::RollBeltImplicits(
        const BeltBaseDef& belt,
        const ItemData& data,
        std::mt19937_64& rng)
    {
        std::vector<RolledAffix> out;
        if (!belt.implicitAffixId.empty()) {
            if (const auto* impDef = data.FindAffix(belt.implicitAffixId)) {
                out.push_back(RollAffixValues(*impDef, rng));
            }
        }
        return out;
    }

    std::uint8_t ItemCrafting::ComputeBeltCharmSlots(
        bool beltEquipped,
        const PoEItemRecord* beltRec,
        bool isStygianVise) noexcept
    {
        if (!beltEquipped) {
            return 0;
        }
        if (isStygianVise) {
            return 3;
        }
        if (!beltRec) {
            return 1;
        }
        if (beltRec->rarity == ItemRarity::Unique) {
            return 3;
        }
        const std::uint8_t tier = ItemLevelToTier(beltRec->itemLevel);
        if (beltRec->rarity == ItemRarity::Rare) {
            return (tier >= 4) ? 3 : 2;
        }
        if (beltRec->rarity == ItemRarity::Magic) {
            return (tier >= 4) ? 2 : 1;
        }
        return 1;
    }

    CraftOutcome ItemCrafting::CanApplyOrb(
        CurrencyKind orb,
        const PoEItemRecord& item,
        const std::unordered_set<std::string>& itemTags,
        const ItemData& data)
    {
        // 5단계(결정 164): 고유 장비에는 신성한 오브(Divine)만 허용, 그 외 모든 오브 차단
        if (item.rarity == ItemRarity::Unique && orb != CurrencyKind::Divine) {
            return { CraftResultCode::InvalidRarity, "고유 장비에는 신성한 오브만 사용할 수 있습니다." };
        }

        const std::size_t rareMaxPerSide = (item.slotGroup == TokenSlotGroup::Jewel) ? 2 : 3;
        const auto counts = CountAffixes(item, data);
        const std::unordered_set<std::string> emptyGroups;
        auto hasAnyFreshCandidate = [&]() {
            return !data.CollectCandidates(AffixType::Prefix, item.itemLevel, itemTags, emptyGroups).empty() ||
                   !data.CollectCandidates(AffixType::Suffix, item.itemLevel, itemTags, emptyGroups).empty();
        };
        auto hasAnyAdditionalCandidate = [&](std::size_t maxP, std::size_t maxS) {
            const bool hasP = counts.prefixes < maxP &&
                !data.CollectCandidates(AffixType::Prefix, item.itemLevel, itemTags, counts.usedGroups).empty();
            const bool hasS = counts.suffixes < maxS &&
                !data.CollectCandidates(AffixType::Suffix, item.itemLevel, itemTags, counts.usedGroups).empty();
            return hasP || hasS;
        };

        switch (orb) {
            case CurrencyKind::Transmutation:
                if (item.rarity != ItemRarity::Normal) {
                    return { CraftResultCode::InvalidRarity, "진화의 오브는 일반 장비에만 사용할 수 있습니다." };
                }
                if (!hasAnyFreshCandidate()) {
                    return { CraftResultCode::NoEligibleAffix, "이 장비에 부여 가능한 어픽스가 없습니다." };
                }
                return { CraftResultCode::Ok, "" };

            case CurrencyKind::Augmentation:
                if (item.rarity != ItemRarity::Magic) {
                    return { CraftResultCode::InvalidRarity, "확장의 오브는 매직 장비에만 사용할 수 있습니다." };
                }
                if (counts.prefixes >= 1 && counts.suffixes >= 1) {
                    return { CraftResultCode::AffixLimitReached, "매직 장비의 접두·접미 자리가 이미 가득 찼습니다." };
                }
                if (!hasAnyAdditionalCandidate(1, 1)) {
                    return { CraftResultCode::NoEligibleAffix, "이 장비에 추가로 부여할 수 있는 어픽스가 없습니다." };
                }
                return { CraftResultCode::Ok, "" };

            case CurrencyKind::Alteration:
                if (item.rarity != ItemRarity::Magic) {
                    return { CraftResultCode::InvalidRarity, "변화의 오브는 매직 장비에만 사용할 수 있습니다." };
                }
                if (!hasAnyFreshCandidate()) {
                    return { CraftResultCode::NoEligibleAffix, "이 장비에 부여 가능한 어픽스가 없습니다." };
                }
                return { CraftResultCode::Ok, "" };

            case CurrencyKind::Regal:
                if (item.rarity != ItemRarity::Magic) {
                    return { CraftResultCode::InvalidRarity, "제왕의 오브는 매직 장비에만 사용할 수 있습니다." };
                }
                if (!hasAnyAdditionalCandidate(rareMaxPerSide, rareMaxPerSide)) {
                    return { CraftResultCode::NoEligibleAffix, "이 장비에 추가로 부여할 수 있는 어픽스가 없습니다." };
                }
                return { CraftResultCode::Ok, "" };

            case CurrencyKind::Alchemy:
                if (item.rarity != ItemRarity::Normal) {
                    return { CraftResultCode::InvalidRarity, "연금술의 오브는 일반 장비에만 사용할 수 있습니다." };
                }
                if (!hasAnyFreshCandidate()) {
                    return { CraftResultCode::NoEligibleAffix, "이 장비에 부여 가능한 어픽스가 없습니다." };
                }
                return { CraftResultCode::Ok, "" };

            case CurrencyKind::Chaos:
                if (item.rarity != ItemRarity::Rare) {
                    return { CraftResultCode::InvalidRarity, "카오스 오브는 희귀 장비에만 사용할 수 있습니다." };
                }
                if (!hasAnyFreshCandidate()) {
                    return { CraftResultCode::NoEligibleAffix, "이 장비에 부여 가능한 어픽스가 없습니다." };
                }
                return { CraftResultCode::Ok, "" };

            case CurrencyKind::Exalted: {
                if (item.rarity != ItemRarity::Rare) {
                    return { CraftResultCode::InvalidRarity, "엑잘티드 오브는 희귀 장비에만 사용할 수 있습니다." };
                }
                if (counts.prefixes >= rareMaxPerSide && counts.suffixes >= rareMaxPerSide) {
                    return { CraftResultCode::AffixLimitReached,
                             item.slotGroup == TokenSlotGroup::Jewel
                                 ? "희귀 주얼의 접두 2·접미 2 자리가 모두 가득 찼습니다."
                                 : "희귀 장비의 접두 3·접미 3 자리가 모두 가득 찼습니다." };
                }
                if (!hasAnyAdditionalCandidate(rareMaxPerSide, rareMaxPerSide)) {
                    return { CraftResultCode::NoEligibleAffix, "이 장비에 추가로 부여할 수 있는 어픽스가 없습니다." };
                }
                return { CraftResultCode::Ok, "" };
            }

            case CurrencyKind::Scouring:
                if (item.rarity == ItemRarity::Normal && item.affixes.empty()) {
                    return { CraftResultCode::InvalidRarity, "정제의 오브는 매직·희귀 장비에만 사용할 수 있습니다." };
                }
                return { CraftResultCode::Ok, "" };

            case CurrencyKind::Divine:
                if (item.affixes.empty() || !HasAnyVariableRange(item, data)) {
                    return { CraftResultCode::NoOp, "재조정할 변동 수치 어픽스가 없습니다." };
                }
                return { CraftResultCode::Ok, "" };

            default:
                return { CraftResultCode::UnsupportedOrb, "장비에 직접 사용할 수 없는 화폐입니다." };
        }
    }

    CraftOutcome ItemCrafting::ApplyOrb(
        CurrencyKind orb,
        PoEItemRecord& item,
        const std::unordered_set<std::string>& itemTags,
        const ItemData& data,
        std::mt19937_64& rng)
    {
        const auto check = CanApplyOrb(orb, item, itemTags, data);
        if (!check.IsOk()) {
            return check;
        }

        const std::size_t rareMaxPerSide = (item.slotGroup == TokenSlotGroup::Jewel) ? 2 : 3;
        PoEItemRecord working = item;
        switch (orb) {
            case CurrencyKind::Transmutation:
                if (!RollFreshAffixes(working, ItemRarity::Magic, itemTags, data, rng)) {
                    return { CraftResultCode::NoEligibleAffix, "부여 가능한 어픽스가 없습니다." };
                }
                item = std::move(working);
                return { CraftResultCode::Ok, "장비를 매직 등급으로 변환했습니다." };

            case CurrencyKind::Augmentation:
                if (!TryAddOneAffix(working, 1, 1, itemTags, data, rng)) {
                    return { CraftResultCode::NoEligibleAffix, "추가할 수 있는 어픽스가 없습니다." };
                }
                item = std::move(working);
                return { CraftResultCode::Ok, "매직 장비에 새 어픽스를 추가했습니다." };

            case CurrencyKind::Alteration:
                if (!RollFreshAffixes(working, ItemRarity::Magic, itemTags, data, rng)) {
                    return { CraftResultCode::NoEligibleAffix, "부여 가능한 어픽스가 없습니다." };
                }
                item = std::move(working);
                return { CraftResultCode::Ok, "매직 장비의 어픽스를 다시 부여했습니다." };

            case CurrencyKind::Regal:
                working.rarity = ItemRarity::Rare;
                if (!TryAddOneAffix(working, rareMaxPerSide, rareMaxPerSide, itemTags, data, rng)) {
                    return { CraftResultCode::NoEligibleAffix, "추가할 수 있는 어픽스가 없습니다." };
                }
                item = std::move(working);
                return { CraftResultCode::Ok, "장비를 희귀 등급으로 승격하고 어픽스를 추가했습니다." };

            case CurrencyKind::Alchemy:
                if (!RollFreshAffixes(working, ItemRarity::Rare, itemTags, data, rng)) {
                    return { CraftResultCode::NoEligibleAffix, "부여 가능한 어픽스가 없습니다." };
                }
                item = std::move(working);
                return { CraftResultCode::Ok, "장비를 희귀 등급으로 변환했습니다." };

            case CurrencyKind::Chaos:
                if (!RollFreshAffixes(working, ItemRarity::Rare, itemTags, data, rng)) {
                    return { CraftResultCode::NoEligibleAffix, "부여 가능한 어픽스가 없습니다." };
                }
                item = std::move(working);
                return { CraftResultCode::Ok, "희귀 장비의 어픽스를 모두 다시 부여했습니다." };

            case CurrencyKind::Exalted:
                if (!TryAddOneAffix(working, rareMaxPerSide, rareMaxPerSide, itemTags, data, rng)) {
                    return { CraftResultCode::NoEligibleAffix, "추가할 수 있는 어픽스가 없습니다." };
                }
                item = std::move(working);
                return { CraftResultCode::Ok, "희귀 장비에 새 어픽스를 추가했습니다." };

            case CurrencyKind::Scouring:
                // 검수 M1 + 5단계 임플리싯 보존: 어픽스만 제거하고 itemLevel / slotGroup / defenseSubtype / implicitAffixes 보존
                working.rarity = ItemRarity::Normal;
                working.affixes.clear();
                item = std::move(working);
                return { CraftResultCode::Ok, "어픽스를 모두 정제해 일반 등급으로 되돌렸습니다." };

            case CurrencyKind::Divine:
                for (auto& rolled : working.affixes) {
                    if (const auto* def = data.FindAffix(rolled.affixId)) {
                        rolled = RollAffixValues(*def, rng);
                    }
                }
                item = std::move(working);
                return { CraftResultCode::Ok, "기존 어픽스의 수치를 다시 조정했습니다." };

            default:
                return { CraftResultCode::UnsupportedOrb, "장비에 직접 사용할 수 없는 화폐입니다." };
        }
    }

    CraftOutcome ItemCrafting::ApplyJewellerToGem(GemInstance& gem, GemKind gemKind)
    {
        if (gemKind == GemKind::Support || gemKind == GemKind::Aura ||
            gemKind == GemKind::Herald || gemKind == GemKind::Unknown) {
            return { CraftResultCode::UnsupportedTarget, "보조·오라·전령 젬에는 쥬얼러 오브를 사용할 수 없습니다." };
        }
        if (gem.socketCount >= kMaxGemSockets) {
            return { CraftResultCode::AlreadyMaxSockets, "이미 최대 보조 소켓(5칸)에 도달한 젬입니다." };
        }
        ++gem.socketCount;
        return { CraftResultCode::Ok, "젬의 보조 소켓을 1칸 확장했습니다." };
    }
}
