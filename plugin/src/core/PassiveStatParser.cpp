#include "core/PassiveStatParser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>

namespace SkyrimPoE::Core
{
    namespace
    {
        std::string ToLowerAscii(std::string_view sv)
        {
            std::string out(sv);
            std::transform(out.begin(), out.end(), out.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        std::string_view Trim(std::string_view sv)
        {
            constexpr std::string_view ws = " \t\r\n";
            const auto b = sv.find_first_not_of(ws);
            if (b == std::string_view::npos) return {};
            const auto e = sv.find_last_not_of(ws);
            return sv.substr(b, e - b + 1);
        }

        // 선두 숫자(부호 포함) 파싱 후 나머지 꼬리 문자열 반환
        bool ParseLeadingNumber(std::string_view text, double& valueOut, bool& hasPercentOut, std::string_view& restOut)
        {
            text = Trim(text);
            if (text.empty()) {
                return false;
            }
            std::size_t idx = 0;
            bool neg = false;
            if (text[idx] == '+') {
                ++idx;
            } else if (text[idx] == '-') {
                neg = true;
                ++idx;
            }
            const std::size_t numStart = idx;
            bool sawDigit = false;
            bool sawDot = false;
            while (idx < text.size()) {
                const char c = text[idx];
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    sawDigit = true;
                    ++idx;
                } else if (c == '.' && !sawDot) {
                    sawDot = true;
                    ++idx;
                } else {
                    break;
                }
            }
            if (!sawDigit) {
                return false;
            }
            double val = 0.0;
            const auto [ptr, ec] = std::from_chars(text.data() + numStart, text.data() + idx, val);
            if (ec != std::errc{} || !std::isfinite(val)) {
                return false;
            }
            if (neg) {
                val = -val;
            }
            bool hasPct = false;
            if (idx < text.size() && text[idx] == '%') {
                hasPct = true;
                ++idx;
            }
            valueOut = val;
            hasPercentOut = hasPct;
            restOut = Trim(text.substr(idx));
            return true;
        }

        bool ContainsAnyConditionQualifier(std::string_view low)
        {
            // 무기/방패/조건부 한정이 붙은 줄 중 3단계 단순 전역 합산에 넣으면 과적용되는 항목을 보수적으로 분리
            return low.find("minion") != std::string_view::npos ||
                low.find("totem") != std::string_view::npos ||
                low.find("trap") != std::string_view::npos ||
                low.find("mine") != std::string_view::npos ||
                low.find("flask") != std::string_view::npos ||
                low.find("brand") != std::string_view::npos ||
                low.find("warcry") != std::string_view::npos ||
                low.find("bow") != std::string_view::npos ||
                low.find("wand") != std::string_view::npos ||
                low.find("claw") != std::string_view::npos ||
                low.find("dagger") != std::string_view::npos ||
                low.find("staff") != std::string_view::npos ||
                low.find("staves") != std::string_view::npos ||
                low.find("ailment") != std::string_view::npos ||
                low.find("damage over time") != std::string_view::npos ||
                low.find("projectile") != std::string_view::npos ||
                low.find("per ") != std::string_view::npos ||
                low.find("recently") != std::string_view::npos ||
                low.find("while holding a shield") != std::string_view::npos ||
                low.find("while dual wielding") != std::string_view::npos ||
                low.find("with shields") != std::string_view::npos ||
                low.find("from equipped") != std::string_view::npos ||
                low.find("on full life") != std::string_view::npos ||
                low.find("against ") != std::string_view::npos ||
                low.find("enemies") != std::string_view::npos ||
                low.find("allies") != std::string_view::npos;
        }
    }

    bool PassiveStatParser::ParseLine(std::string_view statLine, PassiveStatTotals& out)
    {
        const auto trimmed = Trim(statLine);
        if (trimmed.empty()) {
            return false;
        }
        const std::string low = ToLowerAscii(trimmed);

        // ---- 1. 핵심 키스톤 문구 정확 매칭 ----
        if (low == "removes all mana" || low == "skills cost life instead of mana") {
            out.bloodMagic = true;
            return true;
        }
        if (low == "skills reserve life instead of mana") {
            out.bloodMagic = true;
            return true;
        }
        if (low == "maximum life becomes 1, immune to chaos damage") {
            out.chaosInoculation = true;
            return true;
        }
        if (low == "your hits can't be evaded") {
            // PoE 3.30 트리에서 "Your hits can't be Evaded"는 Resolute Technique(31961) 전용 문구.
            // "Never deal Critical Strikes" 단독 매칭을 피함으로써 Precise Technique(63620) 패널티 단독 오적용 방지.
            out.resoluteTechnique = true;
            return true;
        }
        if (low == "never deal critical strikes") {
            return false;
        }
        if (low.find("converts all evasion rating to armour") != std::string::npos) {
            out.ironReflexes = true;
            return true;
        }
        if (low == "cannot evade enemy attacks") {
            out.cannotEvade = true;
            return true;
        }
        if (low == "cannot be stunned") {
            out.unwaveringStance = true;
            return true;
        }
        if (low.find("modifiers to chance to suppress spell damage instead apply to chance to dodge spell hits") != std::string::npos ||
            low == "maximum chance to dodge spell hits is 75%") {
            out.acrobatics = true;
            return true;
        }
        if (low == "your critical strikes do not deal extra damage") {
            out.elementalOverload = true;
            return true;
        }
        if (low == "30% more spell damage when on low life") {
            out.painAttunement = true;
            return true;
        }
        if (low == "strength's damage bonus applies to all spell damage as well") {
            out.ironWill = true;
            return true;
        }

        // PoE 3.30 비선두 수치 문구: "Prevent +N% of Suppressed Spell Damage" (조건부 꼬리 제외)
        if (low.starts_with("prevent ")) {
            double pVal = 0.0;
            bool pPct = false;
            std::string_view pRest;
            if (ParseLeadingNumber(std::string_view(low).substr(8), pVal, pPct, pRest) &&
                pPct && pRest == "of suppressed spell damage") {
                out.spellSuppressionPrevent += pVal;
                return true;
            }
        }

        // 재검수 R-1: PoE 3.30 트리의 공격 한정 상태이상 문구는 숫자 앞에 "Attacks have"가 붙는다.
        // ("Attacks have N% chance to cause Bleeding" 12줄(15% 9, 10% 3), "Attacks have N% chance to Ignite" 7줄)
        if (low.starts_with("attacks have ")) {
            double aVal = 0.0;
            bool aPct = false;
            std::string_view aRest;
            if (ParseLeadingNumber(std::string_view(low).substr(13), aVal, aPct, aRest) && aPct) {
                if (aRest == "chance to cause bleeding") {
                    out.attackBleedChancePct += aVal;
                    return true;
                }
                if (aRest == "chance to ignite") {
                    out.attackIgniteChancePct += aVal;
                    return true;
                }
            }
            return false;
        }

        // ---- 2. 선두 수치 기반 스탯 파싱 ----
        double val = 0.0;
        bool pct = false;
        std::string_view rest;
        if (!ParseLeadingNumber(low, val, pct, rest)) {
            return false;
        }

        // Mind Over Matter (수치 기반 키스톤)
        if (pct && rest == "of damage is taken from mana before life") {
            out.mindOverMatterRatio = std::clamp(out.mindOverMatterRatio + val / 100.0, 0.0, 1.0);
            return true;
        }

        // Blood Magic 10% more maximum Life
        if (pct && rest == "more maximum life") {
            out.moreLifeMult *= std::max(0.0, 1.0 + val / 100.0);
            return true;
        }
        if (pct && rest == "more maximum mana") {
            out.moreManaMult *= std::max(0.0, 1.0 + val / 100.0);
            return true;
        }

        // 조건부·타 시스템 한정 줄은 오적용 방지를 위해 여기서 스킵
        if (ContainsAnyConditionQualifier(rest)) {
            return false;
        }

        // ---- 2a. 고정 수치 (pct == false) ----
        if (!pct) {
            if (rest == "to strength") { out.flatStr += val; return true; }
            if (rest == "to dexterity") { out.flatDex += val; return true; }
            if (rest == "to intelligence") { out.flatInt += val; return true; }
            if (rest == "to strength and dexterity") { out.flatStr += val; out.flatDex += val; return true; }
            if (rest == "to strength and intelligence") { out.flatStr += val; out.flatInt += val; return true; }
            if (rest == "to dexterity and intelligence") { out.flatDex += val; out.flatInt += val; return true; }
            if (rest == "to all attributes") { out.flatStr += val; out.flatDex += val; out.flatInt += val; return true; }

            if (rest == "to maximum life") { out.flatLife += val; return true; }
            if (rest == "to maximum mana") { out.flatMana += val; return true; }
            if (rest == "to maximum energy shield") { out.flatEnergyShield += val; return true; }
            if (rest == "to armour") { out.flatArmour += val; return true; }
            if (rest == "to evasion rating") { out.flatEvasion += val; return true; }
            if (rest == "to armour and evasion rating" || rest == "to evasion rating and armour") {
                out.flatArmour += val; out.flatEvasion += val; return true;
            }

            if (rest == "to maximum endurance charges") { out.maxEnduranceBonus += static_cast<std::int32_t>(std::llround(val)); return true; }
            if (rest == "to maximum frenzy charges") { out.maxFrenzyBonus += static_cast<std::int32_t>(std::llround(val)); return true; }
            if (rest == "to maximum power charges") { out.maxPowerBonus += static_cast<std::int32_t>(std::llround(val)); return true; }
            return false;
        }

        // ---- 2b. 퍼센트 수치 (pct == true) ----
        // 능력치 증가
        if (rest == "increased strength") { out.incStrPct += val; return true; }
        if (rest == "increased dexterity") { out.incDexPct += val; return true; }
        if (rest == "increased intelligence") { out.incIntPct += val; return true; }
        if (rest == "increased attributes") { out.incStrPct += val; out.incDexPct += val; out.incIntPct += val; return true; }

        // 풀 & ES 재충전
        if (rest == "increased maximum life") { out.incLifePct += val; return true; }
        if (rest == "reduced maximum life") { out.incLifePct -= val; return true; }
        if (rest == "increased maximum mana") { out.incManaPct += val; return true; }
        if (rest == "reduced maximum mana") { out.incManaPct -= val; return true; }
        if (rest == "increased maximum energy shield") { out.incEnergyShieldPct += val; return true; }
        if (rest == "increased energy shield recharge rate") { out.incEsRechargeRatePct += val; return true; }
        if (rest == "faster start of energy shield recharge") { out.fasterEsRechargeStartPct += val; return true; }

        // 방어·회피
        if (rest == "increased armour") { out.incArmourPct += val; return true; }
        if (rest == "increased evasion rating") { out.incEvasionPct += val; return true; }
        if (rest == "increased evasion rating and armour" || rest == "increased armour and evasion rating" ||
            rest == "increased armour and evasion") {
            out.incArmourAndEvasionPct += val; return true;
        }
        if (rest == "increased armour and energy shield" || rest == "increased armour and maximum energy shield") {
            out.incArmourPct += val; out.incEnergyShieldPct += val; return true;
        }
        if (rest == "increased evasion rating and energy shield" || rest == "increased evasion rating and maximum energy shield") {
            out.incEvasionPct += val; out.incEnergyShieldPct += val; return true;
        }
        if (rest == "increased global defences") {
            out.incArmourAndEvasionPct += val; out.incEnergyShieldPct += val; return true;
        }
        if (rest == "additional physical damage reduction") { out.physDamageReduction += val; return true; }

        // 저항 및 최대 저항 상한
        if (rest == "to fire resistance") { out.fireResist += val; return true; }
        if (rest == "to cold resistance") { out.coldResist += val; return true; }
        if (rest == "to lightning resistance") { out.lightningResist += val; return true; }
        if (rest == "to chaos resistance") { out.chaosResist += val; return true; }
        if (rest == "to fire and cold resistances") { out.fireResist += val; out.coldResist += val; return true; }
        if (rest == "to fire and lightning resistances") { out.fireResist += val; out.lightningResist += val; return true; }
        if (rest == "to cold and lightning resistances") { out.coldResist += val; out.lightningResist += val; return true; }
        if (rest == "to all elemental resistances") {
            out.fireResist += val; out.coldResist += val; out.lightningResist += val; return true;
        }
        if (rest == "to maximum fire resistance") { out.maxFireResistBonus += val; return true; }
        if (rest == "to maximum cold resistance") { out.maxColdResistBonus += val; return true; }
        if (rest == "to maximum lightning resistance") { out.maxLightningResistBonus += val; return true; }
        if (rest == "to maximum chaos resistance") { out.maxChaosResistBonus += val; return true; }
        if (rest == "to maximum all elemental resistances" || rest == "to all maximum elemental resistances") {
            out.maxFireResistBonus += val; out.maxColdResistBonus += val; out.maxLightningResistBonus += val; return true;
        }

        // 막기·주문 억제
        if (rest == "chance to block attack damage") { out.attackBlockChance += val; return true; }
        if (rest == "chance to block spell damage") { out.spellBlockChance += val; return true; }
        if (rest == "to maximum chance to block attack damage") { out.attackBlockMaxBonusPct += val; return true; }
        if (rest == "to maximum chance to block spell damage") { out.spellBlockMaxBonusPct += val; return true; }
        if (rest == "chance to suppress spell damage") { out.spellSuppressionChance += val; return true; }
        if (rest == "to amount of suppressed spell damage prevented") { out.spellSuppressionPrevent += val; return true; }

        // 피해 증가
        if (rest == "increased damage") { out.incGlobalDamagePct += val; return true; }
        if (rest == "increased physical damage") { out.incPhysicalDamagePct += val; return true; }
        if (rest == "increased melee damage") { out.incMeleeDamagePct += val; return true; }
        if (rest == "increased melee physical damage") { out.incMeleePhysicalDamagePct += val; return true; }
        if (rest == "increased attack damage") { out.incAttackDamagePct += val; return true; }
        if (rest == "increased physical attack damage" || rest == "increased attack physical damage") {
            out.incPhysicalAttackDamagePct += val; return true;
        }
        if (rest == "increased elemental damage") { out.incElementalDamagePct += val; return true; }
        if (rest == "increased elemental damage with attack skills") { out.incElementalAttackDamagePct += val; return true; }
        if (rest == "increased fire damage") { out.incFireDamagePct += val; return true; }
        if (rest == "increased cold damage") { out.incColdDamagePct += val; return true; }
        if (rest == "increased lightning damage") { out.incLightningDamagePct += val; return true; }
        if (rest == "increased chaos damage") { out.incChaosDamagePct += val; return true; }
        if (rest == "increased spell damage") { out.incSpellDamagePct += val; return true; }
        if (rest == "increased area damage") { out.incAreaDamagePct += val; return true; }

        // 속도
        if (rest == "increased attack speed") { out.incAttackSpeedPct += val; return true; }
        if (rest == "reduced attack speed") { out.incAttackSpeedPct -= val; return true; }
        if (rest == "increased melee attack speed") { out.incMeleeAttackSpeedPct += val; return true; }
        if (rest == "increased cast speed") { out.incCastSpeedPct += val; return true; }
        if (rest == "increased attack and cast speed") { out.incAttackSpeedPct += val; out.incCastSpeedPct += val; return true; }

        // 치명타
        if (rest == "increased critical strike chance" || rest == "increased global critical strike chance") {
            out.incGlobalCritChancePct += val; return true;
        }
        if (rest == "increased critical strike chance for attacks") { out.incAttackCritChancePct += val; return true; }
        if (rest == "increased melee critical strike chance") { out.incMeleeCritChancePct += val; return true; }
        if (rest == "increased critical strike chance for spells" || rest == "increased spell critical strike chance") {
            out.incSpellCritChancePct += val; return true;
        }
        if (rest == "to critical strike multiplier" || rest == "to global critical strike multiplier") {
            out.globalCritMultiplierPct += val; return true;
        }
        if (rest == "to melee critical strike multiplier" || rest == "to critical strike multiplier for melee") {
            out.meleeCritMultiplierPct += val; return true;
        }
        if (rest == "to critical strike multiplier for spells" || rest == "to spell critical strike multiplier" ||
            rest == "to critical strike multiplier for spell damage") {
            out.spellCritMultiplierPct += val; return true;
        }

        // 범위 & 비용 (PoE 3.30 Cost Efficiency 포함)
        if (rest == "increased area of effect") { out.incAreaOfEffectPct += val; return true; }
        if (rest == "reduced mana cost of skills") { out.reducedManaCostPct += val; return true; }
        if (rest == "increased mana cost of skills") { out.reducedManaCostPct -= val; return true; }
        if (rest == "increased mana cost efficiency" || rest == "increased cost efficiency") {
            out.costEfficiencyPct += val; return true;
        }
        if (rest == "increased cost efficiency of attacks") {
            out.attackCostEfficiencyPct += val; return true;
        }
        if (rest == "increased mana cost efficiency of spells") {
            out.spellCostEfficiencyPct += val; return true;
        }

        // 5단계(N-1): 상태이상 유발 확률 (PoB 3.30 원문 패턴)
        if (rest == "chance to ignite") { out.igniteChancePct += val; return true; }
        if (rest == "chance to freeze") { out.freezeChancePct += val; return true; }
        if (rest == "chance to shock") { out.shockChancePct += val; return true; }
        if (rest == "chance to freeze, shock and ignite") {
            out.freezeChancePct += val;
            out.shockChancePct += val;
            out.igniteChancePct += val;
            return true;
        }
        if (rest == "chance to cause bleeding with attacks") {
            out.attackBleedChancePct += val; return true;
        }
        if (rest == "chance to cause bleeding" || rest == "chance to bleed") {
            out.bleedChancePct += val; return true;
        }
        if (rest == "chance to poison on hit" || rest == "chance to poison") {
            out.poisonChancePct += val; return true;
        }
        if (rest == "chance to poison on hit with attacks") {
            out.attackPoisonChancePct += val; return true;
        }
        if (rest == "chance to poison on hit with spell damage") {
            out.spellPoisonChancePct += val; return true;
        }

        return false;
    }

    PassiveStatTotals PassiveStatParser::AggregateTree(
        const PassiveTreeData& data,
        const PassiveTreeState& state)
    {
        PassiveStatTotals totals;
        if (!data.IsLoaded() || !state.classSelected) {
            return totals;
        }

        auto processStats = [&](const std::vector<std::string>& lines) {
            for (const auto& line : lines) {
                if (ParseLine(line, totals)) {
                    ++totals.parsedStatLines;
                } else {
                    ++totals.unparsedStatLines;
                }
            }
        };

        for (const auto nid : state.allocatedNodes) {
            const auto* node = data.FindNode(nid);
            if (!node) continue;
            processStats(node->stats);
        }

        for (const auto& [mNodeId, effId] : state.masterySelections) {
            (void)mNodeId;
            const auto* eff = data.FindMasteryEffect(effId);
            if (!eff) continue;
            processStats(eff->stats);
        }

        return totals;
    }
}
