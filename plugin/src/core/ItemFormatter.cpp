#include "core/ItemFormatter.h"
#include "core/MapSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace SkyrimPoE::Core
{
    namespace
    {
        std::string FormatNumber(double val, bool isFloat, int precision = 1)
        {
            if (std::abs(val) < 1e-9) {
                val = 0.0;  // 검수 m4: -0.0 표시 방지
            }
            char buf[32]{};
            if (isFloat) {
                if (precision >= 2) {
                    std::snprintf(buf, sizeof(buf), "%.2f", val);
                    std::string s(buf);
                    if (s.find('.') != std::string::npos) {
                        while (!s.empty() && s.back() == '0') s.pop_back();
                        if (!s.empty() && s.back() == '.') s.pop_back();
                    }
                    return s;
                }
                std::snprintf(buf, sizeof(buf), "%.1f", val);
            } else {
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(val)));
            }
            return std::string(buf);
        }
    }

    std::string ItemFormatter::RenderLineTemplate(
        std::string_view tpl,
        const std::vector<double>& values,
        std::size_t valOffset,
        const std::vector<AffixRange>& ranges)
    {
        std::string out(tpl);
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            const std::string token = "{" + std::to_string(i) + "}";
            const double val = (valOffset + i < values.size()) ? values[valOffset + i] : ranges[i].minVal;
            const std::string formatted = FormatNumber(val, ranges[i].isFloat);
            std::size_t pos = 0;
            while ((pos = out.find(token, pos)) != std::string::npos) {
                if (pos > 0 && out[pos - 1] == '+' && !formatted.empty() && formatted[0] == '-') {
                    out.replace(pos - 1, token.size() + 1, formatted);
                    pos = (pos - 1) + formatted.size();
                } else {
                    out.replace(pos, token.size(), formatted);
                    pos += formatted.size();
                }
            }
        }
        return out;
    }

    FormattedItemCard ItemFormatter::FormatItem(
        const PoEItemRecord* record,
        std::string_view baseItemName,
        EquipSlot slot,
        const EquippedSlotInput& computedSlot,
        bool requirementMet,
        const ItemData& data)
    {
        FormattedItemCard card;
        card.title = baseItemName.empty() ? std::string("이름 없는 장비") : std::string(baseItemName);
        card.rarity = record ? record->rarity : ItemRarity::Normal;
        if (record && record->rarity == ItemRarity::Unique && !record->uniqueId.empty()) {
            card.uniqueId = record->uniqueId;
            if (const auto* uDef = data.FindUnique(record->uniqueId)) {
                card.title = uDef->koName + " (" + card.title + ")";
            }
        }
        card.rarityBadge = std::string("[") + RarityKoreanLabel(card.rarity) + "]";
        card.itemLevel = record ? record->itemLevel : computedSlot.itemLevel;
        if (record && record->slotGroup == TokenSlotGroup::Jewel) {
            card.slotLabel = "주얼";
        } else if (record && record->slotGroup == TokenSlotGroup::Map) {
            card.slotLabel = "지도";
        } else {
            card.slotLabel = std::string(EquipSlotKoreanName(slot));
        }
        card.requirementMet = requirementMet;

        // 요구치 문자열 조합
        std::vector<std::string> reqParts;
        if (computedSlot.reqLevel > 1) reqParts.push_back("레벨 " + std::to_string(computedSlot.reqLevel));
        if (computedSlot.reqStr > 0) reqParts.push_back("힘 " + std::to_string(computedSlot.reqStr));
        if (computedSlot.reqDex > 0) reqParts.push_back("민첩 " + std::to_string(computedSlot.reqDex));
        if (computedSlot.reqInt > 0) reqParts.push_back("지능 " + std::to_string(computedSlot.reqInt));
        if (!reqParts.empty()) {
            std::ostringstream oss;
            oss << "요구: ";
            for (std::size_t i = 0; i < reqParts.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << reqParts[i];
            }
            if (!requirementMet) {
                oss << " (미달 — PoE 효과 비활성)";
            }
            card.requirementText = oss.str();
        }

        // 로컬 모디파이어 적용 후 베이스 스탯 요약(StatEngine과 동일한 0 하한 클램핑, 검수 m4)
        const auto& s = computedSlot.stats;
        if (computedSlot.isWeapon) {
            const double physMult = std::max(0.0, 1.0 + s.localIncPhysPct / 100.0);
            const double pMin = std::max(0.0, (computedSlot.baseWeaponPhysMin + s.localFlatPhysMin) * physMult);
            const double pMax = std::max(pMin, (computedSlot.baseWeaponPhysMax + s.localFlatPhysMax) * physMult);
            const double crit = std::max(0.0, computedSlot.baseWeaponCritChancePct * std::max(0.0, 1.0 + s.localIncCritChancePct / 100.0));
            const double aps = std::max(0.1, computedSlot.baseWeaponAttackRate * std::max(0.1, 1.0 + s.localIncAttackSpeedPct / 100.0));
            card.baseStatLines.push_back("물리 피해: " + FormatNumber(pMin, false) + "~" + FormatNumber(pMax, false));
            if (s.localFlatFireMax > 0.0) {
                card.baseStatLines.push_back("화염 피해: " + FormatNumber(std::max(0.0, s.localFlatFireMin), false) + "~" + FormatNumber(s.localFlatFireMax, false));
            }
            if (s.localFlatColdMax > 0.0) {
                card.baseStatLines.push_back("냉기 피해: " + FormatNumber(std::max(0.0, s.localFlatColdMin), false) + "~" + FormatNumber(s.localFlatColdMax, false));
            }
            if (s.localFlatLightningMax > 0.0) {
                card.baseStatLines.push_back("번개 피해: " + FormatNumber(std::max(0.0, s.localFlatLightningMin), false) + "~" + FormatNumber(s.localFlatLightningMax, false));
            }
            if (s.localFlatChaosMax > 0.0) {
                card.baseStatLines.push_back("카오스 피해: " + FormatNumber(std::max(0.0, s.localFlatChaosMin), false) + "~" + FormatNumber(s.localFlatChaosMax, false));
            }
            card.baseStatLines.push_back("치명타 확률: " + FormatNumber(crit, true, 2) + "% · 초당 공격: " + FormatNumber(aps, true, 2));
        } else {
            const double arm = std::max(0.0, (computedSlot.baseArmour + s.localFlatArmour) * std::max(0.0, 1.0 + s.localIncArmourPct / 100.0));
            const double eva = std::max(0.0, (computedSlot.baseEvasion + s.localFlatEvasion) * std::max(0.0, 1.0 + s.localIncEvasionPct / 100.0));
            const double es = std::max(0.0, (computedSlot.baseEnergyShield + s.localFlatEnergyShield) * std::max(0.0, 1.0 + s.localIncEnergyShieldPct / 100.0));
            const double blk = std::max(0.0, computedSlot.baseBlockChance * std::max(0.0, 1.0 + s.localIncBlockPct / 100.0));
            if (arm > 0.5) card.baseStatLines.push_back("방어도: " + FormatNumber(arm, false));
            if (eva > 0.5) card.baseStatLines.push_back("회피: " + FormatNumber(eva, false));
            if (es > 0.5) card.baseStatLines.push_back("에너지 실드: " + FormatNumber(es, false));
            if (blk > 0.5) card.baseStatLines.push_back("막기 확률: " + FormatNumber(blk, false) + "%");
        }

        if (!record) {
            return card;
        }

        // 5단계: 임플리싯 어픽스 분리 표시
        for (const auto& rolled : record->implicitAffixes) {
            const auto* def = data.FindAffix(rolled.affixId);
            if (!def) continue;
            std::size_t valOffset = 0;
            for (const auto& ln : def->lines) {
                FormattedAffixLine fl;
                fl.type = def->type;
                fl.tier = def->tier;
                fl.tierBadge = "[고정]";
                fl.textKo = RenderLineTemplate(ln.templateKo, rolled.values, valOffset, ln.ranges);
                card.implicitLines.push_back(std::move(fl));
                valOffset += ln.ranges.size();
            }
        }

        if (record->rarity == ItemRarity::Unique) {
            // 고유 아이템 명시 모드는 정의 순서대로 [고유] 배지로 표시
            for (const auto& rolled : record->affixes) {
                const auto* def = data.FindAffix(rolled.affixId);
                if (!def) continue;
                std::size_t valOffset = 0;
                for (const auto& ln : def->lines) {
                    FormattedAffixLine fl;
                    fl.type = def->type;
                    fl.tier = 1;
                    fl.tierBadge = "[고유]";
                    fl.textKo = RenderLineTemplate(ln.templateKo, rolled.values, valOffset, ln.ranges);
                    card.affixLines.push_back(std::move(fl));
                    valOffset += ln.ranges.size();
                }
            }
            return card;
        }

        // 접두 먼저, 접미 나중에 정렬해 표시
        auto appendByType = [&](AffixType targetType) {
            for (const auto& rolled : record->affixes) {
                const auto* def = data.FindAffix(rolled.affixId);
                if (!def || def->type != targetType) continue;
                std::size_t valOffset = 0;
                for (const auto& ln : def->lines) {
                    FormattedAffixLine fl;
                    fl.type = def->type;
                    fl.tier = def->tier;
                    fl.tierBadge = std::string(def->type == AffixType::Prefix ? "[접두 T" : "[접미 T") +
                        std::to_string(def->tier) + "]";
                    if (record->slotGroup == TokenSlotGroup::Map) {
                        fl.textKo = MapSystem::FormatMapModKoreanLine(rolled);
                    } else {
                        fl.textKo = RenderLineTemplate(ln.templateKo, rolled.values, valOffset, ln.ranges);
                    }
                    card.affixLines.push_back(std::move(fl));
                    valOffset += ln.ranges.size();
                }
            }
        };
        appendByType(AffixType::Prefix);
        appendByType(AffixType::Suffix);
        return card;
    }
}
