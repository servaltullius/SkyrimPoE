#include "core/DataPack.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace SkyrimPoE::Core
{
    namespace
    {
        GemKind ParseGemKind(const std::string& s)
        {
            if (s == "attack_channel") return GemKind::AttackChannel;
            if (s == "trigger") return GemKind::Trigger;
            if (s == "spell") return GemKind::Spell;
            if (s == "support") return GemKind::Support;
            return GemKind::Unknown;
        }

        GemLevelData ParseLevelData(const nlohmann::json& j)
        {
            GemLevelData d;
            d.level = static_cast<std::uint8_t>(j.value("level", 1));
            d.levelRequirement = static_cast<std::uint8_t>(j.value("levelRequirement", 1));
            d.reqStr = j.value("reqStr", 0u);
            d.reqDex = j.value("reqDex", 0u);
            d.reqInt = j.value("reqInt", 0u);
            d.manaCost = j.value("manaCost", 0.0);
            d.manaMultiplier = j.value("manaMultiplier", 0.0);
            d.attackSpeedMultiplier = j.value("attackSpeedMultiplier", 0.0);
            d.baseMultiplier = j.value("baseMultiplier", 1.0);
            d.damageEffectiveness = j.value("damageEffectiveness", 1.0);
            d.critChance = j.value("critChance", 0.0);
            d.cooldown = j.value("cooldown", 0.0);
            if (j.contains("stats") && j["stats"].is_object()) {
                for (const auto& [k, v] : j["stats"].items()) {
                    if (v.is_number()) {
                        d.stats[k] = v.get<double>();
                    }
                }
            }
            return d;
        }

        std::size_t ClampLevelIndex(std::uint32_t level, std::size_t size) noexcept
        {
            if (size == 0) {
                return 0;
            }
            if (level <= 1) {
                return 0;
            }
            const auto idx = static_cast<std::size_t>(level - 1);
            return std::min(idx, size - 1);
        }
    }

    double GemLevelData::GetStat(const std::string& key, double fallback) const
    {
        const auto it = stats.find(key);
        return it == stats.end() ? fallback : it->second;
    }

    const GemLevelData* SkillEffectDefinition::GetLevel(std::uint8_t lv) const
    {
        if (levels.empty()) {
            return nullptr;
        }
        const auto clamped = std::clamp<std::uint8_t>(lv, 1, static_cast<std::uint8_t>(kMaxGemLevel));
        if (const auto it = levels.find(clamped); it != levels.end()) {
            return &it->second;
        }
        // 결정적 폴백: 요청 이하 최고 레벨, 없으면 가장 낮은 레벨
        const GemLevelData* bestBelow = nullptr;
        const GemLevelData* lowest = nullptr;
        for (const auto& [k, v] : levels) {
            if (!lowest || k < lowest->level) {
                lowest = &v;
            }
            if (k <= clamped && (!bestBelow || k > bestBelow->level)) {
                bestBelow = &v;
            }
        }
        return bestBelow ? bestBelow : lowest;
    }

    const GemLevelData* SkillEffectDefinition::GetTriggeredLevel(std::uint8_t lv) const
    {
        if (triggeredLevels.empty()) {
            return nullptr;
        }
        const auto clamped = std::clamp<std::uint8_t>(lv, 1, static_cast<std::uint8_t>(kMaxGemLevel));
        if (const auto it = triggeredLevels.find(clamped); it != triggeredLevels.end()) {
            return &it->second;
        }
        const GemLevelData* bestBelow = nullptr;
        const GemLevelData* lowest = nullptr;
        for (const auto& [k, v] : triggeredLevels) {
            if (!lowest || k < lowest->level) {
                lowest = &v;
            }
            if (k <= clamped && (!bestBelow || k > bestBelow->level)) {
                bestBelow = &v;
            }
        }
        return bestBelow ? bestBelow : lowest;
    }

    bool DoesTypeExpressionMatch(
        const std::vector<std::string>& checkTypes,
        const std::unordered_set<std::string>& skillTypes)
    {
        if (checkTypes.empty()) {
            return true;
        }
        std::vector<bool> stack;
        stack.reserve(checkTypes.size());
        for (const auto& token : checkTypes) {
            if (token == "OR") {
                if (stack.size() >= 2) {
                    const bool rhs = stack.back();
                    stack.pop_back();
                    stack.back() = stack.back() || rhs;
                }
            } else if (token == "AND") {
                if (stack.size() >= 2) {
                    const bool rhs = stack.back();
                    stack.pop_back();
                    stack.back() = stack.back() && rhs;
                }
            } else if (token == "NOT") {
                if (!stack.empty()) {
                    stack.back() = !stack.back();
                }
            } else {
                stack.push_back(skillTypes.contains(token));
            }
        }
        for (const bool val : stack) {
            if (val) {
                return true;
            }
        }
        return false;
    }

    bool DataPack::LoadFromFile(const std::filesystem::path& path, std::string* errorOut)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            if (errorOut) {
                *errorOut = "Could not open data pack file: " + path.string();
            }
            return false;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        return LoadFromJsonString(oss.str(), errorOut);
    }

    bool DataPack::LoadFromJsonString(std::string_view jsonText, std::string* errorOut)
    {
        try {
            const auto root = nlohmann::json::parse(jsonText.begin(), jsonText.end());
            const auto& c = root.at("constants");
            PoEConstants nextConst;
            nextConst.lifeBase = c.at("lifeBase").get<double>();
            nextConst.lifePerLevel = c.at("lifePerLevel").get<double>();
            nextConst.manaBase = c.at("manaBase").get<double>();
            nextConst.manaPerLevel = c.at("manaPerLevel").get<double>();
            nextConst.resistCap = c.at("resistCap").get<double>();
            nextConst.esRechargeDelay = c.at("esRechargeDelay").get<double>();
            nextConst.esRechargeRate = c.at("esRechargeRate").get<double>();
            nextConst.monsterDamageMult = c.value("monsterDamageMult", 1.5);
            nextConst.armourMitigationCap = c.value("armourMitigationCap", 0.90);
            nextConst.minHitChance = c.value("minHitChance", 0.05);

            const auto& t = root.at("tables");
            auto nextLife = t.at("monsterLife").get<std::vector<double>>();
            auto nextDamage = t.at("monsterDamage").get<std::vector<double>>();
            auto nextAccuracy = t.at("monsterAccuracy").get<std::vector<double>>();
            if (nextLife.size() < 100 || nextDamage.size() < 100 || nextAccuracy.size() < 100) {
                if (errorOut) *errorOut = "Monster tables must contain at least 100 entries.";
                return false;
            }

            std::vector<ExpectedDefenseRow> nextDef;
            for (const auto& row : t.at("expectedDefense")) {
                ExpectedDefenseRow r;
                r.level = row.at("level").get<std::uint32_t>();
                r.bestArmour = row.at("bestArmour").get<double>();
                r.bestEvasion = row.at("bestEvasion").get<double>();
                r.bestEnergyShield = row.at("bestEnergyShield").get<double>();
                r.bestResistTier = row.at("bestResistTier").get<double>();
                nextDef.push_back(r);
            }
            if (nextDef.size() < 100) {
                if (errorOut) *errorOut = "expectedDefense table must contain at least 100 entries.";
                return false;
            }

            std::unordered_map<std::string, ClassBase> nextClasses;
            for (const auto& cls : root.at("classes")) {
                ClassBase cb;
                cb.name = cls.at("name").get<std::string>();
                cb.baseStr = cls.at("baseStr").get<std::uint32_t>();
                cb.baseDex = cls.at("baseDex").get<std::uint32_t>();
                cb.baseInt = cls.at("baseInt").get<std::uint32_t>();
                nextClasses[cb.name] = std::move(cb);
            }

            std::unordered_map<std::string, SkillEffectDefinition> nextGems;
            for (const auto& [gid, g] : root.at("gems").items()) {
                SkillEffectDefinition def;
                def.id = g.value("id", gid);
                def.name = g.value("name", gid);
                def.color = static_cast<std::uint8_t>(g.value("color", 0));
                def.isSupport = g.value("support", false);
                def.kind = ParseGemKind(g.value("kind", ""));
                def.castTime = g.value("castTime", 0.0);
                if (g.contains("skillTypes")) {
                    for (const auto& st : g["skillTypes"]) def.skillTypes.insert(st.get<std::string>());
                }
                if (g.contains("requireSkillTypes")) {
                    def.requireSkillTypes = g["requireSkillTypes"].get<std::vector<std::string>>();
                }
                if (g.contains("excludeSkillTypes")) {
                    def.excludeSkillTypes = g["excludeSkillTypes"].get<std::vector<std::string>>();
                }
                if (g.contains("addSkillTypes")) {
                    def.addSkillTypes = g["addSkillTypes"].get<std::vector<std::string>>();
                }
                if (g.contains("weaponTypes")) {
                    for (const auto& wt : g["weaponTypes"]) def.weaponTypes.insert(wt.get<std::string>());
                }
                if (g.contains("constantStats") && g["constantStats"].is_object()) {
                    for (const auto& [sk, sv] : g["constantStats"].items()) {
                        if (sv.is_number()) def.constantStats[sk] = sv.get<double>();
                    }
                }
                if (g.contains("levels") && g["levels"].is_object()) {
                    for (const auto& [lk, lv] : g["levels"].items()) {
                        auto ld = ParseLevelData(lv);
                        def.levels[ld.level] = std::move(ld);
                    }
                }
                if (g.contains("triggeredEffect") && g["triggeredEffect"].is_object()) {
                    const auto& te = g["triggeredEffect"];
                    if (te.contains("requireSkillTypes")) {
                        def.triggeredRequireSkillTypes = te["requireSkillTypes"].get<std::vector<std::string>>();
                    }
                    if (te.contains("excludeSkillTypes")) {
                        def.triggeredExcludeSkillTypes = te["excludeSkillTypes"].get<std::vector<std::string>>();
                    }
                    if (te.contains("addSkillTypes")) {
                        def.triggeredAddSkillTypes = te["addSkillTypes"].get<std::vector<std::string>>();
                    }
                    if (te.contains("levels") && te["levels"].is_object()) {
                        for (const auto& [lk, lv] : te["levels"].items()) {
                            auto ld = ParseLevelData(lv);
                            def.triggeredLevels[ld.level] = std::move(ld);
                        }
                    }
                }
                nextGems[def.id] = std::move(def);
            }

            constants_ = nextConst;
            monsterLife_ = std::move(nextLife);
            monsterDamage_ = std::move(nextDamage);
            monsterAccuracy_ = std::move(nextAccuracy);
            expectedDefense_ = std::move(nextDef);
            classes_ = std::move(nextClasses);
            gems_ = std::move(nextGems);
            loaded_ = true;
            return true;
        } catch (const std::exception& e) {
            if (errorOut) {
                *errorOut = e.what();
            }
            return false;
        }
    }

    double DataPack::MonsterLife(std::uint32_t level) const noexcept
    {
        if (monsterLife_.empty()) return 22.0;
        return monsterLife_[ClampLevelIndex(level, monsterLife_.size())];
    }

    double DataPack::MonsterDamage(std::uint32_t level) const noexcept
    {
        if (monsterDamage_.empty()) return 4.99;
        return monsterDamage_[ClampLevelIndex(level, monsterDamage_.size())];
    }

    double DataPack::MonsterAccuracy(std::uint32_t level) const noexcept
    {
        if (monsterAccuracy_.empty()) return 14.0;
        return monsterAccuracy_[ClampLevelIndex(level, monsterAccuracy_.size())];
    }

    const ExpectedDefenseRow& DataPack::ExpectedDefense(std::uint32_t level) const noexcept
    {
        static const ExpectedDefenseRow kFallback{};
        if (expectedDefense_.empty()) return kFallback;
        return expectedDefense_[ClampLevelIndex(level, expectedDefense_.size())];
    }

    const ClassBase* DataPack::FindClass(const std::string& name) const
    {
        const auto it = classes_.find(name);
        return it == classes_.end() ? nullptr : &it->second;
    }

    const SkillEffectDefinition* DataPack::FindGem(const std::string& gemId) const
    {
        const auto it = gems_.find(gemId);
        return it == gems_.end() ? nullptr : &it->second;
    }

    bool DataPack::CanSupportApply(
        const std::string& supportGemId,
        const std::string& activeGemId,
        const std::unordered_set<std::string>* extraSkillTypes) const
    {
        const auto* sup = FindGem(supportGemId);
        const auto* act = FindGem(activeGemId);
        if (!sup || !act || !sup->isSupport || sup->kind == GemKind::Trigger || act->isSupport) {
            return false;
        }
        auto types = act->skillTypes;
        if (extraSkillTypes) {
            types.insert(extraSkillTypes->begin(), extraSkillTypes->end());
        }
        if (!sup->requireSkillTypes.empty() && !DoesTypeExpressionMatch(sup->requireSkillTypes, types)) {
            return false;
        }
        if (!sup->excludeSkillTypes.empty() && DoesTypeExpressionMatch(sup->excludeSkillTypes, types)) {
            return false;
        }
        return true;
    }

    bool DataPack::CanTriggerLinkAttack(
        const std::string& triggerGemId,
        const std::string& attackGemId) const
    {
        const auto* trig = FindGem(triggerGemId);
        const auto* atk = FindGem(attackGemId);
        if (!trig || !atk || trig->kind != GemKind::Trigger || atk->isSupport) {
            return false;
        }
        if (!trig->requireSkillTypes.empty() && !DoesTypeExpressionMatch(trig->requireSkillTypes, atk->skillTypes)) {
            return false;
        }
        if (!trig->excludeSkillTypes.empty() && DoesTypeExpressionMatch(trig->excludeSkillTypes, atk->skillTypes)) {
            return false;
        }
        return true;
    }

    bool DataPack::CanTriggerSocketSpell(
        const std::string& triggerGemId,
        const std::string& spellGemId) const
    {
        const auto* trig = FindGem(triggerGemId);
        const auto* spell = FindGem(spellGemId);
        if (!trig || !spell || trig->kind != GemKind::Trigger || spell->kind != GemKind::Spell) {
            return false;
        }
        if (trig->triggeredRequireSkillTypes && !trig->triggeredRequireSkillTypes->empty()) {
            if (!DoesTypeExpressionMatch(*trig->triggeredRequireSkillTypes, spell->skillTypes)) {
                return false;
            }
        }
        if (trig->triggeredExcludeSkillTypes && !trig->triggeredExcludeSkillTypes->empty()) {
            if (DoesTypeExpressionMatch(*trig->triggeredExcludeSkillTypes, spell->skillTypes)) {
                return false;
            }
        }
        return true;
    }
}
