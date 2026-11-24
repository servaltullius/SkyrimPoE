#include "test_harness.h"

#include "core/AilmentSystem.h"
#include "core/Config.h"
#include "core/ContentCheck.h"
#include "core/DamageConversion.h"
#include "core/EnergyShield.h"
#include "core/ItemAffixParser.h"
#include "core/KillTracker.h"
#include "core/MapSystem.h"
#include "core/RulesPack.h"
#include "core/SkillSystem.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <set>

using namespace SkyrimPoE::Core;

namespace
{
    using nlohmann::json;

    json ReadData(const std::filesystem::path& relative)
    {
        std::ifstream file(std::filesystem::path(POE_TEST_DATA_DIR) / relative, std::ios::binary);
        POE_CHECK(file.is_open());
        return json::parse(file);
    }

    json RulesFixture()
    {
        return ReadData("packs/rules.json");
    }

    DataPack ContentFixture()
    {
        DataPack pack;
        POE_CHECK(pack.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "core_3_30.json"));
        return pack;
    }

    void Reject(std::string_view payload, std::string_view expectedError)
    {
        RulesPack pack;
        std::string error;
        POE_CHECK(!pack.LoadFromJsonString(payload, &error));
        POE_CHECK(pack.Values() == nullptr);
        POE_CHECK(error.find(expectedError) != std::string::npos);
    }
}

const RulesPack* SkyrimPoE::Core::GetRulesPack() noexcept
{
    static const RulesPack pack = [] {
        RulesPack loaded;
        POE_CHECK(loaded.LoadFromJsonString(RulesFixture().dump()));
        return loaded;
    }();
    return &pack;
}

POE_TEST("RulesPack: loads PoB constants, game rules and all one-based level tables")
{
    RulesPack pack;
    std::string error;
    POE_CHECK(pack.LoadFromJsonString(RulesFixture().dump(), &error));
    POE_CHECK(error.empty());
    const auto* rules = pack.Values();
    POE_CHECK(rules);
    POE_CHECK(rules->energyShield.poisonBypasses && rules->energyShield.bleedBypasses);
    POE_CHECK(rules->armour.brokenPlayerDurationSeconds == 4);
    POE_CHECK(rules->armour.damageCoefficient == 10);
    POE_CHECK(rules->armour.physicalReductionCapPercent == 90);
    POE_CHECK(rules->critical.monsterBonusPercent == 30);
    POE_CHECK(rules->critical.playerMinionAdditionalBonusPercent == 70);
    POE_CHECK(rules->levelTables.life[79] == 31065);
    POE_CHECK(rules->levelTables.ailmentThreshold[79] == 65527);
    POE_CHECK(rules->levelTables.poiseThreshold[79] == 213635);
    POE_CHECK(rules->levelTables.evasion.size() == 100);
    POE_CHECK(rules->projectSpirit.achievementRewards[2] == 40);
}

POE_TEST("RulesPack: rejects missing values and retains an earlier valid load")
{
    auto root = RulesFixture();
    RulesPack pack;
    POE_CHECK(pack.LoadFromJsonString(root.dump()));
    root["armour"].erase("damageCoefficient");
    const auto badPayload = root.dump();
    Reject(badPayload, "damageCoefficient");
    POE_CHECK(!pack.LoadFromJsonString(badPayload));
    POE_CHECK(pack.Values()->armour.damageCoefficient == 10);
}

POE_TEST("RulesPack: rejects non-finite JSON numbers")
{
    auto root = RulesFixture();
    root["energyShield"]["rechargeDelaySeconds"] = "nonfinite-placeholder";
    auto payload = root.dump();
    constexpr std::string_view placeholder = "\"nonfinite-placeholder\"";
    payload.replace(payload.find(placeholder), placeholder.size(), "1e999");
    Reject(payload, "overflow");
}

POE_TEST("RulesPack: rejects fractional and out-of-range integer fields")
{
    auto root = RulesFixture();
    auto& spirit = root["project"]["spirit"];
    for (const auto& invalid : { json(-1), json(30.5), json(std::uint64_t{ 1 } << 32) }) {
        spirit["base"] = invalid;
        Reject(root.dump(), "base");
        spirit["base"] = 100;
        spirit["achievementRewards"][2] = invalid;
        Reject(root.dump(), "achievementRewards[2]");
        spirit["achievementRewards"][2] = 40;
    }
    root["chill"]["precision"] = 1.5;
    Reject(root.dump(), "precision");
}

POE_TEST("RulesPack: rejects short and extended level tables")
{
    auto root = RulesFixture();
    auto& table = root["levelTables"]["damage"];
    table.erase(table.size() - 1);
    Reject(root.dump(), "exactly 100");
    table.push_back(100);
    table.push_back(101);
    Reject(root.dump(), "exactly 100");
}

POE_TEST("RulesPack: rejects invalid percentage, zero denominator coefficient, and table entry")
{
    auto root = RulesFixture();
    root["block"]["capPercent"] = 101;
    Reject(root.dump(), "capPercent");
    root = RulesFixture();
    root["armour"]["damageCoefficient"] = 0;
    Reject(root.dump(), "damageCoefficient");
    root = RulesFixture();
    root["levelTables"]["accuracy"][99] = 0;
    Reject(root.dump(), "accuracy[99]");
}

POE_TEST("RulesPack: reads the four-section P2b answer format")
{
    const auto root = ReadData("poe2/answers/p2b.json");
    POE_CHECK(root.is_object() && root.size() == 4);
    std::set<std::string> names;
    for (const std::string_view section : { "pob", "project", "conversion", "gameRule" }) {
        const auto& cases = root.at(section);
        POE_CHECK(cases.is_array());
        POE_CHECK(!cases.empty());
        for (const auto& entry : cases) {
            POE_CHECK(names.insert(entry.at("id").get<std::string>()).second);
            POE_CHECK(entry.at("input").is_object() && !entry.at("input").empty());
            POE_CHECK(entry.at("expected").is_object() && !entry.at("expected").empty());
            const double tolerance = entry.at("tolerance").get<double>();
            POE_CHECK(std::isfinite(tolerance) && tolerance >= 0);
            const auto& source = entry.at("provenance");
            const auto grade = source.at("grade").get<std::string>();
            if (section == "project") {
                POE_CHECK(grade == "project" && source.at("decisions").is_array());
            } else if (section == "conversion") {
                POE_CHECK(grade == "conversion" && source.at("decisions").is_array());
            } else {
                if (section == "pob") {
                    POE_CHECK(grade == "pob");
                } else {
                    POE_CHECK(grade == "gameData" || grade == "official");
                }
                POE_CHECK(source.at("references").is_array() && !source.at("references").empty());
                if (source.value("pobDiffers", false)) {
                    POE_CHECK(source.at("pobReferences").is_array() && !source.at("pobReferences").empty());
                }
            }
        }
    }
}

POE_TEST("P2b player rules: PoB armour and monster hit answer cases")
{
    const auto answers = ReadData("poe2/answers/p2b.json");
    const auto pack = ContentFixture();
    const auto& rules = ActiveRuleValues();
    for (const auto& entry : answers.at("pob")) {
        const auto id = entry.at("id").get<std::string>();
        const auto& input = entry.at("input");
        const auto& expected = entry.at("expected");
        const double tolerance = entry.at("tolerance");
        if (id.starts_with("armour-")) {
            const double armour = input.at("armour");
            const double damage = input.at("damage");
            const double reduction = expected.at("reductionPercent");
            POE_CHECK_NEAR(ArmourReductionPercent(armour, damage), reduction, tolerance);
            // PoB's raw formula preserves negative armour. Actual mitigation also has a floor.
            POE_CHECK_NEAR(ArmourPassRatio(armour, damage),
                1.0 - std::clamp(reduction, 0.0, rules.armour.physicalReductionCapPercent) / 100.0, 1e-9);
        } else if (id.starts_with("monster-hit-")) {
            const auto level = input.at("monsterLevel").get<std::uint32_t>();
            const double accuracy = expected.at("monsterAccuracy");
            POE_CHECK_NEAR(pack.MonsterAccuracy(level), accuracy, tolerance);
            POE_CHECK_NEAR(EvasionHitChance(input.at("evasion"), pack.MonsterAccuracy(level)),
                expected.at("hitChancePercent").get<double>() / 100.0, tolerance);
        }
    }
}

POE_TEST("P2b player rules: PoB life, mana, block, deflection and critical answer cases")
{
    const auto answers = ReadData("poe2/answers/p2b.json");
    const auto pack = ContentFixture();
    const auto& rules = ActiveRuleValues();
    Config config;
    config.enforceGemRequirements = false;
    StatEngine engine;
    for (const auto& entry : answers.at("pob")) {
        const auto id = entry.at("id").get<std::string>();
        const auto& input = entry.at("input");
        const auto& expected = entry.at("expected");
        const double tolerance = entry.at("tolerance");
        Layer1Input layer;
        if (id.starts_with("life-mana-")) {
            const auto* scion = pack.FindClass(layer.className);
            POE_CHECK(scion);
            layer.exileLevel = input.at("level");
            layer.passives.flatStr = input.at("strength").get<double>() - scion->baseStr;
            layer.passives.flatInt = input.at("intelligence").get<double>() - scion->baseInt;
            layer.flatLife = input.value("flatLife", 0.0);
            layer.flatMana = input.value("flatMana", 0.0);
            layer.incLifePct = layer.incManaPct = input.value("increasedPercent", 0.0);
            layer.passives.moreLifeMult = layer.passives.moreManaMult = input.value("moreMultiplier", 1.0);
            engine.SetLayer1(layer);
            POE_CHECK(engine.RebuildIfDirty(pack, config));
            const auto snap = engine.GetSnapshot();
            POE_CHECK_NEAR(snap->poeMaxLife, expected.at("life"), tolerance);
            POE_CHECK_NEAR(snap->poeMaxMana, expected.at("mana"), tolerance);
        } else if (id.starts_with("block-")) {
            layer.attackBlockChance = layer.spellBlockChance = input.at("chancePercent");
            layer.attackBlockMaxBonusPct = layer.spellBlockMaxBonusPct = input.at("addedMaxPercent");
            engine.SetLayer1(layer);
            POE_CHECK(engine.RebuildIfDirty(pack, config));
            const auto snap = engine.GetSnapshot();
            POE_CHECK_NEAR(BlockChanceLimit(layer.attackBlockMaxBonusPct), expected.at("maxPercent"), tolerance);
            POE_CHECK_NEAR(snap->attackBlockChance, expected.at("chancePercent"), tolerance);
            POE_CHECK_NEAR(snap->spellBlockChance, expected.at("chancePercent"), tolerance);
        } else if (id.starts_with("deflect-")) {
            layer.deflectionRating = input.at("rating");
            engine.SetLayer1(layer);
            POE_CHECK(engine.RebuildIfDirty(pack, config));
            const double chance = DeflectionChance(engine.GetSnapshot()->deflectionRating, input.at("accuracy"));
            POE_CHECK_NEAR(chance, expected.at("chancePercent").get<double>() / 100.0, tolerance);
            POE_CHECK_NEAR(rules.deflect.baseReductionPercent, expected.at("reductionOnDeflectPercent"), tolerance);
        } else if (id == "critical-player-base") {
            for (const auto* gemId : {"Cyclone", "Fireball"}) {
                auto& slot = layer.slots[0];
                slot.occupied = true;
                slot.gem.gemId = gemId;
                engine.SetLayer1(layer);
                POE_CHECK(engine.RebuildIfDirty(pack, config));
                const auto snap = engine.GetSnapshot();
                const auto& stats = snap->slotStats[0];
                const double damage = input.at("damage").get<double>() * (stats.critMultiplierPct / 100.0);
                POE_CHECK_NEAR(damage, expected.at("damage"), tolerance);
            }
        }
    }
}

POE_TEST("P2b player rules: PoB hits and game-rule ailment ES answer cases")
{
    const auto answers = ReadData("poe2/answers/p2b.json");
    for (const auto* section : {"pob", "gameRule"}) {
        for (const auto& entry : answers.at(section)) {
            const auto id = entry.at("id").get<std::string>();
            if (!id.starts_with("es-")) {
                continue;
            }
            const auto& input = entry.at("input");
            const auto& expected = entry.at("expected");
            const double tolerance = entry.at("tolerance");
            const std::string kind = input.value("ailment", "");
            const auto ailment = kind == "ignite" ? AilmentKind::Ignite : kind == "bleed" ? AilmentKind::Bleed
                : kind == "poison" ? AilmentKind::Poison : AilmentKind::Count;
            EnergyShieldPool pool;
            pool.Configure();
            pool.ResetFull(input.at("energyShield"));
            const auto before = pool.Snapshot();
            POE_CHECK(before.timeSinceLastHit > 0.0);
            const auto result = pool.AbsorbDamage(input.at("damage"), 1.0, input.at("damageType") == "chaos", ailment);
            const auto after = pool.Snapshot();
            POE_CHECK_NEAR(after.current, expected.at("esRemaining"), tolerance);
            POE_CHECK_NEAR(result.remainingRawDamage, expected.at("lifeLost"), tolerance);
            if (expected.contains("secondsSinceEsLoss")) {
                POE_CHECK_NEAR(after.timeSinceLastHit, expected.at("secondsSinceEsLoss"), tolerance);
            } else if (after.current == before.current) {
                POE_CHECK_NEAR(after.timeSinceLastHit, before.timeSinceLastHit, tolerance);
            }
        }
    }
}

POE_TEST("P2b player rules: all Spirit masks and INI overrides")
{
    const auto answers = ReadData("poe2/answers/p2b.json");
    const auto pack = ContentFixture();
    const auto& rules = ActiveRuleValues();
    StatEngine engine;
    for (const auto& entry : answers.at("project")) {
        const auto id = entry.at("id").get<std::string>();
        if (!id.starts_with("spirit-mask-")) {
            continue;
        }
        Layer1Input layer;
        layer.achievementMask = entry.at("input").at("achievementMask");
        const auto spirit = entry.at("expected").at("spirit").get<std::uint32_t>();
        engine.SetLayer1(layer);
        POE_CHECK(engine.RebuildIfDirty(pack, Config{}));
        const auto snap = engine.GetSnapshot();
        POE_CHECK(snap->baseMaxSpirit == spirit && snap->maxSpirit == spirit);
    }

    const auto cfg = LoadConfigFromString("[Spirit]\nSpiritBase=70\nSpiritPerMilestone=99\n"
        "SpiritMilestoneRewards= 10, 20, 41, 0\nSpiritPerMilestone=55\n");
    POE_CHECK(cfg.warnings.size() == 1);
    POE_CHECK(cfg.warnings[0].find("Ignored [Spirit] SpiritPerMilestone") != std::string::npos);
    POE_CHECK(ComputeSpirit(0x84, rules, &cfg.config) == 111);
    POE_CHECK(ComputeSpirit(0x8, rules, &cfg.config) == 70);
    const auto invalid = LoadConfigFromString("[Spirit]\nSpiritMilestoneRewards=10,20,40,30,50\n");
    POE_CHECK(invalid.warnings.size() == 1);
    POE_CHECK(ComputeSpirit(0x4, rules, &invalid.config) == 140);
}

POE_TEST("P2b KillTracker: all ten level-difference XP answer cases")
{
    const auto answers = ReadData("poe2/answers/p2b.json");
    const auto config = LoadConfigFromFile(std::filesystem::path(POE_TEST_PLUGIN_DIR) / "dist/SKSE/Plugins/SkyrimPoE.ini");
    POE_CHECK(config.fileFound && config.warnings.empty());
    std::size_t count = 0;
    for (const auto& entry : answers.at("project")) {
        if (!entry.at("id").get<std::string>().starts_with("xp-")) continue;
        const auto& input = entry.at("input");
        const double multiplier = KillTracker::ComputeLevelDiffXpMultiplier(
            input.at("exileLevel"), input.at("effectiveMonsterLevel"), config.config);
        POE_CHECK_NEAR(multiplier, entry.at("expected").at("multiplier").get<double>(), entry.at("tolerance").get<double>());
        ++count;
    }
    POE_CHECK(count == 10);
}

POE_TEST("P2b block: signed passive maximum modifiers reach the separate capped chances")
{
    Layer1Input layer;
    layer.attackBlockChance = layer.spellBlockChance = 100.0;
    POE_CHECK(PassiveStatParser::ParseLine("+3% to maximum Chance to Block Attack Damage", layer.passives));
    POE_CHECK(PassiveStatParser::ParseLine("-10% to maximum Chance to Block Spell Damage", layer.passives));
    const auto pack = ContentFixture();
    StatEngine engine;
    engine.SetLayer1(layer);
    POE_CHECK(engine.RebuildIfDirty(pack, Config{}));
    POE_CHECK_NEAR(engine.GetSnapshot()->attackBlockChance, 53.0, 1e-9);
    POE_CHECK_NEAR(engine.GetSnapshot()->spellBlockChance, 40.0, 1e-9);
    POE_CHECK(PassiveStatParser::ParseLine("+60% to maximum Chance to Block Attack Damage", layer.passives));
    engine.SetLayer1(layer);
    POE_CHECK(engine.RebuildIfDirty(pack, Config{}));
    POE_CHECK_NEAR(engine.GetSnapshot()->attackBlockChance, 90.0, 1e-9);
}

POE_TEST("P2b block: equipped signed maximum modifiers add to the matching passive caps")
{
    ItemData items;
    POE_CHECK(items.LoadFromJsonString(R"({"affixes":[{"id":"BlockCaps","lines":[
        {"template":"+{0}% to maximum Chance to Block Attack Damage","ranges":[[1,5]]},
        {"template":"-{0}% to maximum Block chance","ranges":[[1,3]]},
        {"template":"+{0}% to maximum Chance to Block Spell Damage","ranges":[[-10,4]]}
    ]}]})"));
    PoEItemRecord shield;
    shield.affixes.push_back({ "BlockCaps", { 5.0, 2.0, -10.0 } });

    Layer1Input layer;
    layer.attackBlockChance = layer.spellBlockChance = 100.0;
    POE_CHECK(PassiveStatParser::ParseLine("+2% to maximum Chance to Block Attack Damage", layer.passives));
    POE_CHECK(PassiveStatParser::ParseLine("+4% to maximum Chance to Block Spell Damage", layer.passives));
    auto& equipped = layer.equippedGear[static_cast<std::size_t>(EquipSlot::OffHand)];
    equipped.occupied = true;
    equipped.isCrafted = true;
    equipped.stats = ItemAffixParser::ParseItem(shield, items);

    const auto pack = ContentFixture();
    StatEngine engine;
    engine.SetLayer1(layer);
    POE_CHECK(engine.RebuildIfDirty(pack, Config{}));
    POE_CHECK_NEAR(engine.GetSnapshot()->attackBlockChance, 55.0, 1e-9);
    POE_CHECK_NEAR(engine.GetSnapshot()->spellBlockChance, 44.0, 1e-9);

    equipped.occupied = false;
    engine.SetLayer1(layer);
    POE_CHECK(engine.RebuildIfDirty(pack, Config{}));
    POE_CHECK_NEAR(engine.GetSnapshot()->attackBlockChance, 52.0, 1e-9);
    POE_CHECK_NEAR(engine.GetSnapshot()->spellBlockChance, 54.0, 1e-9);
}

POE_TEST("P2b Spirit toggle preview and snapshot use gear, highest active duplicate and a priced trigger")
{
    auto source = ReadData("packs/core_3_30.json");
    auto& gems = source["gems"];
    gems["Determination"]["levels"]["1"]["spiritCost"] = 50;
    gems["Determination"]["levels"]["2"]["spiritCost"] = 75;
    gems["SupportCastOnCriticalStrike"]["levels"]["1"]["spiritCost"] = 50;
    DataPack pack;
    POE_CHECK(pack.LoadFromJsonString(source.dump()));
    Config config;
    config.enforceGemRequirements = false;
    Layer1Input layer;
    layer.slots[0].occupied = layer.slots[1].occupied = layer.slots[2].occupied = true;
    layer.slots[0].gem.gemId = layer.slots[1].gem.gemId = "Determination";
    layer.slots[0].gem.level = 1;
    layer.slots[1].gem.level = 2;
    layer.slots[2].gem.gemId = "SupportCastOnCriticalStrike";
    layer.slots[2].gem.level = 1;
    auto& gear = layer.equippedGear[static_cast<std::size_t>(EquipSlot::OffHand)];
    gear.occupied = true;
    gear.stats.flatSpirit = 30.0;
    StatEngine engine;
    engine.SetLayer1(layer);
    const auto preview = engine.PreviewSpiritToggle(2, 0x7, pack, config);
    POE_CHECK(preview.canReserve && preview.maxSpirit == 130 && preview.requestedSpirit == 125);
    layer.activeAuraMask = 0x7;
    engine.SetLayer1(layer);
    POE_CHECK(engine.RebuildIfDirty(pack, config));
    const auto snapshot = engine.GetSnapshot();
    POE_CHECK(snapshot->effectiveActiveAuraMask == 0x6);
    POE_CHECK(snapshot->maxSpirit == preview.maxSpirit && snapshot->reservedSpirit == preview.reservedSpirit);
    POE_CHECK(snapshot->slotStats[2].spiritCost == 50);
}

POE_TEST("P2b Spirit: a missing level cost inherits the gem cost and an explicit zero overrides it")
{
    auto source = ReadData("packs/core_3_30.json");
    auto& gem = source["gems"]["SupportCastOnCriticalStrike"];
    gem["spiritCost"] = 30;
    gem["levels"]["1"].erase("spiritCost");
    gem["levels"]["2"]["spiritCost"] = 0;
    DataPack pack;
    POE_CHECK(pack.LoadFromJsonString(source.dump()));
    const auto* def = pack.FindGem("SupportCastOnCriticalStrike");
    POE_CHECK(def && def->GetLevel(1) && def->GetLevel(2));
    POE_CHECK(!def->GetLevel(1)->spiritCost.has_value());
    POE_CHECK(def->GetLevel(2)->spiritCost == 0u);

    Config config;
    config.enforceGemRequirements = false;
    Layer1Input layer;
    layer.slots[0].occupied = layer.slots[1].occupied = true;
    layer.slots[0].gem = GemInstance{ 1, def->id, 1 };
    layer.slots[1].gem = GemInstance{ 2, def->id, 2 };
    layer.activeAuraMask = 0x1;
    StatEngine engine;
    engine.SetLayer1(layer);
    POE_CHECK(engine.RebuildIfDirty(pack, config));
    POE_CHECK(engine.GetSnapshot()->reservedSpirit == 30);
    POE_CHECK(engine.GetSnapshot()->slotStats[0].spiritCost == 30);

    const auto preview = engine.PreviewSpiritToggle(1, 0x3, pack, config);
    POE_CHECK(preview.canReserve && preview.reservedSpirit == 0 && preview.requestedSpirit == 0);
    layer.activeAuraMask = 0x3;
    engine.SetLayer1(layer);
    POE_CHECK(engine.RebuildIfDirty(pack, config));
    const auto snapshot = engine.GetSnapshot();
    POE_CHECK(snapshot->effectiveActiveAuraMask == 0x2 && snapshot->reservedSpirit == 0);
    POE_CHECK(snapshot->slotStats[1].auraActive && snapshot->slotStats[1].spiritCost == 0);
}

POE_TEST("P2b skill costs and Molten Shell use the class's rounded pool references")
{
    const auto pack = ContentFixture();
    Config config;
    config.enforceGemRequirements = false;
    config.expectedLifeIncrease = config.expectedManaIncrease = 0.05;
    Layer1Input layer;
    layer.className = "Witch";
    layer.armour = 1000.0;
    layer.slots[0].occupied = layer.slots[1].occupied = layer.slots[2].occupied = layer.slots[3].occupied = true;
    layer.slots[0].gem = GemInstance{ 1, "Fireball", 1 };
    layer.slots[1].gem = GemInstance{ 2, "MoltenShell", 1 };
    layer.slots[2].gem = GemInstance{ 3, "Cyclone", 1 };
    layer.slots[3].gem = GemInstance{ 4, "SupportCastOnCriticalStrike", 1, 0, 0, 3,
        { GemInstance{ 5, "IceNova", 1 } } };
    layer.slots[3].linkedAttackSlot = 2;
    StatEngine engine;
    engine.SetLayer1(layer);
    POE_CHECK(engine.RebuildIfDirty(pack, config));
    const auto snap = engine.GetSnapshot();
    POE_CHECK_NEAR(snap->expectedPoeLife, 59.0, 1e-9);
    POE_CHECK_NEAR(snap->expectedPoeMana, 103.0, 1e-9);

    SkillSystem skills;
    const auto cast = skills.TryBuildHandCastSpell(0, *snap, layer.slots, 500.0, 206.0, pack, config);
    POE_CHECK(cast && cast->skyrimMagickaCost > 0.0);
    POE_CHECK_NEAR(cast->skyrimMagickaCost, snap->slotStats[0].poeManaCost * 2.0, 1e-9);
    const auto guard = skills.TryActivateMoltenShell(1, *snap, layer.slots, 500.0, 206.0, 118.0, pack, config);
    POE_CHECK(guard.ok && guard.skyrimMagickaCost > 0.0);
    POE_CHECK_NEAR(guard.skyrimMagickaCost, snap->slotStats[1].poeManaCost * 2.0, 1e-9);
    POE_CHECK_NEAR(guard.maxShieldSkyrim, snap->slotStats[1].moltenShellAbsorbCapPoe * 2.0, 1e-9);
    const auto triggered = skills.TryTriggerOnCrit(2, *snap, layer.slots, 500.0, 206.0, pack, config);
    POE_CHECK(triggered && triggered->skyrimMagickaCost > 0.0);
    POE_CHECK_NEAR(triggered->skyrimMagickaCost, snap->triggeredSpellStats.at(5).poeManaCost * 2.0, 1e-9);
}

POE_TEST("P2b physical defense: negative armour floor and pre-normalization Astramentis delta")
{
    POE_CHECK(ArmourReductionPercent(-100.0, 100.0) < 0.0);
    POE_CHECK_NEAR(ArmourPassRatio(-100.0, 100.0), 1.0, 1e-9);
    POE_CHECK_NEAR(ArmourPassRatio(-100.0, 100.0, 15.0), 0.94, 1e-9);
    const auto pack = ContentFixture();
    Config config;
    config.defenseDref = 23.0;
    config.defenseRollback = 0.6;
    for (const auto level : { 1u, 50u, 80u }) {
        StatSnapshot snapshot;
        snapshot.armour = 125.0;
        const double raw = 50.0;
        const double baseline = ConvertDefensePhysicalHit(raw, level, snapshot, pack, config);
        const double expectedNormalization = std::pow(ExpectedPhysicalPass(level, pack, config), 0.6) *
            std::pow(ExpectedPhysicalPass(1, pack, config), 0.4);
        snapshot.flatPhysTakenFromAttacksDelta = -4.0;
        const double withAstramentis = ConvertDefensePhysicalHit(raw, level, snapshot, pack, config);
        const double poeDelta = (withAstramentis - baseline) * expectedNormalization *
            pack.MonsterDamage(level) * pack.Constants().monsterDamageMult / config.defenseDref;
        POE_CHECK_NEAR(poeDelta, -4.0, 1e-9);
    }
}

POE_TEST("P2b spell defense: projectile block uses the higher chance and Shavronne absorbs poison as chaos")
{
    StatSnapshot snapshot;
    snapshot.attackBlockChance = 53.0;
    snapshot.spellBlockChance = 40.0;
    POE_CHECK_NEAR(SpellHitBlockChance(snapshot, true), 53.0, 1e-9);
    POE_CHECK_NEAR(SpellHitBlockChance(snapshot, false), 40.0, 1e-9);
    snapshot.spellBlockChance = 87.0;
    POE_CHECK_NEAR(SpellHitBlockChance(snapshot, true), 87.0, 1e-9);

    EnergyShieldPool shield;
    shield.ResetFull(60.0);
    const auto poison = shield.AbsorbDamage(50.0, 1.0, true, EnergyShieldBypassAilment(AilmentKind::Poison, false));
    POE_CHECK_NEAR(poison.remainingRawDamage, 50.0, 1e-9);
    POE_CHECK_NEAR(poison.newEnergyShield, 60.0, 1e-9);
    const auto shav = shield.AbsorbDamage(50.0, 1.0, true, EnergyShieldBypassAilment(AilmentKind::Poison, true));
    POE_CHECK_NEAR(shav.remainingRawDamage, 20.0, 1e-9);
    POE_CHECK_NEAR(shav.newEnergyShield, 0.0, 1e-9);
    POE_CHECK(EnergyShieldBypassAilment(AilmentKind::Bleed, true) == AilmentKind::Bleed);
}

POE_TEST("P2b vanilla damage: the ailment dispatch accepts attacks and excludes spell effects")
{
    POE_CHECK(CanInflictPoEAilments(VanillaHitSource::Attack));
    POE_CHECK(!CanInflictPoEAilments(VanillaHitSource::MagicEffect));
}

POE_TEST("P2b ailment threshold uses damage after deflection")
{
    const double afterDeflection = 6.0 * (1.0 - ActiveRuleValues().deflect.baseReductionPercent / 100.0);
    POE_CHECK_NEAR(afterDeflection, 3.6, 1e-9);
    POE_CHECK(PlayerHitMeetsAilmentThreshold(6.0, 100.0));
    POE_CHECK(!PlayerHitMeetsAilmentThreshold(afterDeflection, 100.0));
    POE_CHECK(PlayerHitMeetsAilmentThreshold(5.0, 100.0));
}

POE_TEST("P2b armour break: enemy physical hit bonus expires and excludes physical DoT")
{
    const auto answers = ReadData("poe2/answers/p2b.json");
    bool sawPlayer = false;
    bool sawEnemy = false;
    for (const auto& entry : answers.at("gameRule")) {
        const auto id = entry.at("id").get<std::string>();
        const auto& input = entry.at("input");
        const auto& expected = entry.at("expected");
        ArmourBreakState broken;
        constexpr double appliedAt = 10.0;
        if (id == "armour-break-player") {
            const double end = appliedAt + expected.at("durationSeconds").get<double>();
            broken.ApplyFullyBroken(appliedAt, true);
            POE_CHECK(broken.IsFullyBroken(end - 0.01) && !broken.IsFullyBroken(end));
            POE_CHECK_NEAR((broken.DamageTakenMultiplier(appliedAt, DamageType::Physical, false) - 1.0) * 100.0,
                expected.at("physicalHitBonusPercent"), 1e-9);
            sawPlayer = true;
        } else if (id == "armour-break-enemy") {
            const double duration = input.at("elapsedSeconds");
            const double damage = input.at("damage");
            EnemyArmourBreaks targets;
            targets.Advance(appliedAt, 1);
            POE_CHECK_NEAR(targets.DamageTakenMultiplier(50, 1, DamageType::Physical, false), 1.0, 1e-9);
            targets.ApplyFullyBroken(50, 1);
            targets.Advance(duration - 0.01, 1);
            POE_CHECK_NEAR(damage * targets.DamageTakenMultiplier(50, 1, DamageType::Physical, false), expected.at("physicalHit"), 1e-9);
            POE_CHECK_NEAR(damage * targets.DamageTakenMultiplier(50, 1, DamageType::Physical, true), expected.at("physicalDot"), 1e-9);
            POE_CHECK_NEAR(damage * targets.DamageTakenMultiplier(50, 1, DamageType::Fire, false), expected.at("elementalHit"), 1e-9);
            targets.Advance(0.02, 1);
            POE_CHECK_NEAR(damage * targets.DamageTakenMultiplier(50, 1, DamageType::Physical, false), expected.at("physicalHitAfterExpiry"), 1e-9);
            targets.ApplyFullyBroken(50, 1);
            targets.Advance(0.0, 2);
            targets.ApplyFullyBroken(50, 1);
            targets.Advance(10.0, 1);
            POE_CHECK_NEAR(targets.DamageTakenMultiplier(50, 2, DamageType::Physical, false), 1.0, 1e-9);
            sawEnemy = true;
        }
    }
    POE_CHECK(sawPlayer && sawEnemy);
}

POE_TEST("P2b armour break: mixed hits amplify only the physical damage actually rolled")
{
    EnemyArmourBreaks targets;
    targets.ApplyFullyBroken(50, 1);
    DamagePacket base;
    base.byType[static_cast<std::size_t>(DamageType::Physical)] = { 0.0, 80.0 };
    base.byType[static_cast<std::size_t>(DamageType::Cold)] = { 60.0, 60.0 };

    const auto coldOnly = RollSpellDamagePacket(base, 0.0, 1.0);
    const auto coldDominant = RollSpellDamagePacket(base, 0.5, 1.0);
    const auto physicalDominant = RollSpellDamagePacket(base, 1.0, 1.0);
    POE_CHECK_NEAR(targets.ApplyToDamage(50, 1, 60.0, PhysicalDamageFraction(coldOnly), false), 60.0, 1e-9);
    POE_CHECK_NEAR(targets.ApplyToDamage(50, 1, 100.0, PhysicalDamageFraction(coldDominant), false), 108.0, 1e-9);
    POE_CHECK_NEAR(targets.ApplyToDamage(50, 1, 140.0, PhysicalDamageFraction(physicalDominant), false), 156.0, 1e-9);
    POE_CHECK_NEAR(targets.ApplyToDamage(50, 1, 100.0, 1.0, false), 120.0, 1e-9);
    POE_CHECK_NEAR(targets.ApplyToDamage(50, 1, 100.0, 1.0, true), 100.0, 1e-9);
    targets.Advance(12.0, 1);
    POE_CHECK_NEAR(targets.ApplyToDamage(50, 1, 100.0, PhysicalDamageFraction(coldDominant), false), 100.0, 1e-9);
}

POE_TEST("P2b player rules: Skyrim physical, life and difficulty conversion answer cases")
{
    const auto answers = ReadData("poe2/answers/p2b.json");
    const auto pack = ContentFixture();
    const auto loaded = LoadConfigFromFile(std::filesystem::path(POE_TEST_PLUGIN_DIR) / "dist/SKSE/Plugins/SkyrimPoE.ini");
    POE_CHECK(loaded.fileFound && loaded.warnings.empty());
    for (const auto& entry : answers.at("conversion")) {
        const auto id = entry.at("id").get<std::string>();
        const auto& input = entry.at("input");
        const auto& expected = entry.at("expected");
        const double tolerance = entry.at("tolerance");
        auto config = loaded.config;
        if (id.starts_with("offense-life-")) {
            const auto level = input.at("level").get<std::uint32_t>();
            POE_CHECK_NEAR(config.offenseK, input.at("offenseK"), tolerance);
            POE_CHECK_NEAR(config.offenseRollback, input.at("offenseRollback"), tolerance);
            POE_CHECK_NEAR(OffenseReferenceLife(level, pack, config), expected.at("referenceLife"), tolerance);
        } else if (id.starts_with("physical-profile-")) {
            const auto level = input.at("level").get<std::uint32_t>();
            const double hit = input.at("baseHit");
            config.defenseDref = hit;
            config.defenseRollback = input.at("defenseRollback");
            const auto& profile = pack.ExpectedDefense(level);
            const double gearMult = 1.0 + config.expectedArmorIncrease;
            StatSnapshot armour;
            armour.armour = profile.bestArmour * gearMult;
            const StatSnapshot unarmoured;
            const double armourHit = ConvertDefensePhysicalHit(hit, level, armour, pack, config);
            const double bareHit = ConvertDefensePhysicalHit(hit, level, unarmoured, pack, config);
            const double chance = EvasionHitChance(profile.bestEvasion * gearMult, pack.MonsterAccuracy(level));
            const double normalized = (armourHit + bareHit * chance + bareHit) / (3.0 * hit);
            POE_CHECK_NEAR(normalized, expected.at("normalizedPass"), tolerance);
        } else if (id.starts_with("expected-life-")) {
            const auto level = input.at("level").get<std::uint32_t>();
            const auto* cls = pack.FindClass(input.at("class").get<std::string>());
            POE_CHECK(cls);
            const double health = ComputeSkyrimMaxHealth(input.at("baseSkyrimHealth"), input.at("poeLife"),
                ExpectedLife(level, cls->baseStr, config));
            POE_CHECK_NEAR(health, expected.at("skyrimHealth"), tolerance);
        } else if (id.starts_with("starting-pools-")) {
            Layer1Input layer;
            layer.className = input.at("class").get<std::string>();
            StatEngine engine;
            engine.SetLayer1(layer);
            config.expectedLifeIncrease = config.expectedManaIncrease = 0.0;
            POE_CHECK(engine.RebuildIfDirty(pack, config));
            const auto snap = engine.GetSnapshot();
            POE_CHECK(snap->str == input.at("startingStrength") && snap->intel == input.at("startingIntelligence"));
            const double health = ComputeSkyrimMaxHealth(123.0, snap->poeMaxLife, snap->expectedPoeLife);
            const auto mana = ComputeMagickaScaling(123.0, snap->poeMaxMana, 0.0, snap->expectedPoeMana);
            POE_CHECK_NEAR(health / 123.0, expected.at("healthMultiplier"), tolerance);
            POE_CHECK_NEAR(mana.skyrimMaxMagicka / 123.0, expected.at("magickaMultiplier"), tolerance);
        } else if (id.starts_with("es-difficulty-")) {
            const auto absorbed = AbsorbWithEnergyShield(input.at("rawDamage"), input.at("difficulty"), false, input.at("energyShield"));
            POE_CHECK_NEAR(absorbed.newEnergyShield, expected.at("esRemaining"), tolerance);
            POE_CHECK_NEAR(absorbed.absorbedEffectiveDamage, expected.at("absorbedEffectiveDamage"), tolerance);
            POE_CHECK_NEAR(absorbed.remainingRawDamage, expected.at("remainingRawDamage"), tolerance);
        } else {
            POE_CHECK(false);
        }
    }
}

POE_TEST("ContentCheck: the rules meta and manifest hash participate in four-pack startup checks")
{
    const std::array<ContentFile, 5> files{{
        { "packs/core_3_30.json", std::string(64, 'a') },
        { "packs/tree_3_30.json", std::string(64, 'b') },
        { "packs/items_3_30.json", std::string(64, 'c') },
        { "packs/rules.json", std::string(64, 'd') },
        { "SkyrimPoE.esp", std::string(64, 'e') },
    }};
    constexpr auto buildId = "1234567890abcdef";
    std::array<json, 4> meta{
        ReadData(files[0].path).at("meta"), ReadData(files[1].path).at("meta"),
        ReadData(files[2].path).at("meta"), RulesFixture().at("meta"),
    };
    meta[3].update({ { "game", "poe1" }, { "gameVersion", "3.30" }, { "contentGeneration", 0 } });
    for (auto& entry : meta) {
        entry["buildId"] = buildId;
    }
    json manifest = {
        { "manifestVersion", 1 }, { "game", "poe1" }, { "gameVersion", "3.30" },
        { "contentGeneration", 0 }, { "buildId", buildId },
        { "files", json::array() },
    };
    for (const auto& file : files) {
        manifest["files"].push_back({
            { "path", file.path }, { "sha256", *file.sha256 }, { "size", 123 },
        });
    }
    POE_CHECK(CheckContent(meta, manifest, files, std::nullopt).identity);
    const auto savedMeta = meta[3];
    meta[3] = nullptr;
    POE_CHECK(!CheckContent(meta, manifest, files, std::nullopt).identity);
    meta[3] = savedMeta;
    meta[3]["schemaVersion"] = 2;
    POE_CHECK(!CheckContent(meta, manifest, files, std::nullopt).identity);
    meta[3] = savedMeta;
    manifest["files"][3]["sha256"] = std::string(64, 'f');
    POE_CHECK(!CheckContent(meta, manifest, files, std::nullopt).identity);
    manifest["files"].erase(3);
    POE_CHECK(!CheckContent(meta, manifest, files, std::nullopt).identity);
}

POE_TEST("P2b enemy rules: positive table entries and stack limit are validated by RulesPack")
{
    const auto root = RulesFixture();
    for (const auto* table : { "life", "damage", "accuracy", "evasion", "ailmentThreshold", "poiseThreshold", "mapLevelLifeMult" }) {
        auto invalid = root;
        invalid["levelTables"][table][67] = 0;
        Reject(invalid.dump(), table);
    }
    auto invalid = root;
    invalid["poison"]["baseMaxStacks"] = 0;
    Reject(invalid.dump(), "baseMaxStacks");
    invalid["poison"]["baseMaxStacks"] = 1.5;
    Reject(invalid.dump(), "baseMaxStacks");
}

POE_TEST("P2b enemy rules: PoB level tables, ailment chances, damage and buildup answers")
{
    const auto answers = ReadData("poe2/answers/p2b.json");
    const auto pack = ContentFixture();
    const auto& rules = ActiveRuleValues();
    for (const auto& entry : answers.at("pob")) {
        const auto id = entry.at("id").get<std::string>();
        const auto& input = entry.at("input");
        const auto& expected = entry.at("expected");
        const double tolerance = entry.at("tolerance");
        constexpr std::uint32_t actor = 1;
        AilmentSystem ailments;
        HitAilmentInput hit;
        if (id.starts_with("thresholds-")) {
            const auto level = input.at("monsterLevel").get<std::uint32_t>();
            POE_CHECK_NEAR(pack.MonsterLife(level), expected.at("life"), tolerance);
            POE_CHECK_NEAR(pack.MonsterDamage(level), expected.at("damage"), tolerance);
            POE_CHECK_NEAR(pack.MonsterAccuracy(level), expected.at("accuracy"), tolerance);
            POE_CHECK_NEAR(rules.levelTables.At(rules.levelTables.evasion, level), expected.at("evasion"), tolerance);
            POE_CHECK_NEAR(MapSystem::AilmentThreshold(level), expected.at("ailmentThreshold"), tolerance);
            POE_CHECK_NEAR(MapSystem::PoiseThreshold(level), expected.at("poiseThreshold"), tolerance);
        } else if (id.starts_with("ignite-chance-") || id.starts_with("shock-chance-")) {
            const bool shock = id.starts_with("shock-");
            const double coefficient = shock ? rules.shock.chanceMultiplier : rules.ignite.chanceMultiplier;
            const double chance = ThresholdAilmentChance(input.at("damage"), input.at("ailmentThreshold"), coefficient,
                input.at("baseChancePercent"), input.at("increasedChancePercent"), input.at("moreChanceMultiplier"));
            POE_CHECK_NEAR(chance, expected.at("chancePercent"), tolerance);
            if (shock) {
                hit.lightningHitPoe = input.at("damage");
                hit.targetAilmentThresholdPoe = input.at("ailmentThreshold");
                hit.shockRoll01 = 0.0;
                POE_CHECK(ailments.ApplyHitAilments(actor, hit).appliedShock);
                POE_CHECK_NEAR(ailments.GetState(actor)->shockRemaining, expected.at("enemyDurationSeconds"), tolerance);
                POE_CHECK_NEAR((ailments.ShockDamageMultiplier(actor) - 1) * 100, expected.at("effectPercent"), tolerance);
                POE_CHECK_NEAR(rules.shock.playerDurationSeconds, expected.at("playerDurationSeconds"), tolerance);
                (void)ailments.TickActor(actor, expected.at("enemyDurationSeconds"), false);
                POE_CHECK(ailments.ShockDamageMultiplier(actor) == 1.0);
            }
        } else if (id.starts_with("chill-breakdown-")) {
            const double raw = RawChillPercent(input.at("coldDamage"), input.at("ailmentThreshold"));
            POE_CHECK_NEAR(raw, expected.at("rawEffectPercent"), tolerance);
            POE_CHECK_NEAR(rules.chill.minPercent, expected.at("minPercent"), tolerance);
            POE_CHECK_NEAR(rules.chill.maxPercent, expected.at("maxPercent"), tolerance);
            // The PoB breakdown's chance column uses a separate strict 1% check.
            POE_CHECK_NEAR(raw > 1.0 ? 100 : 0, expected.at("chanceOnHitPercent"), tolerance);
        } else if (id.starts_with("freeze-")) {
            const auto level = input.at("monsterLevel").get<std::uint32_t>();
            hit.coldHitPoe = input.at("coldDamage");
            hit.targetAilmentThresholdPoe = MapSystem::AilmentThreshold(level);
            hit.targetPoiseThresholdPoe = MapSystem::PoiseThreshold(level);
            hit.isBossOrDragon = id.starts_with("freeze-boss-");
            hit.isMapBoss = input.value("mapBoss", false);
            if (expected.contains("poiseThreshold")) {
                POE_CHECK_NEAR(hit.targetPoiseThresholdPoe, expected.at("poiseThreshold"), tolerance);
            }
            POE_CHECK(!ailments.ApplyHitAilments(actor, hit).appliedFreeze);
            POE_CHECK(ailments.GetState(actor));
            const auto& freeze = ailments.GetState(actor)->poise[static_cast<std::size_t>(PoiseKind::Freeze)];
            POE_CHECK_NEAR(freeze.ratio * 100, expected.at("buildupPercent"), tolerance);
        } else if (id.starts_with("dot-")) {
            const auto kind = input.at("ailment").get<std::string>();
            const double damage = input.at("hitDamage");
            const bool moving = input.at("moving");
            if (kind == "ignite") {
                hit.fireHitPoe = damage;
                hit.igniteChancePct = 100.0;
            } else if (kind == "bleed") {
                hit.physHitPoe = damage;
                hit.bleedChancePct = 100.0;
                hit.aggravatesBleed = input.at("aggravated");
            } else {
                POE_CHECK(kind == "poison");
                hit.physHitPoe = damage * 0.5;
                hit.chaosHitPoe = damage - hit.physHitPoe;
                hit.poisonChancePct = 100.0;
            }
            (void)ailments.ApplyHitAilments(actor, hit);
            const double dps = expected.at("damagePerSecond");
            const double duration = expected.at("durationSeconds");
            POE_CHECK_NEAR(ailments.TickActor(actor, 1, moving).TotalPoe(), dps, tolerance);
            POE_CHECK_NEAR(ailments.TickActor(actor, duration, moving).TotalPoe(), dps * (duration - 1), tolerance);
            POE_CHECK_NEAR(ailments.TickActor(actor, 1, moving).TotalPoe(), 0, tolerance);
        } else if (id == "poison-base-stacks") {
            hit.poisonChancePct = 100.0;
            const auto& hits = input.at("hitDamage");
            for (const auto& damage : hits) {
                hit.chaosHitPoe = damage;
                (void)ailments.ApplyHitAilments(actor, hit);
                POE_CHECK(ailments.GetState(actor)->activePoisonCount == expected.at("activeStacks").get<std::size_t>());
            }
            const double duration = expected.at("durationSeconds");
            const double damagePerSecond = expected.at("damagePerSecond");
            POE_CHECK_NEAR(ailments.TickActor(actor, duration + 1, false).chaosPoeDamage, damagePerSecond * duration, tolerance);
            POE_CHECK(ailments.GetState(actor) == nullptr);
        }
    }
}

POE_TEST("P2b enemy rules: game-rule chill, freeze and flammability sequences")
{
    const auto answers = ReadData("poe2/answers/p2b.json");
    const auto& rules = ActiveRuleValues();
    auto cases = answers.at("gameRule");
    const auto& projectCases = answers.at("project");
    const auto independentRolls = std::ranges::find_if(projectCases, [](const auto& entry) {
        return entry.at("id") == "ailment-independent-rolls";
    });
    POE_CHECK(independentRolls != projectCases.end());
    cases.push_back(*independentRolls);
    for (const auto& entry : cases) {
        const auto id = entry.at("id").get<std::string>();
        const auto& input = entry.at("input");
        const auto& expected = entry.at("expected");
        const double tolerance = entry.at("tolerance");
        constexpr std::uint32_t actor = 1;
        AilmentSystem ailments;
        HitAilmentInput hit;
        if (id.starts_with("ailment-handoff-")) {
            const auto kind = id.substr(std::string("ailment-handoff-").size());
            const double weakAt = input.at("weakAt");
            if (kind == "chill" || kind == "shock") {
                const double strong = input.at("strongPercent");
                const double weak = input.at("weakPercent");
                const double expires = input.at("strongExpires");
                hit.targetFreezeImmune = true;
                if (kind == "chill") {
                    hit.coldHitPoe = strong / rules.chill.effectMultiplier * hit.targetAilmentThresholdPoe;
                } else {
                    hit.lightningHitPoe = 100.0;
                    hit.shockChancePct = 100.0;
                }
                (void)ailments.ApplyHitAilments(actor, hit);
                (void)ailments.TickActor(actor, weakAt, false);
                if (kind == "chill") {
                    hit.coldHitPoe = weak / rules.chill.effectMultiplier * hit.targetAilmentThresholdPoe;
                    (void)ailments.ApplyHitAilments(actor, hit);
                } else {
                    ailments.ProliferateIgniteAndShock(actor, 0.0, 0.0, weak / 100.0, expires, false, weakAt);
                }
                const auto effect = [&]() {
                    return 100.0 * (kind == "chill" ? ailments.ChillSlowRatio(actor) :
                        ailments.ShockDamageMultiplier(actor) - 1.0);
                };
                POE_CHECK_NEAR(effect(), expected.at("effects")[0], tolerance);
                (void)ailments.TickActor(actor, expires - weakAt, false);
                POE_CHECK_NEAR(effect(), expected.at("effects")[1], tolerance);
                (void)ailments.TickActor(actor, weakAt, false);
                POE_CHECK_NEAR(effect(), expected.at("effects")[2], tolerance);
            } else {
                const auto hitWith = [&](double damage) {
                    if (kind == "ignite") {
                        hit.fireHitPoe = damage;
                        hit.igniteChancePct = 100.0;
                    } else if (kind == "bleed") {
                        hit.physHitPoe = damage;
                        hit.bleedChancePct = 100.0;
                    } else {
                        POE_CHECK(kind == "poison");
                        hit.chaosHitPoe = damage;
                        hit.poisonChancePct = 100.0;
                    }
                    (void)ailments.ApplyHitAilments(actor, hit);
                };
                hitWith(input.at("strongHit"));
                (void)ailments.TickActor(actor, weakAt, false);
                hitWith(input.at("weakHit"));
                POE_CHECK_NEAR(ailments.TickActor(actor, input.at("crossingTick"), false).TotalPoe(),
                    expected.at("damageThroughCrossing"), tolerance);
                POE_CHECK_NEAR(ailments.TickActor(actor, weakAt, false).TotalPoe(), expected.at("remainingDamage"), tolerance);
            }
        } else if (id.starts_with("ailment-rarity-") || id == "ailment-freeze-resistance") {
            const auto rarity = input.value("rarity", std::string{});
            if (rarity == "magic") hit.targetRarity = MonsterRarity::Magic;
            if (rarity == "rare") hit.targetRarity = MonsterRarity::Rare;
            hit.isBossOrDragon = input.value("unique", false);
            hit.isMapBoss = input.value("mapBoss", false);
            hit.coldHitPoe = input.at("coldDamage");
            hit.targetPoiseThresholdPoe = input.at("poiseThreshold");
            hit.coldDamageAfterResistanceMultiplier = 1.0 - input.value("coldResistancePercent", 0.0) / 100.0;
            POE_CHECK(!ailments.ApplyHitAilments(actor, hit).appliedFreeze);
            const auto* state = ailments.GetState(actor);
            POE_CHECK(state != nullptr);
            POE_CHECK_NEAR(state->poise[static_cast<std::size_t>(PoiseKind::Freeze)].ratio * 100.0,
                expected.at("buildupPercent"), tolerance);
            if (expected.contains("chillPercent")) {
                POE_CHECK_NEAR(ailments.ChillSlowRatio(actor) * 100.0, expected.at("chillPercent"), tolerance);
            }
        } else if (id == "ailment-independent-rolls") {
            const double damage = input.at("damagePerType");
            const double chance = input.at("chance");
            hit.targetAilmentThresholdPoe = input.at("threshold");
            hit.physHitPoe = hit.fireHitPoe = hit.lightningHitPoe = damage;
            hit.igniteChancePct = chance - damage / hit.targetAilmentThresholdPoe * rules.ignite.chanceMultiplier;
            hit.shockChancePct = chance - damage / hit.targetAilmentThresholdPoe * rules.shock.chanceMultiplier;
            hit.bleedChancePct = hit.poisonChancePct = chance;
            const auto& rolls = input.at("rolls");
            hit.igniteRoll01 = rolls[0];
            hit.shockRoll01 = rolls[1];
            hit.bleedRoll01 = rolls[2];
            hit.poisonRoll01 = rolls[3];
            const auto result = ailments.ApplyHitAilments(actor, hit);
            const auto& applied = expected.at("applied");
            POE_CHECK(result.appliedIgnite == applied[0].get<bool>());
            POE_CHECK(result.appliedShock == applied[1].get<bool>());
            POE_CHECK(result.appliedBleed == applied[2].get<bool>());
            POE_CHECK(result.appliedPoison == applied[3].get<bool>());
        } else if (id == "ailment-flammability-chance-bonus") {
            hit.fireHitPoe = input.at("fireDamage");
            hit.targetAilmentThresholdPoe = input.at("threshold");
            hit.igniteChancePct = input.at("firstBonus");
            hit.igniteRoll01 = 1.0;
            POE_CHECK(!ailments.ApplyHitAilments(actor, hit).appliedIgnite);
            POE_CHECK_NEAR(ailments.FlammabilityChancePercent(actor), expected.at("magnitudeByHit")[0], tolerance);
            hit.igniteChancePct = 0.0;
            hit.igniteRoll01 = input.at("secondRoll");
            POE_CHECK(ailments.ApplyHitAilments(actor, hit).appliedIgnite == expected.at("secondIgnites").get<bool>());
            POE_CHECK_NEAR(ailments.FlammabilityChancePercent(actor), expected.at("magnitudeByHit")[1], tolerance);
        } else if (id.starts_with("chill-game-")) {
            hit.coldHitPoe = input.at("coldDamage");
            hit.targetAilmentThresholdPoe = input.at("ailmentThreshold");
            hit.targetFreezeImmune = true;
            hit.isCrit = true;
            const auto applied = ailments.ApplyHitAilments(actor, hit);
            POE_CHECK(applied.appliedChill == expected.at("applies").get<bool>());
            POE_CHECK_NEAR(ailments.ChillSlowRatio(actor) * 100, expected.at("effectPercent"), tolerance);
            if (applied.appliedChill) {
                POE_CHECK_NEAR(ailments.GetState(actor)->chillRemaining, 8.0, tolerance);
                (void)ailments.TickActor(actor, 8.0, false);
                POE_CHECK(ailments.GetState(actor) == nullptr);
            }
        } else if (id == "freeze-game-buildup") {
            hit.targetPoiseThresholdPoe = input.at("poiseThreshold");
            const auto& steps = input.at("steps");
            double previousTime = 0.0;
            for (std::size_t i = 0; i < steps.size(); ++i) {
                const double now = steps[i].at("timeSeconds");
                (void)ailments.TickActor(actor, now - previousTime, false);
                hit.coldHitPoe = steps[i].at("coldDamage");
                (void)ailments.ApplyHitAilments(actor, hit);
                POE_CHECK(ailments.IsFrozen(actor) == expected.at("steps")[i].at("frozen").get<bool>());
                const auto* state = ailments.GetState(actor);
                const double buildup = state ? state->poise[static_cast<std::size_t>(PoiseKind::Freeze)].ratio : 0.0;
                POE_CHECK_NEAR(buildup * 100, expected.at("steps")[i].at("buildupPercent"), tolerance);
                previousTime = now;
            }
        } else if (id.starts_with("ignite-flammability-")) {
            const bool player = input.at("target") == "player";
            const auto& hits = input.at("hits");
            hit.targetAilmentThresholdPoe = input.at("ailmentThreshold");
            hit.isCrit = true;
            Flammability flammability;
            double previousTime = 0.0;
            for (std::size_t i = 0; i < hits.size(); ++i) {
                const double now = hits[i].at("timeSeconds");
                hit.fireHitPoe = hits[i].at("fireDamage");
                const double chance = expected.at("chancePercentByHit")[i];
                if (player) {
                    // Only the mechanic is shared. Enemy hits on the Skyrim player keep their project chances.
                    flammability.Tick(now - previousTime);
                    flammability.AddHit(hit.fireHitPoe, hit.targetAilmentThresholdPoe,
                        rules.ignite.flammabilityPlayerDurationSeconds);
                    POE_CHECK_NEAR(flammability.ChancePercent(), chance, tolerance);
                } else {
                    (void)ailments.TickActor(actor, now - previousTime, false);
                    const auto applied = ailments.ApplyHitAilments(actor, hit);
                    POE_CHECK_NEAR(ailments.FlammabilityChancePercent(actor), chance, tolerance);
                    if (i < 2) {
                        POE_CHECK(applied.appliedIgnite == (chance > hit.igniteRoll01 * 100));
                    }
                }
                previousTime = now;
            }
        }
    }
}

POE_TEST("P2b enemy rules: buildup sources, death cleanup and reload cleanup")
{
    constexpr std::uint32_t first = 1;
    constexpr std::uint32_t second = 2;
    AilmentSystem ailments;
    HitAilmentInput hit;
    hit.coldHitPoe = hit.fireHitPoe = 100.0;
    const auto ordinary = ailments.ApplyHitAilments(first, hit);
    POE_CHECK(ordinary.appliedFreeze);
    POE_CHECK(!ordinary.appliedElectrocute && !ordinary.appliedHeavyStun && !ordinary.appliedPin);
    const auto* active = ailments.GetState(first);
    for (const auto kind : { PoiseKind::Electrocute, PoiseKind::HeavyStun, PoiseKind::Pin }) {
        POE_CHECK(active->poise[static_cast<std::size_t>(kind)].IsEmpty());
    }
    hit.electrocuteHitPoe = hit.heavyStunHitPoe = hit.pinHitPoe = 1000.0;
    const auto supplied = ailments.ApplyHitAilments(second, hit);
    POE_CHECK(supplied.appliedElectrocute && supplied.appliedHeavyStun && supplied.appliedPin);
    ailments.ClearAilment(first, AilmentKind::Freeze);
    POE_CHECK(!ailments.IsFrozen(first));
    POE_CHECK(!ailments.GetState(first)->flammability.IsEmpty());
    ailments.RemoveActor(first);
    POE_CHECK(ailments.GetState(first) == nullptr);
    ailments.ClearAll();
    POE_CHECK(ailments.ActiveActorIds().empty());
    POE_CHECK(!ailments.IsFrozen(second) && ailments.FlammabilityChancePercent(second) == 0);
    POE_CHECK(ailments.ApplyHitAilments(second, hit).appliedFreeze);
}

POE_TEST("P2b enemy rules: flammability hits between ticks keep distinct expiry times")
{
    AilmentSystem ailments;
    HitAilmentInput hit;
    hit.targetAilmentThresholdPoe = 1000.0;
    hit.fireHitPoe = 1000.0;
    hit.igniteRoll01 = 1.0;
    (void)ailments.ApplyHitAilments(1, hit, 0.05);
    (void)ailments.ApplyHitAilments(1, hit, 0.15);
    (void)ailments.TickActor(1, 8.0, false);
    POE_CHECK_NEAR(ailments.FlammabilityChancePercent(1), 40.0, 1e-9);

    // The first instance expired. The second instance and the current hit form 40%, below this roll.
    hit.igniteRoll01 = 0.5;
    POE_CHECK(!ailments.ApplyHitAilments(1, hit, 0.10).appliedIgnite);
    (void)ailments.TickActor(1, 0.16, false);
    POE_CHECK_NEAR(ailments.FlammabilityChancePercent(1), 20.0, 1e-9);
}

POE_TEST("P2b enemy rules: hits between ticks age buildup and start damage at impact")
{
    AilmentSystem ailments;
    HitAilmentInput hit;
    const auto& rules = ActiveRuleValues();
    const double coldForFullPoise = hit.targetPoiseThresholdPoe / rules.freeze.damageScale;
    hit.coldHitPoe = coldForFullPoise * 0.99;
    (void)ailments.ApplyHitAilments(1, hit);
    (void)ailments.TickActor(1, rules.poise.enemyDecayDelayMilliseconds / 1000.0, false);
    hit.coldHitPoe = coldForFullPoise * 0.011;
    POE_CHECK(!ailments.ApplyHitAilments(1, hit, 0.24).appliedFreeze);
    POE_CHECK_NEAR(ailments.GetState(1)->poise[static_cast<std::size_t>(PoiseKind::Freeze)].ratio, 0.989, 1e-9);

    hit.coldHitPoe = coldForFullPoise * 2.0;
    hit.fireHitPoe = 100.0;
    hit.igniteChancePct = 100.0;
    const auto result = ailments.ApplyHitAilments(2, hit, 0.24);
    POE_CHECK(result.appliedFreeze && result.appliedIgnite);
    const auto damage = ailments.TickActor(2, 0.25, false);
    POE_CHECK_NEAR(damage.firePoeDamage, 0.01 * 100.0 * rules.ignite.damagePercentPerMinute / 6000.0, 1e-9);
    POE_CHECK_NEAR(ailments.GetState(2)->poise[static_cast<std::size_t>(PoiseKind::Freeze)].activeRemaining,
        rules.freeze.enemyDurationSeconds - 0.01, 1e-9);
}

POE_TEST("P2b enemy rules: the killing cold hit makes a fresh victim eligible for Herald of Ice")
{
    AilmentSystem ailments;
    HitAilmentInput killingHit;
    killingHit.coldHitPoe = 50.0;
    killingHit.targetPoiseThresholdPoe = 1000.0;
    constexpr std::uint32_t victim = 1;
    POE_CHECK(!ailments.IsChilledOrFrozen(victim));
    const auto result = ailments.ApplyHitAilments(victim, killingHit);
    POE_CHECK(result.appliedChill && !result.appliedFreeze);
    POE_CHECK(ailments.IsChilledOrFrozen(victim));
    ailments.RemoveActor(victim);
    POE_CHECK(!ailments.IsChilledOrFrozen(victim));
}

POE_TEST("P2b enemy rules: a zero lightning roll cannot borrow cold damage")
{
    const auto cold = static_cast<std::size_t>(DamageType::Cold);
    const auto lightning = static_cast<std::size_t>(DamageType::Lightning);
    DamagePacket spell;
    spell.byType[cold] = { 100.0, 100.0 };
    spell.byType[lightning] = { 0.0, 200.0 };
    const auto rolled = RollSpellDamagePacket(spell, 0.0, 2.0);
    POE_CHECK(rolled.byType[cold].min == 200.0 && rolled.byType[cold].max == 200.0);
    POE_CHECK(rolled.byType[lightning].min == 0.0 && rolled.byType[lightning].max == 0.0);
    POE_CHECK(rolled.TotalAverage() == 200.0);

    HitAilmentInput hit;
    hit.coldHitPoe = rolled.byType[cold].min;
    hit.lightningHitPoe = rolled.byType[lightning].min;
    hit.targetPoiseThresholdPoe = 300.0;
    hit.shockChancePct = 100.0;
    AilmentSystem ailments;
    const auto result = ailments.ApplyHitAilments(1, hit);
    POE_CHECK(result.appliedFreeze && !result.appliedShock);
}
