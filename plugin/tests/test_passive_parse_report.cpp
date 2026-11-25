#include "test_harness.h"

#include "core/DamageConversion.h"
#include "core/PassiveStatParser.h"
#include "core/PassiveTree.h"
#include "core/StatEngine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>

namespace
{
    using namespace SkyrimPoE::Core;
    using Json = nlohmann::json;

    Json ParseDisplay(const std::vector<std::string>& lines, bool keystone, std::uint32_t generation,
        bool phylactery = false)
    {
        auto result = Json::array();
        bool allSupported = true;
        for (const auto& line : lines) {
            PassiveStatTotals stats;
            bool full = generation == 1
                ? PassiveStatParser::ParsePoe2Line(line, stats, keystone)
                : PassiveStatParser::ParseLine(line, stats);
            auto mapped = PassiveStatParser::Describe(stats);
            if (phylactery) {
                if (line == "Can Socket a non-Unique Basic Jewel into the Phylactery") {
                    mapped["nonUniqueBasicJewelSocket"] = 1.0;
                } else if (line == "100% increased Effect of bonuses gained from Socketed Jewel") {
                    mapped["socketedJewelEffectMultiplier"] = 2;
                } else if (line == "50% more Mana Cost of Skills if you have no Energy Shield") {
                    mapped["manaCostMultiplierWithoutEnergyShield"] = 1.5;
                }
                full = !mapped.empty();
            }
            const bool mappingOnly = std::ranges::any_of(mapped,
                [](const auto& stat) { return stat.first.starts_with("chargeGain."); });
            allSupported = allSupported && full;
            result.push_back({
                { "line", line }, { "status", mappingOnly ? "mappingOnly" : full ? "full" : "unsupported" },
                { "stats", std::move(mapped) }
            });
        }
        if (generation == 1 && keystone && !allSupported) {
            for (auto& line : result) {
                line["status"] = "unsupported";
                line["stats"] = Json::object();
            }
        }
        return result;
    }

    std::string ReadBytes(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        POE_CHECK(input.good());
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    void CollectDrawbacks(const Json& parsed, std::set<std::string>& unsupported,
        std::set<std::string>& implemented)
    {
        if (std::ranges::none_of(parsed, [](const auto& line) { return line.at("status") == "full"; })) {
            return;
        }
        for (const auto& line : parsed) {
            const auto& text = line.at("line").get_ref<const std::string&>();
            if (line.at("status") == "unsupported" && PassiveStatParser::IsPoe2Drawback(text)) {
                unsupported.insert(text);
            } else if (line.at("stats").contains("blockedHitDamageTakenPct") ||
                line.at("stats").contains("energyShieldBypassPct")) {
                implemented.insert(text);
            }
        }
    }

    std::string BuildReport(const std::filesystem::path& path, Json* mixed = nullptr)
    {
        const auto input = Json::parse(ReadBytes(path));
        const auto& raw = input.contains("tree") ? input.at("tree") : input;
        PassiveTreeData tree;
        std::string error;
        if (!tree.LoadFromJsonString(raw.dump(), &error)) throw std::runtime_error(error);
        auto nodes = Json::object();
        std::map<std::uint32_t, Json> mixedNodes;
        for (const auto& [id, node] : tree.AllNodes()) {
            auto result = Json::object();
            const bool keystone = node.kind == PassiveNodeKind::Keystone;
            const bool phylactery = PassiveTree::IsPhylactery(tree.Generation(), id);
            result["node"] = ParseDisplay(node.stats, keystone, tree.Generation(), phylactery);
            for (const auto& option : node.attrOptions) {
                result["attributeOptions"][std::to_string(option.id)] = ParseDisplay(option.stats, false, tree.Generation());
            }
            for (const auto& [key, variant] : node.variants) {
                result["variants"][key] = ParseDisplay(variant.stats, keystone, tree.Generation(), phylactery);
            }
            if (mixed && tree.Generation() == 1) {
                std::set<std::string> unsupported, implemented, unavailableReplacements;
                const auto collectReplacement = [&](std::span<const std::string> lines) {
                    const auto replacement = PassiveStatParser::UnimplementedPoe2ManaRegenReplacement(lines);
                    if (!replacement.empty()) unavailableReplacements.emplace(replacement);
                };
                CollectDrawbacks(result.at("node"), unsupported, implemented);
                collectReplacement(node.stats);
                for (const auto& option : node.attrOptions) {
                    CollectDrawbacks(result.at("attributeOptions").at(std::to_string(option.id)), unsupported, implemented);
                    collectReplacement(option.stats);
                }
                for (const auto& [key, variant] : node.variants) {
                    CollectDrawbacks(result.at("variants").at(key), unsupported, implemented);
                    collectReplacement(variant.stats);
                }
                if (!unsupported.empty() || !implemented.empty() || !unavailableReplacements.empty()) {
                    const bool deferred = !unsupported.empty() || !unavailableReplacements.empty();
                    Json entry{
                        { "id", id },
                        { "unsupportedDrawbacks", unsupported },
                        { "implementedDrawbacks", implemented },
                        { "treatment", deferred ? "deferred" : "implemented" }
                    };
                    if (!unavailableReplacements.empty()) entry["unsupportedManaRegenReplacement"] = unavailableReplacements;
                    mixedNodes.emplace(id, std::move(entry));
                }
            }
            nodes[std::to_string(id)] = std::move(result);
        }
        if (mixed) {
            auto entries = Json::array();
            for (auto& [id, entry] : mixedNodes) {
                entries.push_back(std::move(entry));
            }
            *mixed = { { "contentGeneration", tree.Generation() }, { "nodes", std::move(entries) } };
        }
        Json report{ { "contentGeneration", tree.Generation() }, { "nodes", std::move(nodes) } };
        if (tree.Generation() == 0) {
            auto masteries = Json::object();
            for (const auto& [key, _] : raw.at("masteryEffects").items()) {
                const auto id = static_cast<std::uint32_t>(std::stoul(key));
                const auto* effect = tree.FindMasteryEffect(id);
                POE_CHECK(effect);
                masteries[key] = ParseDisplay(effect->stats, false, tree.Generation());
            }
            report["masteries"] = std::move(masteries);
        }
        return report.dump() + "\n";
    }
}

int WritePassiveParseReport(const std::filesystem::path& tree, const std::filesystem::path& output,
    const std::filesystem::path& mixedOutput)
{
    try {
        Json mixed;
        const auto report = BuildReport(tree, mixedOutput.empty() ? nullptr : &mixed);
        const auto write = [](const std::filesystem::path& path, const std::string& contents) {
            std::ofstream stream(path, std::ios::binary);
            stream.exceptions(std::ios::failbit | std::ios::badbit);
            stream << contents;
            stream.close();
        };
        write(output, report);
        if (!mixedOutput.empty()) write(mixedOutput, mixed.dump(2) + "\n");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

POE_TEST("PoE2 passive parser report is reproducible byte for byte")
{
    const std::filesystem::path data(POE_TEST_DATA_DIR);
    const auto actual = BuildReport(data / "packs" / "gen1" / "tree.json");
    const auto expected = ReadBytes(data / "poe2" / "reports" / "tree-parse.json");
    POE_CHECK(actual == expected);
}

POE_TEST("PoE2 mixed node report matches the parsed drawbacks")
{
    const std::filesystem::path data(POE_TEST_DATA_DIR);
    Json mixed;
    (void)BuildReport(data / "packs/gen1/tree.json", &mixed);
    POE_CHECK(mixed.dump(2) + "\n" == ReadBytes(data / "poe2/reports/tree-mixed-nodes.json"));
}

POE_TEST("PoE2 withholds mixed node benefits when a required drawback is unsupported")
{
    const std::filesystem::path data(POE_TEST_DATA_DIR);
    PassiveTreeData tree;
    std::string error;
    POE_CHECK(tree.LoadFromFile(data / "packs/gen1/tree.json", &error));
    PassiveTreeState state;
    state.classSelected = true;
    state.generation = 1;

    state.allocatedNodes = { 52191 };
    const auto deferred = PassiveStatParser::AggregateTree(tree, state);
    POE_CHECK(deferred.incChaosDamagePct == 0.0);
    POE_CHECK(deferred.parsedStatLines == 0);
    POE_CHECK(deferred.unparsedStatLines == 2);

    state.allocatedNodes = { 52068 };
    const auto implemented = PassiveStatParser::AggregateTree(tree, state);
    POE_CHECK(implemented.baseBlockCapOverridePct == 75.0);
    POE_CHECK(implemented.blockedHitDamageTakenPct == 20.0);
    StatSnapshot snapshot;
    snapshot.passiveAilments = implemented;
    POE_CHECK_NEAR(BlockedHitDamageFraction(snapshot), 0.20, 1e-8);

    state.allocatedNodes = { 27491 };
    snapshot.passiveAilments = PassiveStatParser::AggregateTree(tree, state);
    POE_CHECK_NEAR(snapshot.passiveAilments.incEnergyShieldPct, 40.0, 1e-8);
    const double bypass = PlayerDamageBypassingEnergyShield(100.0, snapshot);
    const auto absorbed = AbsorbWithEnergyShield(100.0 - bypass, 1.0, false, 100.0);
    POE_CHECK_NEAR(bypass + absorbed.remainingRawDamage, 5.0, 1e-8);
    POE_CHECK_NEAR(absorbed.newEnergyShield, 5.0, 1e-8);

    state.allocatedNodes = { 33570 };
    const auto soullessForm = PassiveStatParser::AggregateTree(tree, state);
    POE_CHECK(!soullessForm.noInherentManaRegen);
    POE_CHECK_NEAR(soullessForm.energyShieldBypassPct, 0.0, 1e-8);

    state.allocatedNodes = { 14265 };
    const auto benign = PassiveStatParser::AggregateTree(tree, state);
    POE_CHECK(benign.incFireDamagePct == 20.0);
}

POE_TEST("PoE2 cost efficiency preserves its resource and skill selectors")
{
    PassiveStatTotals stats;
    POE_CHECK(PassiveStatParser::ParsePoe2Line("15% increased Cost Efficiency of Attacks", stats));
    POE_CHECK(PassiveStatParser::ParsePoe2Line("4% increased Cost Efficiency", stats));
    POE_CHECK(PassiveStatParser::ParsePoe2Line("7% increased Mana Cost Efficiency", stats));
    POE_CHECK(PassiveStatParser::ParsePoe2Line("8% increased Life Cost Efficiency", stats));
    POE_CHECK(PassiveStatParser::ParsePoe2Line("9% reduced Mana Cost Efficiency of Attacks", stats));
    POE_CHECK(PassiveStatParser::ParsePoe2Line("10% increased Mana Cost Efficiency of Spells", stats));
    POE_CHECK(stats.attackCostEfficiencyPct == 15.0);
    POE_CHECK(stats.costEfficiencyPct == 4.0);
    POE_CHECK(stats.manaCostEfficiencyPct == 7.0);
    POE_CHECK(stats.lifeCostEfficiencyPct == 8.0);
    POE_CHECK(stats.manaAttackCostEfficiencyPct == -9.0);
    POE_CHECK(stats.manaSpellCostEfficiencyPct == 10.0);
}

POE_TEST("Generation 0 keeps the legacy parser results across all node and mastery lines")
{
    const auto report = BuildReport(std::filesystem::path(POE_TEST_DATA_DIR) / "packs/tree_3_30.json");
    // FNV-1a over 4,339 stat occurrences using the unchanged generation-zero grammar.
    std::uint64_t digest = 14695981039346656037ULL;
    for (const unsigned char byte : report) {
        digest ^= byte;
        digest *= 1099511628211ULL;
    }
    POE_CHECK(digest == 0xd782be41cf7d5d57ULL);
}

POE_TEST("Passive parser accepts the correct generation, spelling and units")
{
    struct Case
    {
        std::string_view line;
        bool generation0;
        bool generation1;
    };
    constexpr Case cases[]{
        { " +5 to Strength  ", true, true },
        { "+5% to Strength", false, false },
        { "12% reduced maximum Mana", true, true },
        { "+2% to maximum Chance to Block Spell Damage", true, true },
        { "Attacks have 15% chance to cause Bleeding", true, true },
        { "Attacks have 15 chance to cause Bleeding", false, false },
        { "10% increased Damage while holding a Shield", false, false },
        { "10% increased Critical Hit Chance", false, true },
        { "30% more Spell Damage when on Low Life", true, false },
        { "25% more Spell Damage when on Low Life", false, false },
        { "Grants 1 Passive Skill Point", false, false },
        { "+3.5%chance to Ignite", true, true },
    };
    for (const auto& [line, generation0, generation1] : cases) {
        PassiveStatTotals legacy;
        PassiveStatTotals current;
        POE_CHECK(PassiveStatParser::ParseLine(line, legacy) == generation0);
        POE_CHECK(PassiveStatParser::ParsePoe2Line(line, current) == generation1);
    }
}
