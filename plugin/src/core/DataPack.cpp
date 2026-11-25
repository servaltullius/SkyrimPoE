#include "core/DataPack.h"
#include "core/GemForms.h"
#include "core/RulesPack.h"

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
            if (s == "aura") return GemKind::Aura;
            if (s == "herald") return GemKind::Herald;
            if (s == "guard") return GemKind::Guard;
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
            if (const auto cost = j.find("spiritCost"); cost != j.end()) {
                d.spiritCost = cost->get<std::uint32_t>();
            }
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
                def.spiritCost = g.value("spiritCost", 0u);
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
        const auto& tables = ActiveRuleValues().levelTables;
        return tables.At(tables.life, level);
    }

    double DataPack::MonsterDamage(std::uint32_t level) const noexcept
    {
        const auto& tables = ActiveRuleValues().levelTables;
        return tables.At(tables.damage, level);
    }

    double DataPack::MonsterAccuracy(std::uint32_t level) const noexcept
    {
        const auto& tables = ActiveRuleValues().levelTables;
        return tables.At(tables.accuracy, level);
    }

    double DataPack::MapLevelLifeMult(std::uint32_t level) const noexcept
    {
        const auto& tables = ActiveRuleValues().levelTables;
        return tables.At(tables.mapLevelLifeMult, level);
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

    bool DataPack::LoadGen1GemsFromFile(const std::filesystem::path& path, std::string* errorOut)
    {
        try {
            std::ifstream file(path, std::ios::binary);
            if (!file) throw std::runtime_error("Could not open generation-one gem catalog");
            const auto bytes = std::string(std::istreambuf_iterator<char>(file), {});
            return LoadGen1GemsFromJsonString(bytes, errorOut);
        } catch (const std::exception& e) {
            if (errorOut) *errorOut = e.what();
            return false;
        }
    }

    bool DataPack::LoadGen1GemsFromJsonString(std::string_view jsonText, std::string* errorOut)
    {
        try {
            const auto root = nlohmann::json::parse(jsonText);
            const auto& meta = root.at("meta");
            const auto& gems = root.at("gems");
            const auto& schemaVersion = meta.at("schemaVersion");
            const auto& generation = meta.at("contentGeneration");
            if (meta.at("schema") != "skyrimpoe.gems" || !schemaVersion.is_number_integer() || schemaVersion != 1 ||
                meta.at("game") != "poe2" || !generation.is_number_integer() || generation != 1 ||
                !gems.is_object() || gems.size() != GemForms::kContentKeys.size()) {
                throw std::runtime_error("Invalid generation-one gem catalog");
            }

            std::unordered_map<std::string, SkillEffectDefinition> nextGems;
            for (const auto& [id, gem] : gems.items()) {
                const auto* key = GemForms::FindContent(id);
                if (!key || gem.at("contentId") != id || gem.at("ready") != key->ready ||
                    gem.at("shape") != "S" + std::to_string(static_cast<int>(key->execution) + 1)) {
                    throw std::runtime_error("Gem catalog and form table disagree: " + id);
                }

                // The first effect and stat set are the base skill. Conditional sets are separate.
                const auto& effect = gem.at("effects").at(0);
                const auto& base = effect.at("statSets").at(0);
                SkillEffectDefinition def;
                def.id = id;
                def.name = gem.at("nameKo").get<std::string>();
                def.color = effect.value("color", std::uint8_t{});
                def.castTime = effect.value("castTime", 0.0);
                def.isSupport = key->execution == GemForms::GemExecution::S11;
                const auto type = gem.at("gemType").get<std::string>();
                if (def.isSupport) def.kind = GemKind::Support;
                else if (type == "Spell") def.kind = GemKind::Spell;
                else if (key->execution == GemForms::GemExecution::S12) def.kind = GemKind::AttackChannel;
                else if (key->execution == GemForms::GemExecution::S8) def.kind = GemKind::Aura;
                else if (key->execution == GemForms::GemExecution::S9) def.kind = GemKind::Herald;
                else if (key->execution == GemForms::GemExecution::S10) def.kind = GemKind::Trigger;
                // 실행 종류가 아직 정해지지 않은 젬은 준비 상태가 될 수 없다(장착 판정이 Unknown을 보조 젬처럼 거절함).
                if (key->ready && def.kind == GemKind::Unknown) {
                    throw std::runtime_error("Ready gem has no runtime kind: " + id);
                }
                def.skillTypes = effect.at("skillTypes").get<std::unordered_set<std::string>>();
                def.requireSkillTypes = effect.value("requireSkillTypes", std::vector<std::string>{});
                def.excludeSkillTypes = effect.value("excludeSkillTypes", std::vector<std::string>{});
                def.addSkillTypes = effect.value("addSkillTypes", std::vector<std::string>{});
                def.weaponTypes = effect.value("weaponTypes", std::unordered_set<std::string>{});

                if (const auto constants = base.find("constantStats"); constants != base.end()) {
                    for (const auto& stat : *constants) {
                        def.constantStats.emplace(stat.at(0).get<std::string>(), stat.at(1).get<double>());
                    }
                }
                const auto& statNames = base.at("stats");
                for (const auto& [level, row] : effect.at("levels").items()) {
                    auto values = ParseLevelData(row);
                    const auto numericLevel = std::stoi(level);
                    if (numericLevel < 1 || numericLevel > 255 || level != std::to_string(numericLevel)) {
                        throw std::runtime_error("Invalid gem level: " + id);
                    }
                    values.level = static_cast<std::uint8_t>(numericLevel);
                    const auto& requirements = row.at("requirements");
                    values.reqStr = requirements.at("reqStr").get<std::uint32_t>();
                    values.reqDex = requirements.at("reqDex").get<std::uint32_t>();
                    values.reqInt = requirements.at("reqInt").get<std::uint32_t>();
                    if (const auto cost = row.find("cost"); cost != row.end()) {
                        values.manaCost = cost->value("Mana", 0.0);
                    }
                    if (key->ready && def.kind == GemKind::Spell && !(values.manaCost > 0.0)) {
                        throw std::runtime_error("Ready spell has no mana cost at level " + level + ": " + id);
                    }
                    if (!def.isSupport && row.contains("spiritReservationFlat")) {
                        values.spiritCost = row.at("spiritReservationFlat").get<std::uint32_t>();
                    }
                    const auto& statLevels = base.at("levels");
                    if (key->ready && !statLevels.contains(level)) {
                        throw std::runtime_error("Ready gem has no base stat row at level " + level + ": " + id);
                    }
                    if (const auto statRow = statLevels.find(level); statRow != statLevels.end()) {
                        const auto& stats = statRow->at("values");
                        if (stats.size() > statNames.size()) {
                            throw std::runtime_error("Too many base stats: " + id);
                        }
                        for (std::size_t i = 0; i < stats.size(); ++i) {
                            if (stats.at(i).is_number()) {
                                values.stats.emplace(statNames.at(i).get<std::string>(), stats.at(i).get<double>());
                            }
                        }
                    }
                    def.levels.emplace(values.level, std::move(values));
                }
                if (!def.levels.contains(1)) throw std::runtime_error("Missing level one: " + id);
                nextGems.emplace(id, std::move(def));
            }
            gen1Gems_ = std::move(nextGems);
            return true;
        } catch (const std::exception& e) {
            if (errorOut) *errorOut = e.what();
            return false;
        }
    }

    const SkillEffectDefinition* DataPack::FindGem(const std::string& gemId, std::uint32_t generation) const
    {
        if (generation > 1) return nullptr;
        const auto& catalog = generation == 0 ? gems_ : gen1Gems_;
        const auto it = catalog.find(gemId);
        return it == catalog.end() ? nullptr : &it->second;
    }

    bool DataPack::CanSupportApply(
        const std::string& supportGemId,
        const std::string& activeGemId,
        const std::unordered_set<std::string>* extraSkillTypes, std::uint32_t generation) const
    {
        const auto* sup = FindGem(supportGemId, generation);
        const auto* act = FindGem(activeGemId, generation);
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
        const std::string& attackGemId, std::uint32_t generation) const
    {
        const auto* trig = FindGem(triggerGemId, generation);
        const auto* atk = FindGem(attackGemId, generation);
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
        const std::string& spellGemId, std::uint32_t generation) const
    {
        const auto* trig = FindGem(triggerGemId, generation);
        const auto* spell = FindGem(spellGemId, generation);
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
