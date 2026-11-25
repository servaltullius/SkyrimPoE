#include "test_harness.h"

#include "core/EnergyShield.h"
#include "core/PassiveTree.h"
#include "core/SaveAffixList.h"
#include "core/SaveRecordFormat.h"
#include "core/StatEngine.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

namespace
{
    using namespace SkyrimPoE::Core;
    using namespace SkyrimPoE::Core::SaveRecord;

    const auto kPackDir = std::filesystem::path(POE_TEST_DATA_DIR) / "packs";
    const SaveGeneration kGen0{ "poe1", 0, "3.30", "1234567890abcdef", "0.1.0" };
    const SaveGeneration kGen1{ "poe2", 1, "0.5", "1234567890abcdef", "0.1.0" };

    PassiveTreeData LoadTree(const std::filesystem::path& path)
    {
        PassiveTreeData tree;
        POE_CHECK(tree.LoadFromFile(kPackDir / path));
        return tree;
    }

    const PassiveTreeData& PoE2Tree()
    {
        static const auto tree = LoadTree("gen1/tree.json");
        return tree;
    }

    PassiveTreeState StartAs(std::string_view classId)
    {
        PassiveTreeState state;
        state.ascendancyMilestonesBitmask = 0xF;
        POE_CHECK(PassiveTree::SelectClass(PoE2Tree(), state, classId).ok);
        return state;
    }

    void AllocateTo(PassiveTreeState& state, std::uint32_t target, const Config& config = {})
    {
        const auto& tree = PoE2Tree();
        const auto path = PassiveTree::ComputeShortestPath(tree, state, target);
        POE_CHECK(!path.empty());
        std::unordered_map<std::uint32_t, std::uint32_t> choices;
        for (const auto id : path) {
            const auto* node = tree.FindNode(id);
            if (!node->attrOptions.empty()) {
                choices.emplace(id, node->attrOptions.front().id);
            }
        }
        POE_CHECK(PassiveTree::AllocatePath(tree, state, path, 100, config, 0xFFFF, choices).ok);
    }

    SaveRecordBatch EncodeTree(const PassiveTreeState& tree, const SaveGeneration& generation)
    {
        Snapshot snapshot;
        snapshot.state.tree = tree;
        SaveRecordBatch batch;
        POE_CHECK(WriteSnapshot(snapshot, generation, [&](RawSaveRecord&& record) {
            batch.records.push_back(std::move(record));
            return true;
        }));
        return batch;
    }

    const RawSaveRecord& TreeRecord(const SaveRecordBatch& batch)
    {
        const auto record = std::ranges::find(batch.records, kRecordTree, &RawSaveRecord::type);
        POE_CHECK(record != batch.records.end());
        return *record;
    }

    RawSaveRecord LegacyRangerTree()
    {
        RawSaveRecord record{ kRecordTree, kLegacyTreeVersion, { 1, 2 } };
        const auto append = [&](const void* data, std::uint32_t size) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            record.bytes.insert(record.bytes.end(), bytes, bytes + size);
            return true;
        };
        POE_CHECK(WriteSaveString(append, "Raider"));
        const std::uint32_t body[]{ 1, 39821, 0, 4, 5, 0xF3 };  // node count, node, mastery count, kills, milestones
        append(body, sizeof(body));
        return record;
    }
}

POE_TEST("PoE2 tree loads 8 classes and 22 ascendancies with independent classes sharing starts")
{
    const auto& tree = PoE2Tree();
    POE_CHECK(tree.Generation() == 1);
    POE_CHECK(tree.Classes().size() == 8);
    POE_CHECK(tree.MasteryEffectCount() == 0);
    std::size_t ascendancies = 0;
    for (const auto& cls : tree.Classes()) {
        POE_CHECK(tree.FindClassByContentId(cls.contentId) == &cls);
        ascendancies += cls.ascendancies.size();
    }
    POE_CHECK(ascendancies == 22);
    for (const auto& [first, second] : { std::pair{ "cls.Ranger", "cls.Huntress" },
             std::pair{ "cls.Witch", "cls.Sorceress" } }) {
        const auto* a = tree.FindClassByContentId(first);
        const auto* b = tree.FindClassByContentId(second);
        POE_CHECK(a && b && a->id != b->id);
        POE_CHECK(a->startNodeId == b->startNodeId);
        POE_CHECK(a->startNeighbors == b->startNeighbors);
        POE_CHECK(!a->startNeighbors.empty());
        auto state = StartAs(first);
        POE_CHECK(PassiveTree::SelectClass(tree, state, second).ok);
        const auto path = PassiveTree::ComputeShortestPath(tree, state, b->startNeighbors.front());
        POE_CHECK(path == std::vector<std::uint32_t>{ a->startNeighbors.front() });
        POE_CHECK(!PassiveTree::AllocatePath(tree, state, { a->startNodeId }, 100, {}).ok);
        const auto* monk = tree.FindClassByContentId("cls.Monk");
        POE_CHECK(monk);
        POE_CHECK(PassiveTree::ComputeShortestPath(tree, state, monk->startNodeId).empty());
        POE_CHECK(!PassiveTree::AllocatePath(tree, state, { monk->startNodeId }, 100, {}).ok);
    }
    auto monk = StartAs("cls.Monk");
    POE_CHECK(monk.classId == 7);
    POE_CHECK(PassiveTree::ComputeAvailableNormalPoints(tree, monk, 100, {}, 0xFFFF) == 123);
    POE_CHECK(PassiveTree::ComputeEarnedAscendancyPoints(monk, {}) == 8);
}

POE_TEST("Tree loader rejects invalid node kinds, duplicate class indices, and wrapped class indices")
{
    PassiveTreeData tree;
    POE_CHECK(!tree.LoadFromJsonString(R"({"classes":[{"id":0,"name":"Test"}],"nodes":[{"id":1,"k":256}]})"));
    POE_CHECK(!tree.LoadFromJsonString(R"({"classes":[{"id":0,"name":"Test"},{"id":0,"name":"Other"}],"nodes":[{"id":1,"k":0}]})"));
    POE_CHECK(!tree.LoadFromJsonString(R"({"classes":[{"id":256,"name":"Test"}],"nodes":[{"id":1,"k":0}]})"));
}

POE_TEST("PoE2 path allocation requires every new attribute option and swaps it without breaking the path")
{
    const auto& tree = PoE2Tree();
    auto state = StartAs("cls.Ranger");
    Config config;
    config.freePassiveRefund = false;
    const auto path = PassiveTree::ComputeShortestPath(tree, state, 1140);
    POE_CHECK((path == std::vector<std::uint32_t>{ 13828, 1140 }));
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, path, 100, config).ok);
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, path, 100, config, 0, { { 1140, 1 } }).ok);
    POE_CHECK(state.allocatedNodes.empty() && state.attributeSelections.empty());

    const auto* node = tree.FindNode(1140);
    POE_CHECK(node && node->kind == PassiveNodeKind::Attribute && node->attrOptions.size() == 3);
    const auto& strength = node->attrOptions[0];
    const auto& dexterity = node->attrOptions[1];
    const auto& intelligence = node->attrOptions[2];
    POE_CHECK(strength.name == "Strength" && dexterity.name == "Dexterity" && intelligence.name == "Intelligence");
    POE_CHECK(PassiveTree::AllocatePath(tree, state, path, 100, config, 0, { { 1140, strength.id } }).ok);
    AllocateTo(state, 15507, config);
    POE_CHECK(PassiveTree::ResolveNode(*node, state).stats.front() == "+5 to Strength");
    POE_CHECK(!PassiveTree::ChangeAttribute(tree, state, 1140, dexterity.id, config).ok);
    POE_CHECK(state.attributeSelections.at(1140) == strength.id);
    POE_CHECK(PassiveTree::ChangeAttribute(tree, state, 1140, strength.id, config).regretOrbsConsumed == 0);
    const auto swap = PassiveTree::ChangeAttribute(tree, state, 1140, dexterity.id, config, 1);
    POE_CHECK(swap.ok && swap.regretOrbsConsumed == 1);
    POE_CHECK(PassiveTree::ResolveNode(*node, state).stats.front() == "+5 to Dexterity");
    POE_CHECK(state.IsAllocated(15507));
    POE_CHECK(!PassiveTree::RefundNode(tree, state, 1140, 100, config, 1).ok);
    POE_CHECK(state.attributeSelections.at(1140) == dexterity.id);

    POE_CHECK(PassiveTree::RefundNode(tree, state, 15507, 100, config, 1).ok);
    POE_CHECK(PassiveTree::GetRefundNodeCost(tree, state, 1140, config) == 1);
    const auto refund = PassiveTree::RefundNode(tree, state, 1140, 100, config, 1);
    POE_CHECK(refund.ok && refund.regretOrbsConsumed == 1 && !state.attributeSelections.contains(1140));
}

POE_TEST("Oracle 5571 gates hidden paths, blocks dependent refunds, and is checked during sanitation")
{
    const auto& tree = PoE2Tree();
    auto state = StartAs("cls.Druid");
    Config config;
    config.freePassiveRefund = false;
    config.ascendancyRefundCost = 5;
    POE_CHECK(PassiveTree::SelectAscendancy(tree, state, "asc.Oracle", config).ok);
    const auto* hidden = tree.FindNode(479);
    POE_CHECK(hidden && hidden->unlock == std::vector<std::uint32_t>{ 5571 });
    POE_CHECK(PassiveTree::ComputeShortestPath(tree, state, 479).empty());
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, { 479 }, 100, config).ok);

    AllocateTo(state, 5571, config);
    AllocateTo(state, 479, config);
    POE_CHECK(state.IsAllocated(479));
    POE_CHECK(!PassiveTree::RefundNode(tree, state, 5571, 100, config, 5, 0xFFFF).ok);
    POE_CHECK(!PassiveTree::RefundAll(tree, state, true, 100, config, 10, 0xFFFF).ok);
    POE_CHECK(state.IsAllocated(5571) && state.IsAllocated(479));

    auto corrupt = state;
    std::erase(corrupt.allocatedNodes, 11335);  // Remove the only ascendant path from the start to 5571.
    POE_CHECK(PassiveTree::SanitizeLoadedState(tree, corrupt));
    POE_CHECK(!corrupt.IsAllocated(5571) && !corrupt.IsAllocated(479));
    POE_CHECK(std::ranges::none_of(corrupt.allocatedNodes, [&](auto id) {
        return !tree.FindNode(id)->unlock.empty();
    }));
    POE_CHECK(corrupt.attributeSelections.size() <= state.attributeSelections.size());

    POE_CHECK(PassiveTree::RefundNode(tree, state, 479, 100, config, 1, 0xFFFF).ok);
    for (const auto id : { 28770u, 29126u }) {
        if (state.IsAllocated(id)) {
            POE_CHECK(PassiveTree::RefundNode(tree, state, id, 100, config, 1, 0xFFFF).ok);
        }
    }
    POE_CHECK(PassiveTree::GetRefundNodeCost(tree, state, 5571, config) == 5);
    POE_CHECK(!PassiveTree::RefundNode(tree, state, 5571, 100, config, 4, 0xFFFF).ok);
    POE_CHECK(PassiveTree::RefundNode(tree, state, 5571, 100, config, 5, 0xFFFF).regretOrbsConsumed == 5);
}

POE_TEST("Free ascendancy nodes stay on a 0-1 BFS path and cost no points or refund currency")
{
    const auto& tree = PoE2Tree();
    auto state = StartAs("cls.Witch");
    Config config;
    config.freePassiveRefund = false;
    state.ascendancyMilestonesBitmask = 1;
    POE_CHECK(PassiveTree::SelectAscendancy(tree, state, "asc.BloodMage", config).ok);
    const auto path = PassiveTree::ComputeShortestPath(tree, state, 3165);
    POE_CHECK((path == std::vector<std::uint32_t>{ 8415, 3165 }));
    POE_CHECK(PassiveTree::AllocatePath(tree, state, path, 1, config).ok);
    POE_CHECK(tree.FindNode(8415)->freeAlloc && state.IsAllocated(8415));
    POE_CHECK(PassiveTree::ComputeSpentAscendancyPoints(tree, state) == 1);
    POE_CHECK(!PassiveTree::RefundNode(tree, state, 8415, 1, config).ok);
    POE_CHECK(PassiveTree::RefundNode(tree, state, 3165, 1, config, config.ascendancyRefundCost).ok);
    POE_CHECK(!PassiveTree::ClearAscendancy(tree, state).ok);
    const auto refund = PassiveTree::RefundNode(tree, state, 8415, 1, config);
    POE_CHECK(refund.ok && refund.regretOrbsConsumed == 0);
    POE_CHECK(PassiveTree::ComputeAvailableAscendancyPoints(tree, state, config) == 2);
}

POE_TEST("PoE2 resolves class text and effective ascendancy for Lich replacements and their jewel socket")
{
    const auto& tree = PoE2Tree();
    const auto* node = tree.FindNode(56651);
    POE_CHECK(node);
    const auto ranger = PassiveTree::ResolveNode(*node, StartAs("cls.Ranger"));
    const auto huntress = PassiveTree::ResolveNode(*node, StartAs("cls.Huntress"));
    POE_CHECK(ranger.name == "Projectile Damage" && huntress.name == "Attack Damage");
    POE_CHECK(ranger.stats.front() == "10% increased Projectile Damage");
    POE_CHECK(huntress.stats.front() == "10% increased Attack Damage");

    auto state = StartAs("cls.Witch");
    const auto* base = tree.FindAscendancy("asc.Lich");
    const auto* replacement = tree.FindAscendancy("asc.AbyssalLich");
    POE_CHECK(base && replacement && base->startNodeId == replacement->startNodeId);
    POE_CHECK(base->replacements.size() == 1 && &base->replacements.front() == replacement);
    const auto* branch = tree.FindNode(51142);
    POE_CHECK(branch);
    Config config;
    config.freePassiveRefund = true;
    POE_CHECK(PassiveTree::SelectAscendancy(tree, state, "asc.Lich", config).ok);
    POE_CHECK(PassiveTree::ResolveNode(*branch, state).name == "Mana");
    AllocateTo(state, 17788, config);
    POE_CHECK(tree.FindNode(17788)->kind == PassiveNodeKind::Notable);
    POE_CHECK(PassiveTree::IsAllocatedJewelSocket(tree, state, 17788));
    POE_CHECK(!PassiveTree::SelectAscendancy(tree, state, "asc.AbyssalLich", config).ok);
    POE_CHECK(PassiveTree::RefundAll(tree, state, true, 100, config).ok);
    POE_CHECK(!PassiveTree::IsAllocatedJewelSocket(tree, state, 17788));

    POE_CHECK(PassiveTree::SelectAscendancy(tree, state, "asc.AbyssalLich", config).ok);
    const auto replaced = PassiveTree::ResolveNode(*branch, state);
    POE_CHECK(replaced.name == "Minion Reservation Efficiency" && replaced.ascendancyId == "asc.AbyssalLich");
    AllocateTo(state, 17788, config);
    POE_CHECK(PassiveTree::IsAllocatedJewelSocket(tree, state, 17788));
    std::erase(state.allocatedNodes, 58751);
    POE_CHECK(PassiveTree::SanitizeLoadedState(tree, state));
    POE_CHECK(!PassiveTree::IsAllocatedJewelSocket(tree, state, 17788));

    const auto* alias = tree.FindNode(3367);
    POE_CHECK(alias && alias->jewelSocket && alias->alias == "voices_jewel_slot5" && alias->adjacency.empty());
    POE_CHECK(PassiveTree::ComputeShortestPath(tree, state, 3367).empty());
    POE_CHECK(!PassiveTree::AllocatePath(tree, state, { 3367 }, 100, config).ok);
    state.allocatedNodes.push_back(3367);
    POE_CHECK(!PassiveTree::IsAllocatedJewelSocket(tree, state, 3367));
    POE_CHECK(PassiveTree::SanitizeLoadedState(tree, state));
    POE_CHECK(!state.IsAllocated(3367));
}

POE_TEST("TREE v2 round trips class and ascendancy IDs, attribute choices, allocations, and kill milestones")
{
    const auto& tree = PoE2Tree();
    auto state = StartAs("cls.Huntress");
    state.bossKillCount = 29;
    state.dragonKillCount = 6;
    POE_CHECK(PassiveTree::SelectAscendancy(tree, state, "asc.Amazon", {}).ok);
    AllocateTo(state, 1140);
    const auto batch = EncodeTree(state, kGen1);
    const auto& record = TreeRecord(batch);
    POE_CHECK(record.version == 2);
    POE_CHECK(DecideSave(batch, false, kGen1, tree.Generation()).disposition == SaveDisposition::Normal);
    PassiveTreeState restored;
    POE_CHECK(ReadTreeRecord(record, restored));
    POE_CHECK(restored.classContentId == "cls.Huntress" && restored.ascendancyId == "asc.Amazon");
    POE_CHECK(PassiveTree::SanitizeLoadedState(tree, restored));
    POE_CHECK(restored.generation == 1 && restored.classSelected && restored.classId == state.classId);
    POE_CHECK(restored.allocatedNodes == state.allocatedNodes);
    POE_CHECK(restored.attributeSelections == state.attributeSelections);
    POE_CHECK(restored.bossKillCount == 29 && restored.dragonKillCount == 6 &&
        restored.ascendancyMilestonesBitmask == 0xF);
    auto truncated = record;
    truncated.bytes.pop_back();
    POE_CHECK(!ReadTreeRecord(truncated, restored));
    POE_CHECK(restored.attributeSelections == state.attributeSelections);
    restored.attributeSelections[1140] = 0;
    POE_CHECK(PassiveTree::SanitizeLoadedState(tree, restored));
    POE_CHECK(!restored.IsAllocated(1140) && restored.attributeSelections.empty());
}

POE_TEST("TREE v1 numeric class resolves through the real PoE1 pack and is saved as content IDs")
{
    const auto tree = LoadTree("tree_3_30.json");
    PassiveTreeState state;
    POE_CHECK(ReadTreeRecord(LegacyRangerTree(), state));
    POE_CHECK(state.generation == 0 && state.classSelected && state.classId == 2);
    POE_CHECK(PassiveTree::SanitizeLoadedState(tree, state));
    POE_CHECK(state.classContentId == "cls.Ranger" && state.ascendancyId == "Raider" && state.IsAllocated(39821));
    POE_CHECK(state.bossKillCount == 4 && state.dragonKillCount == 5 && state.ascendancyMilestonesBitmask == 3);
    PassiveTreeState saved;
    const auto batch = EncodeTree(state, kGen0);
    POE_CHECK(TreeRecord(batch).version == kTreeVersion && ReadTreeRecord(TreeRecord(batch), saved));
    POE_CHECK(saved.classContentId == "cls.Ranger" && saved.ascendancyId == "asc.Raider");
    POE_CHECK(PassiveTree::SanitizeLoadedState(tree, saved) && saved.ascendancyId == state.ascendancyId);
}

POE_TEST("Mismatched TREE generation preserves raw records and leaves allocations uninterpreted")
{
    const auto& tree = PoE2Tree();
    auto state = StartAs("cls.Ranger");
    AllocateTo(state, 1140);
    const auto batch = EncodeTree(state, kGen0);  // GENR matches, yet TREE identifies generation 1 nodes.
    const auto decision = DecideSave(batch, false, kGen0, 0);
    POE_CHECK(decision.disposition == SaveDisposition::Preserve);
    POE_CHECK(decision.reason.find("TREE") != std::string_view::npos);

    PassiveTreeState restored;
    POE_CHECK(ReadTreeRecord(TreeRecord(batch), restored));
    POE_CHECK(!PassiveTree::SanitizeLoadedState(LoadTree("tree_3_30.json"), restored));
    POE_CHECK(restored.allocatedNodes == state.allocatedNodes && restored.attributeSelections == state.attributeSelections);
    POE_CHECK(ReplaySaveRecords(batch, [&](const RawSaveRecord& record) {
        if (record.type == kRecordTree) {
            POE_CHECK(record.version == TreeRecord(batch).version && record.bytes == TreeRecord(batch).bytes);
        }
        return true;
    }));

    SaveRecordBatch oldTree{ { LegacyRangerTree(), { kRecordGeneration, kGenerationRecordVersion,
        WriteGenerationRecord(kGen1) } } };
    POE_CHECK(DecideSave(oldTree, false, kGen1, tree.Generation()).disposition == SaveDisposition::Preserve);
}

POE_TEST("PoE2 Phylactery doubles socketed strength and updates mana cost when energy shield runs out")
{
    const auto& tree = PoE2Tree();
    Config config;
    for (const auto ascendancy : { "asc.Lich", "asc.AbyssalLich" }) {
        auto state = StartAs("cls.Witch");
        POE_CHECK(PassiveTree::SelectAscendancy(tree, state, ascendancy, config).ok);
        POE_CHECK(PassiveTree::JewelEffectCopies(tree, state, PassiveTree::kPhylacteryNodeId) == 0);
        AllocateTo(state, PassiveTree::kPhylacteryNodeId, config);

        DataPack pack;
        POE_CHECK(pack.LoadFromFile(kPackDir / "core_3_30.json"));
        StatEngine engine;
        Layer1Input input;
        input.passives.poe2 = true;
        input.flatEnergyShield = 50.0;
        input.slots[0].occupied = true;
        input.slots[0].gem.gemId = "Fireball";
        input.slots[0].gem.level = 1;
        engine.SetLayer1(input);
        POE_CHECK(engine.RebuildIfDirty(pack, config));
        const auto baseline = engine.GetSnapshot();
        const auto baseMana = baseline->slotStats[0].poeManaCost;
        POE_CHECK(baseMana > 0.0);

        EnergyShieldPool shield;
        shield.ResetFull(50.0);
        input.phylacteryEnergyShield = &shield;
        ItemStatTotals jewel;
        jewel.flatStr = 10.0;
        input.socketedJewelStats.assign(PassiveTree::JewelEffectCopies(tree, state, PassiveTree::kPhylacteryNodeId), jewel);
        engine.SetLayer1(input);
        POE_CHECK(engine.RebuildIfDirty(pack, config));
        POE_CHECK(engine.GetSnapshot()->str == baseline->str + 20);
        POE_CHECK(engine.GetSnapshot()->slotStats[0].poeManaCost == baseMana);

        (void)shield.AbsorbDamage(50.0, 1.0, false);
        POE_CHECK(engine.RebuildIfDirty(pack, config));
        POE_CHECK(engine.GetSnapshot()->slotStats[0].poeManaCost == std::floor(baseMana * 1.5));
        POE_CHECK(!engine.RebuildIfDirty(pack, config));
        shield.Recover(1.0);
        POE_CHECK(engine.RebuildIfDirty(pack, config));
        POE_CHECK(engine.GetSnapshot()->slotStats[0].poeManaCost == baseMana);

        POE_CHECK(ComputePoe2SkillCosts(20.0, 0.0, input.passives, false, true, false, true).mana == 30.0);
    }
}

POE_TEST("Tree class stats override the core pack for actual and expected life and mana")
{
    const auto& tree = PoE2Tree();
    const auto* cls = tree.FindClassByContentId("cls.Witch");
    POE_CHECK(cls);
    DataPack pack;
    POE_CHECK(pack.LoadFromFile(kPackDir / "core_3_30.json"));
    const auto* coreWitch = pack.FindClass("Witch");
    POE_CHECK(coreWitch && coreWitch->baseInt != cls->baseInt);

    Config config;
    StatEngine engine;
    Layer1Input input;
    POE_CHECK(input.className.empty() && !input.treeClass && !SkyrimPoE::Runtime::PersistentState{}.tree.classSelected);
    input.treeClass = ClassBase{ cls->name, cls->baseStr, cls->baseDex, cls->baseInt };
    engine.SetLayer1(input);
    POE_CHECK(engine.RebuildIfDirty(pack, config));
    const auto snap = engine.GetSnapshot();
    POE_CHECK(snap->str == cls->baseStr && snap->dex == cls->baseDex && snap->intel == cls->baseInt);
    POE_CHECK_NEAR(snap->poeMaxLife, snap->expectedPoeLife, 1e-9);
    POE_CHECK_NEAR(snap->poeMaxMana, snap->expectedPoeMana, 1e-9);

    const auto gen0 = LoadTree("tree_3_30.json");
    for (const auto& oldClass : gen0.Classes()) {
        const auto* coreClass = pack.FindClass(oldClass.name);
        POE_CHECK(coreClass && coreClass->baseStr == oldClass.baseStr && coreClass->baseDex == oldClass.baseDex &&
            coreClass->baseInt == oldClass.baseInt);
    }
}

namespace
{
    using nlohmann::json;
    constexpr std::uint32_t kFixtureLevel = 67;
    constexpr std::uint16_t kFixtureAtlas = 0x8421;

    json UiTreeState(const PassiveTreeData& tree, const PassiveTreeState& state, const Config& config)
    {
        json attributes = json::object();
        for (const auto& [id, choice] : state.attributeSelections) attributes[std::to_string(id)] = choice;
        json masteries = json::object();
        for (const auto& [id, effect] : state.masterySelections) masteries[std::to_string(id)] = effect;
        return {
            { "generation", state.generation }, { "classSelected", state.classSelected },
            { "classId", state.classId }, { "classContentId", state.classContentId },
            { "ascendancyId", state.ascendancyId }, { "allocatedNodes", state.allocatedNodes },
            { "attributeSelections", attributes }, { "masterySelections", masteries },
            { "ascendancyMilestonesBitmask", state.ascendancyMilestonesBitmask },
            { "availableNormalPoints", PassiveTree::ComputeAvailableNormalPoints(tree, state, kFixtureLevel, config, kFixtureAtlas) },
            { "availableAscendancyPoints", PassiveTree::ComputeAvailableAscendancyPoints(tree, state, config) },
            { "spentNormalPoints", PassiveTree::ComputeSpentNormalPoints(tree, state) },
            { "spentAscendancyPoints", PassiveTree::ComputeSpentAscendancyPoints(tree, state) },
            { "grantedPassivePoints", PassiveTree::ComputeGrantedPassivePoints(tree, state) },
            { "earnedAscendancyPoints", PassiveTree::ComputeEarnedAscendancyPoints(state, config) },
        };
    }

    json BuildPassiveUiFixture()
    {
        auto cases = json::array();
        Config config;
        config.freePassiveRefund = false;
        config.ascendancyRefundCost = 7;
        config.bonusPassivePoints = 3;
        config.bonusAscendancyPoints = 2;
        constexpr std::uint32_t orbs = 100;
        const auto probe = [&](std::string_view name, const PassiveTreeData& tree, const PassiveTreeState& state,
                               std::uint32_t target, std::uint32_t refundNode, std::uint32_t mastery = 0,
                               bool checkTiedClassStarts = false) {
            const auto evaluatePath = [&](const std::vector<std::uint32_t>& path) {
                std::unordered_map<std::uint32_t, std::uint32_t> choices;
                auto choicePairs = json::array();
                for (const auto id : path) {
                    const auto* node = tree.FindNode(id);
                    if (node->attrOptions.empty()) continue;
                    const auto option = node->attrOptions.front().id;
                    choices.emplace(id, option);
                    choicePairs.push_back({ id, option });
                }
                auto allocated = state;
                const auto result = PassiveTree::AllocatePath(tree, allocated, path, kFixtureLevel, config, kFixtureAtlas, choices);
                json candidate = { { "path", path }, { "choices", choicePairs },
                    { "allocation", result.ok ? UiTreeState(tree, allocated, config) : json(nullptr) } };
                return std::pair{ std::move(candidate), std::move(allocated) };
            };
            const auto path = PassiveTree::ComputeShortestPath(tree, state, target);
            auto [primary, allocated] = evaluatePath(path);
            auto candidates = json::array({ primary });
            if (checkTiedClassStarts) {
                POE_CHECK(!primary.at("allocation").is_null() && !path.empty());
                const auto spent = PassiveTree::ComputeSpentNormalPoints(tree, allocated);
                for (const auto start : tree.FindClass(state.classId)->startNeighbors) {
                    if (start == path.front()) continue;
                    const auto [startResult, seeded] = evaluatePath({ start });
                    if (startResult.at("allocation").is_null()) continue;
                    auto branch = PassiveTree::ComputeShortestPath(tree, seeded, target);
                    if (branch.empty()) continue;
                    branch.insert(branch.begin(), start);
                    auto [candidate, branchState] = evaluatePath(branch);
                    if (!candidate.at("allocation").is_null() &&
                        PassiveTree::ComputeSpentNormalPoints(tree, branchState) == spent) {
                        candidates.push_back(std::move(candidate));
                    }
                }
                // 두 시작 가지가 같은 비용인 경우 C++과 JS의 시드 순회 순서가 경로를 가른다.
                POE_CHECK(candidates.size() > 1);
            }
            auto refunded = state;
            const auto refund = PassiveTree::RefundNode(tree, refunded, refundNode, kFixtureLevel, config, orbs, kFixtureAtlas);
            auto refunds = json::array();
            for (const bool ascendancyOnly : { true, false }) {
                auto afterRefund = state;
                const auto result = PassiveTree::RefundAll(tree, afterRefund, ascendancyOnly,
                    kFixtureLevel, config, orbs, kFixtureAtlas);
                refunds.push_back({
                    { "ascendancyOnly", ascendancyOnly },
                    { "quote", PassiveTree::GetRefundAllCost(tree, state, ascendancyOnly, config) },
                    { "cost", result.regretOrbsConsumed },
                    { "state", result.ok ? UiTreeState(tree, afterRefund, config) : json(nullptr) },
                });
            }
            const auto* node = tree.FindNode(target);
            POE_CHECK(node);
            const auto view = PassiveTree::ResolveNode(*node, state);
            auto attributeChanges = json::array();
            if (state.IsAllocated(target) && !node->attrOptions.empty()) {
                for (const auto& option : node->attrOptions) {
                    auto changed = state;
                    const auto change = PassiveTree::ChangeAttribute(tree, changed, target, option.id, config, orbs);
                    attributeChanges.push_back({ { "option", option.id }, { "cost", change.regretOrbsConsumed },
                        { "state", change.ok ? UiTreeState(tree, changed, config) : json(nullptr) } });
                }
            }
            cases.push_back({
                { "name", name }, { "target", target }, { "refundNode", refundNode }, { "mastery", mastery },
                { "packGeneration", tree.Generation() },
                { "points", { { "exileLevel", kFixtureLevel }, { "atlasCompletedTierMask", kFixtureAtlas },
                    { "bonusPassivePoints", config.bonusPassivePoints },
                    { "bonusAscendancyPoints", config.bonusAscendancyPoints } } },
                { "settings", { { "freeRefund", config.freePassiveRefund },
                    { "ascendancyRefundCost", config.ascendancyRefundCost }, { "regretOrbCount", orbs } } },
                { "state", UiTreeState(tree, state, config) },
                { "paths", candidates },
                { "refund", refund.ok ? UiTreeState(tree, refunded, config) : json(nullptr) },
                { "refundCost", PassiveTree::GetRefundNodeCost(tree, state, refundNode, config) },
                { "refundDependents", PassiveTree::RefundDependents(tree, state, refundNode) },
                { "refundAll", refunds },
                { "attributeChanges", attributeChanges },
                { "masteryUnlocked", PassiveTree::IsMasteryUnlocked(tree, state, mastery) },
                { "view", { { "name", view.name }, { "asc", view.ascendancyId },
                    { "stats", std::vector<std::string>(view.stats.begin(), view.stats.end()) } } },
                { "allocatedJewelSocket", PassiveTree::IsAllocatedJewelSocket(tree, state, target) },
                { "jewelStrength", 10 * PassiveTree::JewelEffectCopies(tree, state, target) },
            });
            return allocated;
        };

        const auto poe1 = LoadTree("tree_3_30.json");
        PassiveTreeState ranger;
        POE_CHECK(PassiveTree::SelectClass(poe1, ranger, "cls.Ranger").ok);
        ranger = probe("PoE1 Ranger path and locked mastery", poe1, ranger, 41137, 39821, 46761);
        POE_CHECK(ranger.IsAllocated(41137));
        POE_CHECK(PassiveTree::SelectMastery(poe1, ranger, 46761, 29214, 100, config).ok);
        probe("PoE1 unlocked mastery and automatic refund", poe1, ranger, 41137, 41137, 46761);
        probe("PoE1 blocked disconnecting refund", poe1, ranger, 39821, 39821, 46761);

        const auto& poe2 = PoE2Tree();
        const auto first = probe("PoE2 Ranger shared start", poe2, StartAs("cls.Ranger"), 1140, 13828);
        const auto second = probe("PoE2 Huntress shared start", poe2, StartAs("cls.Huntress"), 1140, 13828);
        POE_CHECK(first.allocatedNodes == second.allocatedNodes && first.IsAllocated(1140));
        const auto branch = probe("PoE2 extended attribute path", poe2, first, 15507, 1140);
        probe("PoE2 blocked attribute refund", poe2, branch, 1140, 1140);

        auto oracle = StartAs("cls.Druid");
        POE_CHECK(PassiveTree::SelectAscendancy(poe2, oracle, "asc.Oracle", config).ok);
        probe("PoE2 locked prerequisite", poe2, oracle, 479, 5571);
        oracle = probe("PoE2 allocate the prerequisite", poe2, oracle, 5571, 5571);
        oracle = probe("PoE2 unlocked path and refundable prerequisite", poe2, oracle, 479, 5571, 0, true);
        POE_CHECK(oracle.IsAllocated(479));
        probe("PoE2 prerequisite refund blocked by a dependent", poe2, oracle, 479, 5571);

        auto bloodMage = StartAs("cls.Witch");
        POE_CHECK(PassiveTree::SelectAscendancy(poe2, bloodMage, "asc.BloodMage", config).ok);
        bloodMage = probe("PoE2 zero-cost node on path", poe2, bloodMage, 3165, 8415);
        probe("PoE2 refund paid leaf", poe2, bloodMage, 8415, 3165);
        POE_CHECK(PassiveTree::RefundNode(poe2, bloodMage, 3165, 100, config, orbs).ok);
        probe("PoE2 refund free node", poe2, bloodMage, 8415, 8415);

        auto lich = StartAs("cls.Witch");
        POE_CHECK(PassiveTree::SelectAscendancy(poe2, lich, "asc.Lich", config).ok);
        const auto baseLich = probe("PoE2 Lich jewel base", poe2, lich, 17788, 17788);
        probe("PoE2 allocated Lich jewel effect", poe2, baseLich, 17788, 17788);
        POE_CHECK(PassiveTree::SelectAscendancy(poe2, lich, "asc.AbyssalLich", config).ok);
        lich = probe("PoE2 replacement jewel variant", poe2, lich, 17788, 17788);
        probe("PoE2 allocated Notable socket", poe2, lich, 17788, 17788);
        probe("PoE2 alias socket cannot be allocated", poe2, first, 3367, 3367);
        config.freePassiveRefund = true;
        probe("PoE2 free attribute change and refund", poe2, first, 1140, 1140);
        config.freePassiveRefund = false;

        auto pathfinder = StartAs("cls.Ranger");
        POE_CHECK(PassiveTree::SelectAscendancy(poe2, pathfinder, "asc.Pathfinder", config).ok);
        pathfinder = probe("PoE2 Pathfinder takes a hub option", poe2, pathfinder, 12795, 12795);
        probe("PoE2 Pathfinder cannot take a second hub option", poe2, pathfinder, 57253, 12795);
        pathfinder = probe("PoE2 Pathfinder accesses the Sorceress start", poe2, pathfinder, 4739, 12795);
        probe("PoE2 full refund clears a granted start dependency", poe2, pathfinder, 4739, 12795);

        auto spiritWalker = StartAs("cls.Huntress");
        POE_CHECK(PassiveTree::SelectAscendancy(poe2, spiritWalker, "asc.SpiritWalker", config).ok);
        AllocateTo(spiritWalker, 41401, config);
        AllocateTo(spiritWalker, 62743, config);
        probe("PoE2 free node needs every prerequisite", poe2, spiritWalker, 28254, 41401);
        AllocateTo(spiritWalker, 46070, config);
        spiritWalker = probe("PoE2 Sacred Unity unlocks with three prerequisites", poe2, spiritWalker, 28254, 41401);
        probe("PoE2 Sacred Unity blocks prerequisite refund", poe2, spiritWalker, 28254, 41401);
        probe("PoE2 Sacred Unity is free to refund", poe2, spiritWalker, 28254, 28254);

        auto wrongGeneration = first;
        wrongGeneration.generation = 0;
        probe("PoE2 rejects a generation-zero state", poe2, wrongGeneration, 15507, 1140);

        return cases;
    }
}

POE_TEST("Passive UI parity fixture exercises both real tree packs")
{
    POE_CHECK(BuildPassiveUiFixture().size() == 29);
}

int WritePassiveUiFixture(const std::filesystem::path& output)
{
    try {
        const auto fixture = BuildPassiveUiFixture();
        std::ofstream stream(output, std::ios::binary);
        stream << fixture.dump(2) << '\n';
        stream.close();
        if (stream) return 0;
        std::cerr << "Could not write the passive UI fixture.\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
    }
    return 1;
}
