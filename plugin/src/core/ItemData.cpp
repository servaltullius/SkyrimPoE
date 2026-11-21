#include "core/ItemData.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace SkyrimPoE::Core
{
    namespace
    {
        CurrencyKind ParseCurrencyKind(std::string_view id) noexcept
        {
            if (id == "ScrollOfWisdom") return CurrencyKind::ScrollOfWisdom;
            if (id == "Transmutation")  return CurrencyKind::Transmutation;
            if (id == "Augmentation")   return CurrencyKind::Augmentation;
            if (id == "Alteration")     return CurrencyKind::Alteration;
            if (id == "Regal")          return CurrencyKind::Regal;
            if (id == "Alchemy")        return CurrencyKind::Alchemy;
            if (id == "Chaos")          return CurrencyKind::Chaos;
            if (id == "Exalted")        return CurrencyKind::Exalted;
            if (id == "Scouring")       return CurrencyKind::Scouring;
            if (id == "Divine")         return CurrencyKind::Divine;
            if (id == "Regret")         return CurrencyKind::Regret;
            if (id == "Jeweller")       return CurrencyKind::Jeweller;
            return CurrencyKind::Count;
        }

        const char* ArmorSlotCurvePrefix(TokenSlotGroup slotGroup) noexcept
        {
            switch (slotGroup) {
                case TokenSlotGroup::Helmet: return "Helmet";
                case TokenSlotGroup::Body:   return "Body";
                case TokenSlotGroup::Gloves: return "Gloves";
                case TokenSlotGroup::Boots:  return "Boots";
                case TokenSlotGroup::Shield: return "Shield";
                default:                     return "";
            }
        }

        const char* DefenseSubtypeKey(DefenseSubtype sub) noexcept
        {
            switch (sub) {
                case DefenseSubtype::Armour:       return "Armour";
                case DefenseSubtype::Evasion:      return "Evasion";
                case DefenseSubtype::EnergyShield: return "EnergyShield";
                default:                           return "Armour";
            }
        }

        MonsterModKind ParseMonsterModKind(std::string_view id) noexcept
        {
            if (id == "Hasted")             return MonsterModKind::Hasted;
            if (id == "Armoured")           return MonsterModKind::Armoured;
            if (id == "FireResistant")      return MonsterModKind::FireResistant;
            if (id == "ColdResistant")      return MonsterModKind::ColdResistant;
            if (id == "LightningResistant") return MonsterModKind::LightningResistant;
            if (id == "ChaosResistant")     return MonsterModKind::ChaosResistant;
            if (id == "EnergyShielded")     return MonsterModKind::EnergyShielded;
            if (id == "ExtraPhysical")      return MonsterModKind::ExtraPhysical;
            if (id == "ExtraFire")          return MonsterModKind::ExtraFire;
            if (id == "ExtraCold")          return MonsterModKind::ExtraCold;
            if (id == "ExtraLightning")     return MonsterModKind::ExtraLightning;
            if (id == "Deadly")             return MonsterModKind::Deadly;
            return MonsterModKind::Count;
        }
    }

    bool ItemData::LoadFromFile(const std::filesystem::path& path, std::string* errorOut)
    {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) {
            if (errorOut) {
                *errorOut = "아이템 데이터 팩 파일을 열 수 없습니다: " + path.string();
            }
            return false;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        return LoadFromJsonString(oss.str(), errorOut);
    }

    bool ItemData::LoadFromJsonString(std::string_view jsonText, std::string* errorOut)
    {
        try {
            const auto root = nlohmann::json::parse(jsonText.begin(), jsonText.end());
            if (!root.is_object() || !root.contains("affixes") || !root["affixes"].is_array()) {
                if (errorOut) {
                    *errorOut = "items_3_30.json에 affixes 배열이 없습니다.";
                }
                return false;
            }

            std::vector<AffixDefinition> loadedAffixes;
            std::unordered_map<std::string, std::size_t> loadedById;
            const auto& arr = root["affixes"];
            loadedAffixes.reserve(arr.size());

            for (const auto& j : arr) {
                AffixDefinition def;
                def.id = j.value("id", "");
                if (def.id.empty()) {
                    continue;
                }
                const std::string t = j.value("type", "Prefix");
                def.type = (t == "Suffix") ? AffixType::Suffix : AffixType::Prefix;
                def.affixName = j.value("affixName", "");
                def.group = j.value("group", "");
                def.level = j.value("level", 1u);
                def.tier = j.value("tier", 1u);
                def.isLocal = j.value("isLocal", false);
                def.rollable = j.value("rollable", false);

                if (j.contains("lines") && j["lines"].is_array()) {
                    for (const auto& jl : j["lines"]) {
                        AffixLineDef line;
                        line.templateEn = jl.value("template", "");
                        line.templateKo = jl.value("koTemplate", line.templateEn);
                        if (jl.contains("ranges") && jl["ranges"].is_array()) {
                            for (const auto& jr : jl["ranges"]) {
                                if (jr.is_array() && jr.size() >= 2) {
                                    AffixRange r;
                                    r.minVal = jr[0].get<double>();
                                    r.maxVal = jr[1].get<double>();
                                    if (r.minVal > r.maxVal) {
                                        std::swap(r.minVal, r.maxVal);
                                    }
                                    r.isFloat = (jr.size() >= 3) ? jr[2].get<bool>() : false;
                                    line.ranges.push_back(r);
                                }
                            }
                        }
                        def.lines.push_back(std::move(line));
                    }
                }

                if (j.contains("weights") && j["weights"].is_array()) {
                    for (const auto& jw : j["weights"]) {
                        if (jw.is_array() && jw.size() >= 2) {
                            def.weights.emplace_back(
                                jw[0].get<std::string>(),
                                jw[1].get<std::uint32_t>());
                        }
                    }
                }

                if (j.contains("tags") && j["tags"].is_array()) {
                    for (const auto& jt : j["tags"]) {
                        if (jt.is_string()) {
                            def.tags.push_back(jt.get<std::string>());
                        }
                    }
                }

                loadedById[def.id] = loadedAffixes.size();
                loadedAffixes.push_back(std::move(def));
            }

            std::vector<CurrencyMetadata> loadedCurrencies;
            if (root.contains("currencies") && root["currencies"].is_array()) {
                for (const auto& jc : root["currencies"]) {
                    CurrencyMetadata cm;
                    cm.id = jc.value("id", "");
                    cm.kind = ParseCurrencyKind(cm.id);
                    if (cm.kind == CurrencyKind::Count) {
                        continue;
                    }
                    cm.enName = jc.value("enName", cm.id);
                    cm.koName = jc.value("koName", cm.enName);
                    cm.koDesc = jc.value("koDesc", "");
                    cm.dropWeight = jc.value("dropWeight", 0u);
                    cm.worldDropTier = static_cast<std::uint8_t>(jc.value("worldDropTier", 0u));
                    loadedCurrencies.push_back(std::move(cm));
                }
            }

            std::unordered_map<std::string, std::vector<BaseArmorCurveRow>> loadedArmorCurves;
            if (root.contains("baseArmorCurves") && root["baseArmorCurves"].is_object()) {
                for (const auto& [key, rowsJson] : root["baseArmorCurves"].items()) {
                    if (!rowsJson.is_array()) continue;
                    std::vector<BaseArmorCurveRow> rows;
                    for (const auto& jr : rowsJson) {
                        BaseArmorCurveRow r;
                        r.reqLevel = jr.value("reqLevel", 1u);
                        r.reqStr = jr.value("reqStr", 0u);
                        r.reqDex = jr.value("reqDex", 0u);
                        r.reqInt = jr.value("reqInt", 0u);
                        r.baseArmour = jr.value("baseArmour", 0.0);
                        r.baseEvasion = jr.value("baseEvasion", 0.0);
                        r.baseEnergyShield = jr.value("baseEnergyShield", 0.0);
                        r.baseBlockChance = jr.value("baseBlockChance", 0.0);
                        rows.push_back(r);
                    }
                    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                        return a.reqLevel < b.reqLevel;
                    });
                    loadedArmorCurves[key] = std::move(rows);
                }
            }

            std::unordered_map<std::string, std::vector<BaseWeaponCurveRow>> loadedWeaponCurves;
            if (root.contains("baseWeaponCurves") && root["baseWeaponCurves"].is_object()) {
                for (const auto& [key, rowsJson] : root["baseWeaponCurves"].items()) {
                    if (!rowsJson.is_array()) continue;
                    std::vector<BaseWeaponCurveRow> rows;
                    for (const auto& jr : rowsJson) {
                        BaseWeaponCurveRow r;
                        r.reqLevel = jr.value("reqLevel", 1u);
                        r.reqStr = jr.value("reqStr", 0u);
                        r.reqDex = jr.value("reqDex", 0u);
                        r.reqInt = jr.value("reqInt", 0u);
                        r.physMin = jr.value("physMin", 10.0);
                        r.physMax = jr.value("physMax", 18.0);
                        r.critChance = jr.value("critChance", 5.0);
                        r.attackRate = jr.value("attackRate", 1.4);
                        rows.push_back(r);
                    }
                    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                        return a.reqLevel < b.reqLevel;
                    });
                    loadedWeaponCurves[key] = std::move(rows);
                }
            }

            std::vector<BeltBaseDef> loadedBelts;
            if (root.contains("beltBases") && root["beltBases"].is_array()) {
                for (const auto& jb : root["beltBases"]) {
                    BeltBaseDef b;
                    b.id = jb.value("id", "");
                    if (b.id.empty()) continue;
                    b.enName = jb.value("enName", b.id);
                    b.koName = jb.value("koName", b.enName);
                    b.localFormId = jb.value("localFormId", 0u);
                    b.reqLevel = jb.value("reqLevel", 1u);
                    b.bonusCharmSlot = jb.value("bonusCharmSlot", false);
                    b.implicitAffixId = jb.value("implicitAffixId", "");
                    loadedBelts.push_back(std::move(b));
                }
            }

            std::vector<CharmMetadata> loadedCharms;
            if (root.contains("charms") && root["charms"].is_array()) {
                for (const auto& jch : root["charms"]) {
                    const auto kInt = jch.value("kind", static_cast<std::uint32_t>(kCharmKindCount));
                    if (kInt >= kCharmKindCount) continue;
                    CharmMetadata cm;
                    cm.id = jch.value("id", "");
                    cm.kind = static_cast<CharmKind>(kInt);
                    cm.localFormId = jch.value("localFormId", 0u);
                    cm.enName = jch.value("enName", cm.id);
                    cm.koName = jch.value("koName", cm.enName);
                    cm.koDesc = jch.value("koDesc", "");
                    cm.maxCharges = jch.value("maxCharges", 40.0);
                    cm.chargesPerUse = jch.value("chargesPerUse", 20.0);
                    cm.durationSec = jch.value("durationSec", 4.0);
                    loadedCharms.push_back(std::move(cm));
                }
            }

            std::vector<MonsterModMetadata> loadedMonsterMods;
            if (root.contains("monsterMods") && root["monsterMods"].is_array()) {
                for (const auto& jm : root["monsterMods"]) {
                    MonsterModMetadata mm;
                    mm.id = jm.value("id", "");
                    mm.kind = ParseMonsterModKind(mm.id);
                    if (mm.kind == MonsterModKind::Count) continue;
                    mm.koName = jm.value("koName", mm.id);
                    mm.koDesc = jm.value("koDesc", "");
                    loadedMonsterMods.push_back(std::move(mm));
                }
            }

            std::vector<UniqueDefinition> loadedUniques;
            std::unordered_map<std::string, std::size_t> loadedUniqueById;
            if (root.contains("uniques") && root["uniques"].is_array()) {
                for (const auto& ju : root["uniques"]) {
                    UniqueDefinition u;
                    u.id = ju.value("id", "");
                    if (u.id.empty()) continue;
                    u.enName = ju.value("enName", u.id);
                    u.koName = ju.value("koName", u.enName);
                    const auto sg = ju.value("slotGroup", 3u);
                    u.slotGroup = (sg < kTokenSlotGroupCount) ? static_cast<TokenSlotGroup>(sg) : TokenSlotGroup::Helmet;
                    const auto ds = ju.value("defenseSubtype", 0u);
                    u.defenseSubtype = (ds <= 3u) ? static_cast<DefenseSubtype>(ds) : DefenseSubtype::None;
                    u.isAmulet = ju.value("isAmulet", false);
                    u.weaponTag = ju.value("weaponTag", "");
                    u.baseFormId = ju.value("baseFormId", 0u);
                    u.reqLevel = ju.value("reqLevel", 1u);
                    if (ju.contains("implicitAffixIds") && ju["implicitAffixIds"].is_array()) {
                        for (const auto& s : ju["implicitAffixIds"]) {
                            if (s.is_string()) u.implicitAffixIds.push_back(s.get<std::string>());
                        }
                    }
                    if (ju.contains("explicitAffixIds") && ju["explicitAffixIds"].is_array()) {
                        for (const auto& s : ju["explicitAffixIds"]) {
                            if (s.is_string()) u.explicitAffixIds.push_back(s.get<std::string>());
                        }
                    }
                    if (ju.contains("specialFlags") && ju["specialFlags"].is_array()) {
                        for (const auto& s : ju["specialFlags"]) {
                            if (s.is_string()) u.specialFlags.push_back(s.get<std::string>());
                        }
                    }
                    loadedUniqueById[u.id] = loadedUniques.size();
                    loadedUniques.push_back(std::move(u));
                }
            }

            affixes_ = std::move(loadedAffixes);
            byId_ = std::move(loadedById);
            currencies_ = std::move(loadedCurrencies);
            beltBases_ = std::move(loadedBelts);
            charms_ = std::move(loadedCharms);
            monsterMods_ = std::move(loadedMonsterMods);
            uniques_ = std::move(loadedUniques);
            uniqueById_ = std::move(loadedUniqueById);
            armorCurves_ = std::move(loadedArmorCurves);
            weaponCurves_ = std::move(loadedWeaponCurves);
            loaded_ = !affixes_.empty();
            return loaded_;
        } catch (const std::exception& ex) {
            if (errorOut) {
                *errorOut = std::string("items_3_30.json 파싱 실패: ") + ex.what();
            }
            return false;
        }
    }

    const AffixDefinition* ItemData::FindAffix(std::string_view id) const
    {
        const auto it = byId_.find(std::string(id));
        if (it == byId_.end()) {
            return nullptr;
        }
        return &affixes_[it->second];
    }

    const CurrencyMetadata* ItemData::FindCurrency(CurrencyKind kind) const
    {
        for (const auto& c : currencies_) {
            if (c.kind == kind) {
                return &c;
            }
        }
        return nullptr;
    }

    const BeltBaseDef* ItemData::FindBelt(std::string_view id) const
    {
        for (const auto& b : beltBases_) {
            if (b.id == id) return &b;
        }
        return nullptr;
    }

    const BeltBaseDef* ItemData::FindBeltByLocalFormId(std::uint32_t localFormId) const
    {
        for (const auto& b : beltBases_) {
            if (b.localFormId == localFormId) return &b;
        }
        return nullptr;
    }

    const CharmMetadata* ItemData::FindCharm(CharmKind kind) const
    {
        for (const auto& c : charms_) {
            if (c.kind == kind) return &c;
        }
        return nullptr;
    }

    const CharmMetadata* ItemData::FindCharmByLocalFormId(std::uint32_t localFormId) const
    {
        for (const auto& c : charms_) {
            if (c.localFormId == localFormId) return &c;
        }
        return nullptr;
    }

    const MonsterModMetadata* ItemData::FindMonsterMod(MonsterModKind kind) const
    {
        for (const auto& m : monsterMods_) {
            if (m.kind == kind) return &m;
        }
        return nullptr;
    }

    const UniqueDefinition* ItemData::FindUnique(std::string_view id) const
    {
        const auto it = uniqueById_.find(std::string(id));
        if (it == uniqueById_.end()) return nullptr;
        return &uniques_[it->second];
    }

    std::uint32_t ItemData::ResolveWeight(
        const AffixDefinition& affix,
        const std::unordered_set<std::string>& itemTags) noexcept
    {
        for (const auto& [key, val] : affix.weights) {
            if (itemTags.contains(key)) {
                return val;
            }
        }
        return 0;
    }

    std::unordered_set<std::string> ItemData::BuildItemTags(
        TokenSlotGroup slotGroup,
        DefenseSubtype defenseSubtype,
        std::string_view weaponTag,
        bool isAmulet,
        std::uint8_t jewelColor)
    {
        std::unordered_set<std::string> tags;
        if (slotGroup == TokenSlotGroup::Jewel) {
            // 5단계: 주얼은 "default"를 넣지 않아 일반 장비 어픽스와 완전 격리하고,
            // PoB Data/Bases/jewel.lua 실측 태그(jewel + not_str/not_dex/not_int)만 부여한다.
            tags.insert("jewel");
            if (jewelColor == 0) {         // Crimson (Str)
                tags.insert("not_dex");
                tags.insert("not_int");
            } else if (jewelColor == 1) {  // Viridian (Dex)
                tags.insert("not_str");
                tags.insert("not_int");
            } else {                       // Cobalt (Int)
                tags.insert("not_str");
                tags.insert("not_dex");
            }
            return tags;
        }
        if (slotGroup == TokenSlotGroup::Map) {
            // 6단계(검수 M-3): 지도는 "default"를 넣지 않아 일반 장비 어픽스와 완전 격리한다.
            tags.insert("map");
            return tags;
        }
        tags.insert("default");

        auto insertArmorSubtypeTag = [&]() {
            tags.insert("armour");
            switch (defenseSubtype) {
                case DefenseSubtype::Armour:       tags.insert("str_armour"); break;
                case DefenseSubtype::Evasion:      tags.insert("dex_armour"); break;
                case DefenseSubtype::EnergyShield: tags.insert("int_armour"); break;
                default:                           tags.insert("str_armour"); break;
            }
        };

        switch (slotGroup) {
            case TokenSlotGroup::OneHandWeapon:
                tags.insert("weapon");
                if (weaponTag == "Staff") {
                    // 검수 m2: 스카이림 kStaff(한손 장착 지팡이)는 검이 아니라 PoE 지팡이(주문) 태그로 분류
                    tags.insert("staff");
                    tags.insert("two_hand_weapon");
                } else {
                    tags.insert("one_hand_weapon");
                    tags.insert("onehand");
                    if (weaponTag == "OneHandAxe") {
                        tags.insert("axe");
                    } else if (weaponTag == "OneHandMace") {
                        tags.insert("mace");
                    } else if (weaponTag == "Dagger") {
                        tags.insert("dagger");
                        tags.insert("attack_dagger");
                    } else {
                        tags.insert("sword");
                    }
                }
                break;

            case TokenSlotGroup::TwoHandOrRangedWeapon:
                tags.insert("weapon");
                tags.insert("two_hand_weapon");
                tags.insert("twohand");
                if (weaponTag == "TwoHandAxe") {
                    tags.insert("axe");
                } else if (weaponTag == "TwoHandMace") {
                    tags.insert("mace");
                } else if (weaponTag == "Bow" || weaponTag == "Crossbow") {
                    tags.insert("bow");
                    tags.insert("ranged");
                } else if (weaponTag == "Warstaff") {
                    tags.insert("staff");
                    tags.insert("warstaff");
                    tags.insert("attack_staff");
                } else if (weaponTag == "Staff") {
                    tags.insert("staff");
                } else {
                    tags.insert("sword");
                }
                break;

            case TokenSlotGroup::Shield:
                insertArmorSubtypeTag();
                tags.insert("shield");
                if (defenseSubtype == DefenseSubtype::Evasion) {
                    tags.insert("dex_shield");
                } else if (defenseSubtype == DefenseSubtype::EnergyShield) {
                    tags.insert("focus");
                } else {
                    tags.insert("str_shield");
                }
                break;

            case TokenSlotGroup::Helmet:
                insertArmorSubtypeTag();
                tags.insert("helmet");
                break;

            case TokenSlotGroup::Body:
                insertArmorSubtypeTag();
                tags.insert("body_armour");
                break;

            case TokenSlotGroup::Gloves:
                insertArmorSubtypeTag();
                tags.insert("gloves");
                break;

            case TokenSlotGroup::Boots:
                insertArmorSubtypeTag();
                tags.insert("boots");
                break;

            case TokenSlotGroup::Jewelry:
                tags.insert(isAmulet ? "amulet" : "ring");
                break;

            case TokenSlotGroup::Belt:
                tags.insert("belt");
                break;

            default:
                break;
        }
        return tags;
    }

    std::vector<std::pair<const AffixDefinition*, std::uint32_t>> ItemData::CollectCandidates(
        AffixType type,
        std::uint32_t itemLevel,
        const std::unordered_set<std::string>& itemTags,
        const std::unordered_set<std::string>& excludedGroups) const
    {
        std::vector<std::pair<const AffixDefinition*, std::uint32_t>> out;
        for (const auto& affix : affixes_) {
            if (!affix.rollable || affix.type != type || affix.level > itemLevel) {
                continue;
            }
            if (!affix.group.empty() && excludedGroups.contains(affix.group)) {
                continue;
            }
            const auto w = ResolveWeight(affix, itemTags);
            if (w > 0) {
                out.emplace_back(&affix, w);
            }
        }
        return out;
    }

    BaseArmorCurveRow ItemData::LookupArmorBase(
        TokenSlotGroup slotGroup,
        DefenseSubtype subtype,
        std::uint32_t itemLevel) const
    {
        const char* prefix = ArmorSlotCurvePrefix(slotGroup);
        if (!prefix || prefix[0] == '\0') {
            return {};
        }
        const std::string key = std::string(prefix) + ":" + DefenseSubtypeKey(subtype);
        const auto it = armorCurves_.find(key);
        if (it == armorCurves_.end() || it->second.empty()) {
            return {};
        }
        const auto& rows = it->second;
        BaseArmorCurveRow best = rows.front();
        for (const auto& r : rows) {
            if (r.reqLevel <= itemLevel) {
                best = r;
            } else {
                break;
            }
        }
        return best;
    }

    BaseWeaponCurveRow ItemData::LookupWeaponBase(
        std::string_view weaponTag,
        std::uint32_t itemLevel) const
    {
        std::string key = weaponTag.empty() ? std::string("OneHandSword") : std::string(weaponTag);
        if (key == "Warstaff") {
            key = "Staff";
        } else if (key == "Crossbow") {
            key = "Bow";
        }
        auto it = weaponCurves_.find(key);
        if (it == weaponCurves_.end() || it->second.empty()) {
            it = weaponCurves_.find("OneHandSword");
            if (it == weaponCurves_.end() || it->second.empty()) {
                return {};
            }
        }
        const auto& rows = it->second;
        BaseWeaponCurveRow best = rows.front();
        for (const auto& r : rows) {
            if (r.reqLevel <= itemLevel) {
                best = r;
            } else {
                break;
            }
        }
        return best;
    }
}
