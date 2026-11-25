#include "core/PassiveStatParser.h"
#include "core/PassiveTree.h"

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

    bool PassiveStatParser::ParsePoe2Line(std::string_view statLine, PassiveStatTotals& out, bool keystone)
    {
        const auto normalized = ToLowerAscii(Trim(statLine));
        const std::string_view line = normalized;
        if (keystone) {
            // Check the whole keystone description before applying any of its effects.
            if (line == "75% of damage converted to fire damage") out.convertToFirePct = 75.0;
            else if (line == "deal no non-fire damage") out.dealOnlyFire = true;
            else if (line == "fire spells convert 100% of fire damage to chaos damage") out.convertFireSpellToChaosPct = 100.0;
            else if (line == "chaos damage from fire spells contributes to flammability and ignite magnitudes") out.chaosFireSpellIgniteMagnitude = true;
            else if (line == "ignite inflicted with fire spells deals chaos damage instead of fire damage") out.fireSpellIgniteDealsChaos = true;
            else if (line == "you have no mana" || line == "skill mana costs converted to life costs") out.bloodMagic = true;
            else if (line == "maximum life is 1") out.chaosInoculation = true;
            else if (line == "immune to chaos damage and bleeding") out.chaosAndBleedImmune = true;
            else if (line == "25% more skill speed while off hand is empty and you have a one-handed martial weapon equipped in your main hand") {
                out.moreOneHandEmptyOffHandSpeedPct = 25.0;
            }
            else if (line == "convert 100% of maximum energy shield to maximum mana") out.convertEsToManaPct = 100.0;
            else if (line == "mana costs are doubled") out.moreManaCostMult = 2.0;
            else if (line == "chance to evade is unlucky") out.unluckyEvade = true;
            else if (line == "chance to deflect is lucky") out.luckyDeflect = true;
            else if (line == "converts all evasion rating to armour") out.ironReflexes = true;
            else if (line == "all damage is taken from mana before life") out.mindOverMatterRatio = 1.0;
            else if (line == "50% less mana recovery rate") out.manaRecoveryMult = 0.5;
            else if (line == "30% less critical damage bonus when on full life") out.fullLifeCritBonusMult = 0.7;
            else if (line == "30% more critical damage bonus when on low life") out.lowLifeCritBonusMult = 1.3;
            else if (line == "gain power charges instead of frenzy charges") out.chargeGain[static_cast<std::size_t>(ChargeKind::Frenzy)] = ChargeKind::Power;
            else if (line == "gain frenzy charges instead of endurance charges") out.chargeGain[static_cast<std::size_t>(ChargeKind::Endurance)] = ChargeKind::Frenzy;
            else if (line == "gain endurance charges instead of power charges") out.chargeGain[static_cast<std::size_t>(ChargeKind::Power)] = ChargeKind::Endurance;
            else if (line == "excess life recovery from regeneration is applied to energy shield") out.excessLifeRegenToEs = true;
            else if (line == "energy shield does not recharge") out.esCannotRecharge = true;
            else if (line == "bleeding you inflict is aggravated") out.alwaysAggravateBleed = true;
            else if (line == "base bleeding duration is 1 second") out.bleedBaseDuration = 1.0;
            else if (line == "50% more magnitude of bleeding you inflict") out.bleedMagnitudeMult = 1.5;
            else return false;
            return true;
        }

        if (line == "energy shield does not recharge") { out.esCannotRecharge = true; return true; }
        if (line == "deflected hits cannot inflict bleeding on you") { out.deflectPreventsBleed = true; return true; }
        if (line == "skills gain a base life cost equal to base mana cost") { out.matchingBaseLifeCost = true; return true; }
        if (line == "skills fire an additional projectile") { out.additionalProjectiles += 1.0; return true; }
        if (line == "no inherent mana regeneration") { out.noInherentManaRegen = true; return true; }
        if (line == "you have no spirit") { out.noSpirit = true; return true; }

        using Member = double PassiveStatTotals::*;
        struct Stat
        {
            std::string_view words;
            Member field;
        };
        const auto apply = [&](const auto& stats, std::string_view words, double value) {
            for (const auto& stat : stats) {
                if (stat.words == words) {
                    out.*stat.field += value;
                    return true;
                }
            }
            return false;
        };
        double value = 0.0;
        bool percent = false;
        std::string_view tail;
        if (ParseLeadingNumber(line, value, percent, tail)) {
            if (!percent && tail == "to spirit") { out.flatSpirit += value; return true; }
            if (!percent && tail == "to deflection rating") { out.flatDeflection += value; return true; }
            if (percent) {
                static constexpr Stat increased[]{
                    { "mana regeneration rate", &PassiveStatTotals::incManaRegenPct },
                    { "mana regeneration rate while on full life", &PassiveStatTotals::fullLifeManaRegenIncPct },
                    { "mana regeneration rate while moving", &PassiveStatTotals::movingManaRegenIncPct },
                    { "mana regeneration rate while stationary", &PassiveStatTotals::stationaryManaRegenIncPct },
                    { "block chance", &PassiveStatTotals::incBlockChancePct },
                    { "critical hit chance", &PassiveStatTotals::incGlobalCritChancePct },
                    { "critical hit chance for spells", &PassiveStatTotals::incSpellCritChancePct },
                    { "critical hit chance for attacks", &PassiveStatTotals::incAttackCritChancePct },
                    { "melee critical hit chance", &PassiveStatTotals::incMeleeCritChancePct },
                    { "critical damage bonus", &PassiveStatTotals::globalCritBonusIncPct },
                    { "critical damage bonus for attack damage", &PassiveStatTotals::attackCritBonusIncPct },
                    { "critical spell damage bonus", &PassiveStatTotals::spellCritBonusIncPct },
                    { "area of effect for attacks", &PassiveStatTotals::attackAreaIncPct },
                    { "projectile damage", &PassiveStatTotals::projectileDamageIncPct },
                    { "flammability magnitude", &PassiveStatTotals::flammabilityIncPct },
                    { "life regeneration rate", &PassiveStatTotals::incLifeRegenPct },
                    { "life regeneration rate while moving", &PassiveStatTotals::movingLifeRegenIncPct },
                    { "life regeneration rate while stationary", &PassiveStatTotals::stationaryLifeRegenIncPct },
                    { "life recovery rate", &PassiveStatTotals::incLifeRecoveryPct },
                    { "mana recovery rate", &PassiveStatTotals::incManaRecoveryPct },
                    { "freeze buildup", &PassiveStatTotals::incFreezeBuildupPct },
                    { "elemental ailment threshold", &PassiveStatTotals::elementalAilmentThresholdIncPct },
                    { "freeze threshold", &PassiveStatTotals::freezeThresholdIncPct },
                    { "attack area damage", &PassiveStatTotals::attackAreaDamageIncPct },
                    { "spell area damage", &PassiveStatTotals::spellAreaDamageIncPct },
                    { "cooldown recovery rate", &PassiveStatTotals::cooldownRecoveryIncPct },
                    { "skill effect duration", &PassiveStatTotals::skillDurationIncPct },
                    { "skill speed with channelling skills", &PassiveStatTotals::channellingSpeedIncPct },
                    { "chance to inflict ailments", &PassiveStatTotals::incAilmentChancePct },
                    { "magnitude of ailments you inflict", &PassiveStatTotals::incAilmentMagnitudePct },
                    { "deflection rating", &PassiveStatTotals::incDeflectionPct },
                    { "deflection rating while moving", &PassiveStatTotals::movingDeflectionIncPct },
                    { "spirit", &PassiveStatTotals::incSpiritPct },
                    { "elemental damage with attacks", &PassiveStatTotals::incElementalAttackDamagePct },
                    { "movement speed", &PassiveStatTotals::incMovementSpeedPct },
                    { "movement speed while sprinting", &PassiveStatTotals::incSprintingMovementSpeedPct },
                    { "movement speed while dual wielding", &PassiveStatTotals::incDualWieldMovementSpeedPct },
                    { "movement speed while you have energy shield", &PassiveStatTotals::incEsMovementSpeedPct },
                    { "movement speed while you have an active charm", &PassiveStatTotals::incCharmMovementSpeedPct },
                    { "life cost of skills", &PassiveStatTotals::incLifeCostPct },
                    { "cost efficiency", &PassiveStatTotals::costEfficiencyPct },
                    { "cost efficiency of skills", &PassiveStatTotals::costEfficiencyPct },
                    { "cost efficiency of attacks", &PassiveStatTotals::attackCostEfficiencyPct },
                    { "cost efficiency of spells", &PassiveStatTotals::spellCostEfficiencyPct },
                    { "mana cost efficiency", &PassiveStatTotals::manaCostEfficiencyPct },
                    { "mana cost efficiency of skills", &PassiveStatTotals::manaCostEfficiencyPct },
                    { "mana cost efficiency of attacks", &PassiveStatTotals::manaAttackCostEfficiencyPct },
                    { "mana cost efficiency of spells", &PassiveStatTotals::manaSpellCostEfficiencyPct },
                    { "life cost efficiency", &PassiveStatTotals::lifeCostEfficiencyPct },
                    { "life cost efficiency of skills", &PassiveStatTotals::lifeCostEfficiencyPct },
                    { "spell damage with spells that cost life", &PassiveStatTotals::lifeCostSpellDamageIncPct },
                    { "projectile speed", &PassiveStatTotals::projectileSpeedIncPct },
                    { "projectile speed for spell skills", &PassiveStatTotals::spellProjectileSpeedIncPct },
                    { "chance to inflict ailments with projectiles", &PassiveStatTotals::projectileAilmentChanceIncPct },
                    { "reservation efficiency of herald skills", &PassiveStatTotals::heraldReservationEfficiencyPct },
                    { "attack and cast speed with elemental skills", &PassiveStatTotals::elementalSkillSpeedIncPct },
                    { "attack and cast speed with lightning skills", &PassiveStatTotals::lightningSkillSpeedIncPct },
                    { "cast speed with cold skills", &PassiveStatTotals::coldCastSpeedIncPct },
                    { "cast speed when on low life", &PassiveStatTotals::lowLifeCastSpeedIncPct },
                    { "cast speed when on full life", &PassiveStatTotals::fullLifeCastSpeedIncPct },
                    { "cast speed while on low life", &PassiveStatTotals::lowLifeCastSpeedIncPct },
                    { "cast speed while on full life", &PassiveStatTotals::fullLifeCastSpeedIncPct },
                    { "armour and evasion rating when on low life", &PassiveStatTotals::lowLifeArmourAndEvasionIncPct },
                    { "evasion rating when on full life", &PassiveStatTotals::fullLifeEvasionIncPct },
                    { "evasion rating while moving", &PassiveStatTotals::movingEvasionIncPct },
                    { "armour while stationary", &PassiveStatTotals::stationaryArmourIncPct },
                    { "armour from equipped body armour", &PassiveStatTotals::bodyArmourIncPct },
                    { "evasion rating from equipped body armour", &PassiveStatTotals::bodyEvasionIncPct },
                    { "energy shield from equipped body armour", &PassiveStatTotals::bodyEsIncPct },
                    { "energy shield from equipped helmet", &PassiveStatTotals::helmetEsIncPct },
                    { "attack cold damage", &PassiveStatTotals::attackColdDamageIncPct },
                    { "armour, evasion and energy shield from equipped shield", &PassiveStatTotals::shieldDefencesIncPct },
                    { "attack damage while dual wielding", &PassiveStatTotals::dualWieldAttackDamageIncPct },
                    { "attack speed while dual wielding", &PassiveStatTotals::dualWieldAttackSpeedIncPct },
                    { "attack damage while on low life", &PassiveStatTotals::lowLifeAttackDamageIncPct },
                    { "attack damage while on full life", &PassiveStatTotals::fullLifeAttackDamageIncPct },
                    { "attack damage while moving", &PassiveStatTotals::movingAttackDamageIncPct },
                    { "life regeneration rate while on low life", &PassiveStatTotals::lowLifeLifeRegenIncPct },
                    { "spell damage while wielding a melee weapon", &PassiveStatTotals::meleeWeaponSpellDamageIncPct },
                    { "damage with hits against enemies affected by elemental ailments", &PassiveStatTotals::hitDamageIncAgainstElementalAilmentsPct },
                    { "damage with hits against enemies that are affected by elemental ailments", &PassiveStatTotals::hitDamageIncAgainstElementalAilmentsPct },
                    { "damage with hits against enemies affected by ailments", &PassiveStatTotals::hitDamageIncAgainstAnyAilmentsPct },
                    { "damage with hits against ignited enemies", &PassiveStatTotals::hitDamageIncAgainstIgnitedPct },
                    { "damage with hits against burning enemies", &PassiveStatTotals::hitDamageIncAgainstBurningPct },
                    { "damage with hits against frozen enemies", &PassiveStatTotals::hitDamageIncAgainstFrozenPct },
                };
                bool reduced = tail.starts_with("reduced ");
                if (reduced || tail.starts_with("increased ")) {
                    const auto words = tail.substr(reduced ? 8 : 10);
                    const double change = reduced ? -value : value;
                    if (apply(increased, words, change)) return true;
                    const auto with = words.find(" with ");
                    if (with != std::string_view::npos) {
                        static constexpr std::pair<std::string_view, PassiveWeapon> weapons[]{
                            { "one handed weapons", PassiveWeapon::OneHand },
                            { "one handed melee weapons", PassiveWeapon::OneHand },
                            { "two handed weapons", PassiveWeapon::TwoHand },
                            { "two handed melee weapons", PassiveWeapon::TwoHand },
                            { "swords", PassiveWeapon::Sword },
                            { "axes", PassiveWeapon::Axe },
                            { "maces", PassiveWeapon::Mace },
                            { "daggers", PassiveWeapon::Dagger },
                        };
                        for (const auto& [weapon, kind] : weapons) {
                            if (weapon != words.substr(with + 6)) continue;
                            auto& stats = out.weapons[static_cast<std::size_t>(kind)];
                            const auto stat = words.substr(0, with);
                            if (stat == "damage") stats.damageIncPct += change;
                            else if (stat == "attack speed") stats.speedIncPct += change;
                            else if (stat == "critical hit chance") stats.critChanceIncPct += change;
                            else if (stat == "critical damage bonus") stats.critBonusIncPct += change;
                            else break;
                            return true;
                        }
                    }
                    if (words == "skill speed") {
                        out.incAttackSpeedPct += change;
                        out.incCastSpeedPct += change;
                        return true;
                    }
                    if (words == "armour, evasion and energy shield") {
                        out.incArmourAndEvasionPct += change;
                        out.incEnergyShieldPct += change;
                        return true;
                    }
                    if (words == "maximum life, mana and energy shield") {
                        out.incLifePct += change;
                        out.incManaPct += change;
                        out.incEnergyShieldPct += change;
                        return true;
                    }
                    struct AilmentStat
                    {
                        std::string_view words;
                        AilmentKind kind;
                        double PassiveAilmentStats::*field;
                    };
                    static constexpr AilmentStat ailments[]{
                        { "chance to shock", AilmentKind::Shock, &PassiveAilmentStats::chanceIncPct },
                        { "chance to poison", AilmentKind::Poison, &PassiveAilmentStats::chanceIncPct },
                        { "chance to ignite", AilmentKind::Ignite, &PassiveAilmentStats::chanceIncPct },
                        { "ignite magnitude", AilmentKind::Ignite, &PassiveAilmentStats::magnitudeIncPct },
                        { "magnitude of poison you inflict", AilmentKind::Poison, &PassiveAilmentStats::magnitudeIncPct },
                        { "magnitude of bleeding you inflict", AilmentKind::Bleed, &PassiveAilmentStats::magnitudeIncPct },
                        { "magnitude of shock you inflict", AilmentKind::Shock, &PassiveAilmentStats::magnitudeIncPct },
                        { "magnitude of chill you inflict", AilmentKind::Chill, &PassiveAilmentStats::magnitudeIncPct },
                        { "ignite duration on enemies", AilmentKind::Ignite, &PassiveAilmentStats::durationIncPct },
                        { "shock duration", AilmentKind::Shock, &PassiveAilmentStats::durationIncPct },
                        { "chill duration on enemies", AilmentKind::Chill, &PassiveAilmentStats::durationIncPct },
                        { "poison duration", AilmentKind::Poison, &PassiveAilmentStats::durationIncPct },
                        { "bleeding duration", AilmentKind::Bleed, &PassiveAilmentStats::durationIncPct },
                        { "freeze duration on enemies", AilmentKind::Freeze, &PassiveAilmentStats::durationIncPct },
                    };
                    for (const auto& stat : ailments) {
                        if (words == stat.words) {
                            out.ailments[static_cast<std::size_t>(stat.kind)].*stat.field += change;
                            return true;
                        }
                    }
                    if (words == "duration of damaging ailments on enemies") {
                        for (auto kind : { AilmentKind::Bleed, AilmentKind::Ignite, AilmentKind::Poison }) {
                            out.ailments[static_cast<std::size_t>(kind)].durationIncPct += change;
                        }
                        return true;
                    }
                    if (words == "duration of ignite, shock and chill on enemies" ||
                        words == "duration of elemental ailments on enemies") {
                        for (auto kind : { AilmentKind::Ignite, AilmentKind::Shock, AilmentKind::Chill }) {
                            out.ailments[static_cast<std::size_t>(kind)].durationIncPct += change;
                        }
                        if (words == "duration of elemental ailments on enemies") {
                            out.ailments[static_cast<std::size_t>(AilmentKind::Freeze)].durationIncPct += change;
                        }
                        return true;
                    }
                    if (words == "chill and freeze duration on enemies") {
                        out.ailments[static_cast<std::size_t>(AilmentKind::Chill)].durationIncPct += change;
                        out.ailments[static_cast<std::size_t>(AilmentKind::Freeze)].durationIncPct += change;
                        return true;
                    }
                    if (words == "magnitude of damaging ailments you inflict" ||
                        words == "magnitude of non-damaging ailments you inflict") {
                        const bool damaging = words.starts_with("magnitude of damaging");
                        for (const auto kind : damaging
                            ? std::array{ AilmentKind::Ignite, AilmentKind::Bleed, AilmentKind::Poison }
                            : std::array{ AilmentKind::Chill, AilmentKind::Shock, AilmentKind::Freeze }) {
                            if (kind != AilmentKind::Freeze) {
                                out.ailments[static_cast<std::size_t>(kind)].magnitudeIncPct += change;
                            }
                        }
                        return true;
                    }
                    if (reduced && (words == "area of effect" || words == "cast speed" || words == "damage" ||
                        words == "spell damage" || words == "armour" || words == "maximum energy shield" ||
                        words == "energy shield recharge rate")) {
                        return ParseLine(std::to_string(-value) + "% increased " + std::string(words), out);
                    }
                }
                if (tail == "chance to inflict bleeding on hit") { out.bleedChancePct += value; return true; }
                if (tail == "chance to inflict poison on hit") { out.poisonChancePct += value; return true; }
                if (tail == "of skill mana costs converted to life costs") { out.manaAsLifeCostPct += value; return true; }
                if (tail == "of damage taken bypasses energy shield") { out.energyShieldBypassPct += value; return true; }
                if (tail == "of spell mana cost converted to life cost") { out.spellManaAsLifeCostPct += value; return true; }
                if (tail == "chance to pierce an enemy") { out.projectilePierceChancePct += value; return true; }
                if (tail == "surpassing chance to fire an additional projectile") { out.additionalProjectileChancePct += value; return true; }
                if (tail == "chance for lightning skills to chain an additional time") { out.additionalChainChancePct += value; return true; }
                constexpr std::string_view spellProj = "chance for spell skills to fire ";
                if (tail.starts_with(spellProj)) {
                    double count = 0.0;
                    bool countPct = false;
                    std::string_view suffix;
                    if (ParseLeadingNumber(tail.substr(spellProj.size()), count, countPct, suffix) && !countPct &&
                        count >= 1 && count < out.additionalSpellProjectilesChancePct.size() && std::floor(count) == count &&
                        suffix == "additional projectiles") {
                        out.additionalSpellProjectilesChancePct[static_cast<std::size_t>(count)] += value;
                        return true;
                    }
                }
                if (tail == "of damage is taken from mana before life" || tail == "of damage taken from mana before life") {
                    out.mindOverMatterRatio = std::clamp(out.mindOverMatterRatio + value / 100.0, 0.0, 1.0);
                    return true;
                }
                if (tail == "reduced duration of ailments on you") { out.reducedIncomingAilmentDurationPct += value; return true; }
                if (tail == "reduced magnitude of ignite on you") { out.reducedIncomingIgniteMagnitudePct += value; return true; }
                if (tail == "reduced effect of shock on you") { out.reducedIncomingShockMagnitudePct += value; return true; }
                if (tail == "to maximum block chance") {
                    out.attackBlockMaxBonusPct += value;
                    out.spellBlockMaxBonusPct += value;
                    return true;
                }
                if (tail == "to critical hit chance") { out.baseGlobalCritPct += value; return true; }
                if (tail == "slower start of energy shield recharge") { out.fasterEsRechargeStartPct -= value; return true; }
                if (tail == "to amount of damage prevented by deflection") { out.deflectReductionBonusPct += value; return true; }
                if (tail == "of armour also applies to elemental damage") {
                    for (auto type : { DamageType::Fire, DamageType::Cold, DamageType::Lightning }) {
                        out.armourAppliesPct[static_cast<std::size_t>(type)] += value;
                    }
                    return true;
                }
                static constexpr std::string_view armourTails[]{
                    "of armour also applies to fire damage", "of armour also applies to cold damage",
                    "of armour also applies to lightning damage"
                };
                for (std::size_t i = 0; i < std::size(armourTails); ++i) {
                    if (tail == armourTails[i]) {
                        out.armourAppliesPct[static_cast<std::size_t>(DamageType::Fire) + i] += value;
                        return true;
                    }
                }
            }
        }

        struct PrefixStat
        {
            std::string_view prefix;
            std::string_view suffix;
            Member field;
            bool percent = true;
            double sign = 1.0;
        };
        static constexpr PrefixStat prefixed[]{
            { "gain deflection rating equal to ", "of evasion rating", &PassiveStatTotals::evasionAsDeflectionPct },
            { "gain deflection rating equal to ", "of armour", &PassiveStatTotals::armourAsDeflectionPct },
            { "gain ", "of evasion rating as extra armour", &PassiveStatTotals::evasionAsExtraArmourPct },
            { "gain additional ailment threshold equal to ", "of maximum energy shield", &PassiveStatTotals::esAsAilmentThresholdPct },
            { "gain ", "of maximum energy shield as additional freeze threshold", &PassiveStatTotals::esAsFreezeThresholdPct },
            { "regenerate ", "of maximum life per second", &PassiveStatTotals::percentLifeRegen },
            { "regenerate ", "of maximum life per second while on low life", &PassiveStatTotals::lowLifePercentLifeRegen },
            { "regenerate ", "of maximum life per second while stationary", &PassiveStatTotals::stationaryPercentLifeRegen },
            { "regenerate ", "life per second", &PassiveStatTotals::flatLifeRegen, false },
            { "spell skills have ", "increased area of effect", &PassiveStatTotals::spellAreaIncPct },
            { "spell skills have ", "reduced area of effect", &PassiveStatTotals::spellAreaIncPct, true, -1.0 },
            { "triggered spells deal ", "increased spell damage", &PassiveStatTotals::triggeredSpellDamageIncPct },
            { "channelling skills deal ", "increased damage", &PassiveStatTotals::channellingDamageIncPct },
            { "attacks have ", "to critical hit chance", &PassiveStatTotals::baseAttackCritPct },
            { "prevent ", "of damage from deflected hits", &PassiveStatTotals::deflectReductionBonusPct },
            { "prevent ", "of damage from deflected critical hits", &PassiveStatTotals::deflectReductionCritBonusPct },
            { "hits against you have ", "reduced critical damage bonus", &PassiveStatTotals::reducedIncomingCritBonusPct },
            { "you take ", "of damage from blocked hits", &PassiveStatTotals::blockedHitDamageTakenPct },
            { "attack skills deal ", "increased damage while holding a shield", &PassiveStatTotals::shieldAttackDamageIncPct },
            { "projectiles have ", "chance for an additional projectile when forking", &PassiveStatTotals::additionalForkProjectileChancePct },
            { "aura skills have ", "increased magnitudes", &PassiveStatTotals::auraMagnitudeIncPct },
            { "recover ", "of maximum life on kill", &PassiveStatTotals::lifeRecoveryOnKillPct },
            { "recover ", "of maximum mana on kill", &PassiveStatTotals::manaRecoveryOnKillPct },
            { "gain ", "life per enemy killed", &PassiveStatTotals::lifeOnKill, false },
            { "gain armour equal to ", "of total strength requirements of equipped boots, gloves and helmet", &PassiveStatTotals::armourFromStrRequirementsPct },
        };
        for (const auto& stat : prefixed) {
            if (line.starts_with(stat.prefix) && ParseLeadingNumber(line.substr(stat.prefix.size()), value, percent, tail) &&
                percent == stat.percent && tail == stat.suffix) {
                out.*stat.field += value * stat.sign;
                return true;
            }
        }
        if (line.starts_with("gain ") && ParseLeadingNumber(line.substr(5), value, percent, tail) && percent) {
            auto& gain = out.extraDamage;
            if (tail == "of cold damage as extra fire damage against frozen enemies") gain.coldAsFireAgainstFrozenPct += value;
            else if (tail == "of physical damage as extra cold damage against shocked enemies") gain.physicalAsColdAgainstShockedPct += value;
            else if (tail == "of physical damage as extra lightning damage against chilled enemies") gain.physicalAsLightningAgainstChilledPct += value;
            else tail = {};
            if (!tail.empty()) return true;
        }
        if (line.starts_with("maximum block chance is ") &&
            ParseLeadingNumber(line.substr(std::string_view("maximum block chance is ").size()), value, percent, tail) &&
            percent && tail.empty() && value > 0.0) {
            out.baseBlockCapOverridePct = value;
            return true;
        }
        if (line.starts_with("maximum chance to evade is ") &&
            ParseLeadingNumber(line.substr(std::string_view("maximum chance to evade is ").size()), value, percent, tail) &&
            percent && tail.empty()) {
            out.maximumEvadePct = std::min(out.maximumEvadePct, value);
            return true;
        }
        if (line.starts_with("maximum physical damage reduction is ") &&
            ParseLeadingNumber(line.substr(std::string_view("maximum physical damage reduction is ").size()), value, percent, tail) &&
            percent && tail.empty()) {
            out.maximumPhysicalReductionPct = std::min(out.maximumPhysicalReductionPct, value);
            return true;
        }
        if (line.starts_with("adds ") && ParseLeadingNumber(line.substr(5), value, percent, tail) &&
            !percent && tail.starts_with("to ")) {
            double maximum = 0.0;
            if (ParseLeadingNumber(tail.substr(3), maximum, percent, tail) && !percent &&
                tail == "lightning damage to attacks" && value >= 0.0 && maximum >= value) {
                out.addedAttackLightningMin += value;
                out.addedAttackLightningMax += maximum;
                return true;
            }
        }
        constexpr std::string_view damageNames[]{ "physical", "fire", "cold", "lightning", "chaos" };
        const auto indexOf = [&](std::string_view name) {
            return static_cast<std::size_t>(std::ranges::find(damageNames, name) - std::begin(damageNames));
        };
        for (const auto prefix : { std::string_view("damage penetrates "), std::string_view("attack damage penetrates ") }) {
            if (!line.starts_with(prefix) || !ParseLeadingNumber(line.substr(prefix.size()), value, percent, tail) ||
                !percent || value < 0.0) continue;
            const bool attack = prefix.starts_with("attack ");
            if (tail == "elemental resistances" || tail == "of enemy elemental resistances") {
                if (attack) out.attackElementalPenetrationPct += value;
                else {
                    for (const auto type : { DamageType::Fire, DamageType::Cold, DamageType::Lightning }) {
                        out.penetrationPct[static_cast<std::size_t>(type)] += value;
                    }
                }
                return true;
            }
            if (!attack && tail.ends_with(" resistance")) {
                const auto type = indexOf(tail.substr(0, tail.size() - std::string_view(" resistance").size()));
                if (type > 0 && type < std::size(damageNames)) {
                    out.penetrationPct[type] += value;
                    return true;
                }
            }
        }
        for (const auto prefix : { std::string_view("gain "), std::string_view("attacks gain "), std::string_view("spells gain ") }) {
            if (!line.starts_with(prefix) || !ParseLeadingNumber(line.substr(prefix.size()), value, percent, tail) ||
                !percent || value < 0.0 || !tail.starts_with("of ")) continue;
            tail.remove_prefix(3);
            const auto split = tail.find(" as extra ");
            if (split == std::string_view::npos) continue;
            const auto source = tail.substr(0, split);
            const auto target = tail.substr(split + std::string_view(" as extra ").size());
            if (prefix == "gain " && source == "damage" && target == "damage of a random element") {
                out.extraDamage.randomElementGainPct += value;
                return true;
            }
            if (!target.ends_with(" damage")) continue;
            const auto to = indexOf(target.substr(0, target.size() - std::string_view(" damage").size()));
            if (to == std::size(damageNames)) continue;
            if (source == "damage" && prefix == "attacks gain ") {
                out.extraDamage.attackGain[to] += value;
                return true;
            }
            if (source == "damage" && prefix == "spells gain ") {
                out.extraDamage.spellGain[to] += value;
                return true;
            }
            if (prefix != "gain ") continue;
            if (source == "damage" || source == "elemental damage") {
                for (std::size_t from = 0; from < std::size(damageNames); ++from) {
                    if (source == "damage" || (from >= static_cast<std::size_t>(DamageType::Fire) &&
                        from <= static_cast<std::size_t>(DamageType::Lightning))) out.extraDamage.gain[from][to] += value;
                }
                return true;
            }
            if (source.ends_with(" damage")) {
                const auto from = indexOf(source.substr(0, source.size() - std::string_view(" damage").size()));
                if (from != std::size(damageNames)) {
                    out.extraDamage.gain[from][to] += value;
                    return true;
                }
            }
        }
        for (const auto& [prefix, kind] : {
            std::pair{ std::string_view("ignites you inflict deal damage "), AilmentKind::Ignite },
            { std::string_view("bleeding you inflict deals damage "), AilmentKind::Bleed }
        }) {
            if (line.starts_with(prefix) && ParseLeadingNumber(line.substr(prefix.size()), value, percent, tail) &&
                percent && tail == "faster") {
                out.ailments[static_cast<std::size_t>(kind)].damageFasterPct += value;
                return true;
            }
        }
        constexpr std::string_view damagingPrefix = "damaging ailments deal damage ";
        if (line.starts_with(damagingPrefix) && ParseLeadingNumber(line.substr(damagingPrefix.size()), value, percent, tail) &&
            percent && tail == "faster") {
            for (const auto kind : { AilmentKind::Ignite, AilmentKind::Bleed, AilmentKind::Poison }) {
                out.ailments[static_cast<std::size_t>(kind)].damageFasterPct += value;
            }
            return true;
        }

        // Shared vocabulary only where the current combat rules consume the result.
        PassiveStatTotals shared;
        if (!ParseLine(line, shared) || shared.spellSuppressionChance != 0.0 || shared.spellSuppressionPrevent != 0.0 ||
            shared.acrobatics || shared.ironWill || shared.painAttunement || shared.elementalOverload ||
            shared.freezeChancePct != 0.0 || shared.resoluteTechnique || shared.unwaveringStance || shared.cannotEvade ||
            shared.chaosInoculation || shared.bloodMagic || shared.mindOverMatterRatio != 0.0) {
            return false;
        }
        return ParseLine(line, out);
    }

    bool PassiveStatParser::IsPoe2Drawback(std::string_view statLine)
    {
        const auto normalized = ToLowerAscii(Trim(statLine));
        const std::string_view line = normalized;
        if (line.starts_with("lose ") || line.starts_with("you lose ") ||
            line.starts_with("you have no ") || line.starts_with("no inherent ") ||
            line.ends_with("are reflected back to you") ||
            line == "your aura buffs do not affect allies" ||
            line == "energy shield does not recharge") {
            return true;
        }

        double value = 0.0;
        bool percent = false;
        std::string_view tail;
        if (line.starts_with("you take ")) {
            return ParseLeadingNumber(line.substr(std::string_view("you take ").size()), value, percent, tail) &&
                value > 0.0 && percent && tail == "of damage from blocked hits";
        }
        if (!ParseLeadingNumber(line, value, percent, tail) || value <= 0.0 || !percent) return false;
        if (tail.starts_with("of damage taken bypasses energy shield") ||
            tail.starts_with("increased damage taken") ||
            tail.starts_with("increased penalty to accuracy rating")) {
            return true;
        }
        const bool reduced = tail.starts_with("reduced ");
        if (!reduced && !tail.starts_with("less ")) return false;
        const auto stat = tail.substr(reduced ? 8 : 5);
        return stat.starts_with("accuracy rating") || stat.starts_with("attack speed") ||
            stat.starts_with("cast speed") || stat.starts_with("mana regeneration rate") ||
            stat.starts_with("life regeneration rate") || stat.starts_with("presence area of effect") ||
               stat.starts_with("light radius") || stat == "knockback distance for blocked hits";
    }

    std::string_view PassiveStatParser::UnimplementedPoe2ManaRegenReplacement(std::span<const std::string> lines)
    {
        if (lines.size() < 2) return {};
        bool disablesNativeRegen = false;
        std::string_view replacement;
        for (const auto& line : lines) {
            const auto normalized = ToLowerAscii(Trim(line));
            if (normalized == "no inherent mana regeneration") disablesNativeRegen = true;
            if (normalized.starts_with("regenerate mana equal to ")) {
                PassiveStatTotals probe;
                if (!ParsePoe2Line(line, probe)) replacement = line;
            }
        }
        return disablesNativeRegen ? replacement : std::string_view{};
    }

    std::map<std::string, double> PassiveStatParser::Describe(const PassiveStatTotals& totals)
    {
        const PassiveStatTotals defaults;
        std::map<std::string, double> result;
#define MAP_STAT(name) if (totals.name != defaults.name) result.emplace(#name, totals.name)
        MAP_STAT(flatStr);
        MAP_STAT(flatDex);
        MAP_STAT(flatInt);
        MAP_STAT(incStrPct);
        MAP_STAT(incDexPct);
        MAP_STAT(incIntPct);
        MAP_STAT(flatLife);
        MAP_STAT(incLifePct);
        MAP_STAT(moreLifeMult);
        MAP_STAT(moreManaMult);
        MAP_STAT(flatMana);
        MAP_STAT(incManaPct);
        MAP_STAT(flatEnergyShield);
        MAP_STAT(incEnergyShieldPct);
        MAP_STAT(incEsRechargeRatePct);
        MAP_STAT(fasterEsRechargeStartPct);
        MAP_STAT(flatArmour);
        MAP_STAT(incArmourPct);
        MAP_STAT(flatEvasion);
        MAP_STAT(incEvasionPct);
        MAP_STAT(incArmourAndEvasionPct);
        MAP_STAT(fireResist);
        MAP_STAT(coldResist);
        MAP_STAT(lightningResist);
        MAP_STAT(chaosResist);
        MAP_STAT(maxFireResistBonus);
        MAP_STAT(maxColdResistBonus);
        MAP_STAT(maxLightningResistBonus);
        MAP_STAT(maxChaosResistBonus);
        MAP_STAT(physDamageReduction);
        MAP_STAT(attackBlockChance);
        MAP_STAT(attackBlockMaxBonusPct);
        MAP_STAT(spellBlockChance);
        MAP_STAT(spellBlockMaxBonusPct);
        MAP_STAT(spellSuppressionChance);
        MAP_STAT(spellSuppressionPrevent);
        MAP_STAT(incGlobalDamagePct);
        MAP_STAT(incPhysicalDamagePct);
        MAP_STAT(incMeleeDamagePct);
        MAP_STAT(incMeleePhysicalDamagePct);
        MAP_STAT(incAttackDamagePct);
        MAP_STAT(incPhysicalAttackDamagePct);
        MAP_STAT(incElementalDamagePct);
        MAP_STAT(incElementalAttackDamagePct);
        MAP_STAT(incFireDamagePct);
        MAP_STAT(incColdDamagePct);
        MAP_STAT(incLightningDamagePct);
        MAP_STAT(incChaosDamagePct);
        MAP_STAT(incSpellDamagePct);
        MAP_STAT(incAreaDamagePct);
        MAP_STAT(incAttackSpeedPct);
        MAP_STAT(incMeleeAttackSpeedPct);
        MAP_STAT(incCastSpeedPct);
        MAP_STAT(incGlobalCritChancePct);
        MAP_STAT(incAttackCritChancePct);
        MAP_STAT(incMeleeCritChancePct);
        MAP_STAT(incSpellCritChancePct);
        MAP_STAT(globalCritMultiplierPct);
        MAP_STAT(meleeCritMultiplierPct);
        MAP_STAT(spellCritMultiplierPct);
        MAP_STAT(incAreaOfEffectPct);
        MAP_STAT(reducedManaCostPct);
        MAP_STAT(costEfficiencyPct);
        MAP_STAT(attackCostEfficiencyPct);
        MAP_STAT(spellCostEfficiencyPct);
        MAP_STAT(manaCostEfficiencyPct);
        MAP_STAT(manaAttackCostEfficiencyPct);
        MAP_STAT(manaSpellCostEfficiencyPct);
        MAP_STAT(lifeCostEfficiencyPct);
        MAP_STAT(igniteChancePct);
        MAP_STAT(freezeChancePct);
        MAP_STAT(shockChancePct);
        MAP_STAT(bleedChancePct);
        MAP_STAT(poisonChancePct);
        MAP_STAT(attackIgniteChancePct);
        MAP_STAT(attackBleedChancePct);
        MAP_STAT(attackPoisonChancePct);
        MAP_STAT(spellPoisonChancePct);
        MAP_STAT(maxEnduranceBonus);
        MAP_STAT(maxFrenzyBonus);
        MAP_STAT(maxPowerBonus);
        MAP_STAT(bloodMagic);
        MAP_STAT(chaosInoculation);
        MAP_STAT(resoluteTechnique);
        MAP_STAT(ironReflexes);
        MAP_STAT(unwaveringStance);
        MAP_STAT(cannotEvade);
        MAP_STAT(acrobatics);
        MAP_STAT(mindOverMatterRatio);
        MAP_STAT(elementalOverload);
        MAP_STAT(painAttunement);
        MAP_STAT(ironWill);
        MAP_STAT(convertToFirePct);
        MAP_STAT(dealOnlyFire);
        MAP_STAT(convertFireSpellToChaosPct);
        MAP_STAT(chaosFireSpellIgniteMagnitude);
        MAP_STAT(fireSpellIgniteDealsChaos);
        MAP_STAT(chaosAndBleedImmune);
        MAP_STAT(moreOneHandEmptyOffHandSpeedPct);
        MAP_STAT(convertEsToManaPct);
        MAP_STAT(moreManaCostMult);
        MAP_STAT(unluckyEvade);
        MAP_STAT(luckyDeflect);
        MAP_STAT(manaRecoveryMult);
        MAP_STAT(fullLifeCritBonusMult);
        MAP_STAT(lowLifeCritBonusMult);
        MAP_STAT(excessLifeRegenToEs);
        MAP_STAT(esCannotRecharge);
        MAP_STAT(alwaysAggravateBleed);
        MAP_STAT(bleedBaseDuration);
        MAP_STAT(bleedMagnitudeMult);
        MAP_STAT(flatLifeRegen);
        MAP_STAT(percentLifeRegen);
        MAP_STAT(incLifeRegenPct);
        MAP_STAT(incLifeRecoveryPct);
        MAP_STAT(flatManaRegen);
        MAP_STAT(percentManaRegen);
        MAP_STAT(incManaRegenPct);
        MAP_STAT(incManaRecoveryPct);
        MAP_STAT(incBlockChancePct);
        MAP_STAT(flatDeflection);
        MAP_STAT(incDeflectionPct);
        MAP_STAT(evasionAsDeflectionPct);
        MAP_STAT(deflectReductionBonusPct);
        MAP_STAT(evasionAsExtraArmourPct);
        MAP_STAT(globalCritBonusIncPct);
        MAP_STAT(attackCritBonusIncPct);
        MAP_STAT(spellCritBonusIncPct);
        MAP_STAT(baseAttackCritPct);
        MAP_STAT(baseGlobalCritPct);
        MAP_STAT(attackAreaIncPct);
        MAP_STAT(spellAreaIncPct);
        MAP_STAT(attackAreaDamageIncPct);
        MAP_STAT(spellAreaDamageIncPct);
        MAP_STAT(projectileDamageIncPct);
        MAP_STAT(attackColdDamageIncPct);
        MAP_STAT(shieldDefencesIncPct);
        MAP_STAT(shieldAttackDamageIncPct);
        MAP_STAT(dualWieldAttackDamageIncPct);
        MAP_STAT(dualWieldAttackSpeedIncPct);
        MAP_STAT(meleeWeaponSpellDamageIncPct);
        MAP_STAT(lowLifeAttackDamageIncPct);
        MAP_STAT(lowLifeLifeRegenIncPct);
        MAP_STAT(lowLifePercentLifeRegen);
        MAP_STAT(lowLifeArmourAndEvasionIncPct);
        MAP_STAT(lowLifeCastSpeedIncPct);
        MAP_STAT(fullLifeAttackDamageIncPct);
        MAP_STAT(fullLifeCastSpeedIncPct);
        MAP_STAT(fullLifeEvasionIncPct);
        MAP_STAT(fullLifeManaRegenIncPct);
        MAP_STAT(movingAttackDamageIncPct);
        MAP_STAT(movingEvasionIncPct);
        MAP_STAT(movingDeflectionIncPct);
        MAP_STAT(movingLifeRegenIncPct);
        MAP_STAT(movingManaRegenIncPct);
        MAP_STAT(stationaryArmourIncPct);
        MAP_STAT(stationaryLifeRegenIncPct);
        MAP_STAT(stationaryManaRegenIncPct);
        MAP_STAT(stationaryPercentLifeRegen);
        MAP_STAT(hitDamageIncAgainstElementalAilmentsPct);
        MAP_STAT(hitDamageIncAgainstAnyAilmentsPct);
        MAP_STAT(hitDamageIncAgainstIgnitedPct);
        MAP_STAT(hitDamageIncAgainstBurningPct);
        MAP_STAT(hitDamageIncAgainstFrozenPct);
        MAP_STAT(reducedIncomingCritBonusPct);
        MAP_STAT(reducedIncomingIgniteMagnitudePct);
        MAP_STAT(reducedIncomingShockMagnitudePct);
        MAP_STAT(deflectReductionCritBonusPct);
        MAP_STAT(deflectPreventsBleed);
        MAP_STAT(cooldownRecoveryIncPct);
        MAP_STAT(skillDurationIncPct);
        MAP_STAT(triggeredSpellDamageIncPct);
        MAP_STAT(channellingDamageIncPct);
        MAP_STAT(channellingSpeedIncPct);
        MAP_STAT(flatSpirit);
        MAP_STAT(incSpiritPct);
        MAP_STAT(incMovementSpeedPct);
        MAP_STAT(incSprintingMovementSpeedPct);
        MAP_STAT(incDualWieldMovementSpeedPct);
        MAP_STAT(incCharmMovementSpeedPct);
        MAP_STAT(incEsMovementSpeedPct);
        MAP_STAT(attackElementalPenetrationPct);
        MAP_STAT(manaAsLifeCostPct);
        MAP_STAT(spellManaAsLifeCostPct);
        MAP_STAT(incLifeCostPct);
        MAP_STAT(matchingBaseLifeCost);
        MAP_STAT(lifeCostSpellDamageIncPct);
        MAP_STAT(projectilePierceChancePct);
        MAP_STAT(additionalProjectileChancePct);
        MAP_STAT(additionalChainChancePct);
        MAP_STAT(additionalForkProjectileChancePct);
        MAP_STAT(additionalProjectiles);
        MAP_STAT(projectileSpeedIncPct);
        MAP_STAT(spellProjectileSpeedIncPct);
        MAP_STAT(projectileAilmentChanceIncPct);
        MAP_STAT(auraMagnitudeIncPct);
        MAP_STAT(heraldReservationEfficiencyPct);
        MAP_STAT(elementalSkillSpeedIncPct);
        MAP_STAT(lightningSkillSpeedIncPct);
        MAP_STAT(coldCastSpeedIncPct);
        MAP_STAT(lifeRecoveryOnKillPct);
        MAP_STAT(manaRecoveryOnKillPct);
        MAP_STAT(lifeOnKill);
        MAP_STAT(armourAsDeflectionPct);
        MAP_STAT(noInherentManaRegen);
        MAP_STAT(noSpirit);
        MAP_STAT(bodyArmourIncPct);
        MAP_STAT(bodyEvasionIncPct);
        MAP_STAT(bodyEsIncPct);
        MAP_STAT(helmetEsIncPct);
        MAP_STAT(armourFromStrRequirementsPct);
        MAP_STAT(baseBlockCapOverridePct);
        MAP_STAT(blockedHitDamageTakenPct);
        MAP_STAT(energyShieldBypassPct);
        MAP_STAT(maximumEvadePct);
        MAP_STAT(maximumPhysicalReductionPct);
        MAP_STAT(addedAttackLightningMin);
        MAP_STAT(addedAttackLightningMax);
        MAP_STAT(incFreezeBuildupPct);
        MAP_STAT(flammabilityIncPct);
        MAP_STAT(elementalAilmentThresholdIncPct);
        MAP_STAT(esAsAilmentThresholdPct);
        MAP_STAT(freezeThresholdIncPct);
        MAP_STAT(esAsFreezeThresholdPct);
        MAP_STAT(incAilmentChancePct);
        MAP_STAT(incAilmentMagnitudePct);
        MAP_STAT(reducedIncomingAilmentDurationPct);
#undef MAP_STAT
        constexpr std::string_view types[]{ "physical", "fire", "cold", "lightning", "chaos" };
        for (std::size_t i = 0; i < totals.armourAppliesPct.size(); ++i) {
            if (totals.armourAppliesPct[i] != 0.0) result.emplace(std::string("armourAppliesPct.") + types[i].data(), totals.armourAppliesPct[i]);
            if (totals.penetrationPct[i] != 0.0) result.emplace(std::string("penetrationPct.") + types[i].data(), totals.penetrationPct[i]);
            if (totals.extraDamage.attackGain[i] != 0.0) result.emplace(std::string("attackGainPct.") + types[i].data(), totals.extraDamage.attackGain[i]);
            if (totals.extraDamage.spellGain[i] != 0.0) result.emplace(std::string("spellGainPct.") + types[i].data(), totals.extraDamage.spellGain[i]);
            for (std::size_t j = 0; j < totals.extraDamage.gain[i].size(); ++j) {
                const double gain = totals.extraDamage.gain[i][j];
                if (gain != 0.0) result.emplace(std::string("gainPct.") + types[i].data() + "." + types[j].data(), gain);
            }
        }
        for (std::size_t i = 1; i < totals.additionalSpellProjectilesChancePct.size(); ++i) {
            const auto chance = totals.additionalSpellProjectilesChancePct[i];
            if (chance != 0.0) result.emplace("additionalSpellProjectilesChancePct." + std::to_string(i), chance);
        }
        if (totals.extraDamage.randomElementGainPct != 0.0)
            result.emplace("randomElementGainPct", totals.extraDamage.randomElementGainPct);
        if (totals.extraDamage.coldAsFireAgainstFrozenPct != 0.0)
            result.emplace("coldAsFireAgainstFrozenPct", totals.extraDamage.coldAsFireAgainstFrozenPct);
        if (totals.extraDamage.physicalAsColdAgainstShockedPct != 0.0)
            result.emplace("physicalAsColdAgainstShockedPct", totals.extraDamage.physicalAsColdAgainstShockedPct);
        if (totals.extraDamage.physicalAsLightningAgainstChilledPct != 0.0)
            result.emplace("physicalAsLightningAgainstChilledPct", totals.extraDamage.physicalAsLightningAgainstChilledPct);
        constexpr std::string_view ailments[]{ "ignite", "chill", "freeze", "shock", "bleed", "poison" };
        for (std::size_t i = 0; i < totals.ailments.size(); ++i) {
            const auto& stats = totals.ailments[i];
            const std::string prefix = std::string(ailments[i]) + ".";
            if (stats.chanceIncPct != 0.0) result.emplace(prefix + "chanceIncPct", stats.chanceIncPct);
            if (stats.magnitudeIncPct != 0.0) result.emplace(prefix + "magnitudeIncPct", stats.magnitudeIncPct);
            if (stats.durationIncPct != 0.0) result.emplace(prefix + "durationIncPct", stats.durationIncPct);
            if (stats.damageFasterPct != 0.0) result.emplace(prefix + "damageFasterPct", stats.damageFasterPct);
        }
        constexpr std::string_view charges[]{ "endurance", "frenzy", "power" };
        for (std::size_t i = 0; i < totals.chargeGain.size(); ++i) {
            if (totals.chargeGain[i] != defaults.chargeGain[i]) {
                result.emplace(std::string("chargeGain.") + charges[i].data(), static_cast<double>(totals.chargeGain[i]));
            }
        }
        constexpr std::string_view weapons[]{ "oneHand", "twoHand", "sword", "axe", "mace", "dagger" };
        for (std::size_t i = 0; i < totals.weapons.size(); ++i) {
            const auto& stats = totals.weapons[i];
            const std::string prefix = std::string(weapons[i]) + ".";
            if (stats.damageIncPct != 0.0) result.emplace(prefix + "damageIncPct", stats.damageIncPct);
            if (stats.speedIncPct != 0.0) result.emplace(prefix + "speedIncPct", stats.speedIncPct);
            if (stats.critChanceIncPct != 0.0) result.emplace(prefix + "critChanceIncPct", stats.critChanceIncPct);
            if (stats.critBonusIncPct != 0.0) result.emplace(prefix + "critBonusIncPct", stats.critBonusIncPct);
        }
        return result;
    }

    PassiveStatTotals PassiveStatParser::AggregateTree(
        const PassiveTreeData& data,
        const PassiveTreeState& state)
    {
        PassiveStatTotals totals;
        if (!data.IsLoaded() || !state.classSelected) {
            return totals;
        }

        totals.poe2 = data.Generation() == 1;
        auto processStats = [&](std::span<const std::string> lines, bool keystone = false) {
            if (totals.poe2 && (!UnimplementedPoe2ManaRegenReplacement(lines).empty() || std::ranges::any_of(lines, [&](const auto& line) {
                if (!keystone && !IsPoe2Drawback(line)) return false;
                PassiveStatTotals probe;
                return !ParsePoe2Line(line, probe, keystone);
            }))) {
                totals.unparsedStatLines += lines.size();
                return;
            }
            for (const auto& line : lines) {
                if (totals.poe2 ? ParsePoe2Line(line, totals, keystone) : ParseLine(line, totals)) {
                    ++totals.parsedStatLines;
                } else {
                    ++totals.unparsedStatLines;
                }
            }
        };

        for (const auto nid : state.allocatedNodes) {
            const auto* node = data.FindNode(nid);
            if (!node) continue;
            if (totals.poe2) {
                processStats(PassiveTree::ResolveNode(*node, state).stats, node->kind == PassiveNodeKind::Keystone);
            } else {
                processStats(node->stats);
            }
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
