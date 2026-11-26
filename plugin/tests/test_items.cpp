#include "test_harness.h"

#include "core/Config.h"
#include "core/DataPack.h"
#include "core/ItemAffixParser.h"
#include "core/ItemCrafting.h"
#include "core/ItemData.h"
#include "core/ItemFormatter.h"
#include "core/LootTable.h"
#include "core/PassiveTree.h"
#include "core/PassiveTreeData.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"

#include <filesystem>
#include <random>
#include <unordered_set>

namespace
{
    using namespace SkyrimPoE::Core;

    const ItemData& SharedItemData()
    {
        static ItemData data;
        static bool init = false;
        if (!init) {
            init = true;
            std::string err;
            const auto path = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "gen0" / "items.json";
            POE_CHECK(data.LoadFromFile(path, &err));
        }
        return data;
    }

    const DataPack& SharedCorePack()
    {
        static DataPack pack;
        static bool init = false;
        if (!init) {
            init = true;
            std::string err;
            const auto path = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "gen0" / "core.json";
            POE_CHECK(pack.LoadFromFile(path, &err));
        }
        return pack;
    }

    const PassiveTreeData& SharedTreeData()
    {
        static PassiveTreeData tree;
        static bool init = false;
        if (!init) {
            init = true;
            std::string err;
            const auto path = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "gen0" / "tree.json";
            POE_CHECK(tree.LoadFromFile(path, &err));
        }
        return tree;
    }
}

POE_TEST("Stage4 ItemData load, first-match-wins weights, and base curves")
{
    const auto& data = SharedItemData();
    POE_CHECK(data.IsLoaded());
    POE_CHECK(data.AllAffixes().size() > 800);
    POE_CHECK(data.AllCurrencies().size() == kCurrencyKindCount);

    // First-match-wins: IncreasedLife0 has weapon=0 before default=1000
    const auto* life0 = data.FindAffix("IncreasedLife0");
    POE_CHECK(life0 != nullptr);
    const auto swordTags = ItemData::BuildItemTags(TokenSlotGroup::OneHandWeapon, DefenseSubtype::None, "OneHandSword");
    const auto helmTags = ItemData::BuildItemTags(TokenSlotGroup::Helmet, DefenseSubtype::Armour, "");
    POE_CHECK(ItemData::ResolveWeight(*life0, swordTags) == 0);
    POE_CHECK(ItemData::ResolveWeight(*life0, helmTags) == 1000);

    // Shield str_shield block chance and ES shield focus spell damage
    const auto* blk1 = data.FindAffix("LocalIncreasedBlockPercentage1");
    POE_CHECK(blk1 != nullptr && blk1->rollable && blk1->isLocal);
    const auto strShieldTags = ItemData::BuildItemTags(TokenSlotGroup::Shield, DefenseSubtype::Armour, "");
    const auto esShieldTags = ItemData::BuildItemTags(TokenSlotGroup::Shield, DefenseSubtype::EnergyShield, "");
    POE_CHECK(ItemData::ResolveWeight(*blk1, strShieldTags) == 1000);
    POE_CHECK(ItemData::ResolveWeight(*blk1, esShieldTags) == 500);
    POE_CHECK(ItemData::ResolveWeight(*blk1, helmTags) == 0);

    const auto* spell1 = data.FindAffix("SpellDamageOnWeapon1");
    POE_CHECK(spell1 != nullptr);
    const auto daggerTags = ItemData::BuildItemTags(TokenSlotGroup::OneHandWeapon, DefenseSubtype::None, "Dagger");
    POE_CHECK(ItemData::ResolveWeight(*spell1, daggerTags) == 0);  // attack_dagger=0 blocks dagger=1500
    POE_CHECK(ItemData::ResolveWeight(*spell1, esShieldTags) > 0); // focus > 0

    // Base curves
    const auto bodyLow = data.LookupArmorBase(TokenSlotGroup::Body, DefenseSubtype::Armour, 1);
    const auto bodyHigh = data.LookupArmorBase(TokenSlotGroup::Body, DefenseSubtype::Armour, 84);
    POE_CHECK(bodyHigh.baseArmour > bodyLow.baseArmour);
    POE_CHECK(bodyHigh.reqStr > bodyLow.reqStr);

    const auto swordHigh = data.LookupWeaponBase("OneHandSword", 80);
    POE_CHECK(swordHigh.physMax > swordHigh.physMin);
    POE_CHECK(swordHigh.critChance >= 5.0);
}

POE_TEST("Stage4 ItemCrafting 12 orbs, 3P/3S caps, group uniqueness, and Scouring iLvl preservation")
{
    const auto& data = SharedItemData();
    std::mt19937_64 rng(0xC0FFEE42ULL);

    // 1. Sealed token identify clamps iLvl inside tier bounds (Decision 157)
    auto t1Item = ItemCrafting::IdentifyToken(
        1, ItemRarity::Rare, TokenSlotGroup::Body, DefenseSubtype::Armour, "", false, 0x00012E49, 95, data, rng);
    POE_CHECK(t1Item.rarity == ItemRarity::Rare);
    POE_CHECK(t1Item.itemLevel >= 1 && t1Item.itemLevel <= 20);
    POE_CHECK(t1Item.affixes.size() >= 4 && t1Item.affixes.size() <= 6);

    auto t5Item = ItemCrafting::IdentifyToken(
        5, ItemRarity::Magic, TokenSlotGroup::OneHandWeapon, DefenseSubtype::None, "OneHandSword", false, 0x00012EB7, 5, data, rng);
    POE_CHECK(t5Item.rarity == ItemRarity::Magic);
    POE_CHECK(t5Item.itemLevel >= 81 && t5Item.itemLevel <= 100);
    POE_CHECK(t5Item.affixes.size() >= 1 && t5Item.affixes.size() <= 2);

    // 2. Full orb round-trip on Normal iLvl 84 Heavy Body Armor
    PoEItemRecord armor;
    armor.baseFormId = 0x00012E49;
    armor.rarity = ItemRarity::Normal;
    armor.itemLevel = 84;
    armor.slotGroup = TokenSlotGroup::Body;
    armor.defenseSubtype = DefenseSubtype::Armour;
    const auto bodyTags = ItemData::BuildItemTags(armor.slotGroup, armor.defenseSubtype, "");

    // Illegal ops on Normal rejected without modifying item
    POE_CHECK(!ItemCrafting::ApplyOrb(CurrencyKind::Chaos, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(!ItemCrafting::ApplyOrb(CurrencyKind::Exalted, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(!ItemCrafting::ApplyOrb(CurrencyKind::Scouring, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(!ItemCrafting::ApplyOrb(CurrencyKind::Divine, armor, bodyTags, data, rng).IsOk());

    // Transmutation -> Magic (1-2)
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Transmutation, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(armor.rarity == ItemRarity::Magic);
    POE_CHECK(armor.affixes.size() >= 1 && armor.affixes.size() <= 2);

    // Alteration -> stays Magic (1-2)
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Alteration, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(armor.rarity == ItemRarity::Magic);

    // Force 1 affix to test Augmentation deterministically
    armor.affixes.resize(1);
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Augmentation, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(armor.affixes.size() == 2);
    // Augmentation rejected when already 1P + 1S
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Augmentation, armor, bodyTags, data, rng).code == CraftResultCode::AffixLimitReached);

    // Regal -> Rare (+1 affix)
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Regal, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(armor.rarity == ItemRarity::Rare);
    POE_CHECK(armor.affixes.size() == 3);

    // Exalted up to 6 affixes, then rejected
    while (armor.affixes.size() < 6) {
        const auto res = ItemCrafting::ApplyOrb(CurrencyKind::Exalted, armor, bodyTags, data, rng);
        POE_CHECK(res.IsOk());
    }
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Exalted, armor, bodyTags, data, rng).code == CraftResultCode::AffixLimitReached);

    // Verify 3P/3S cap and unique groups
    std::size_t pCnt = 0, sCnt = 0;
    std::unordered_set<std::string> groups;
    for (const auto& a : armor.affixes) {
        const auto* def = data.FindAffix(a.affixId);
        POE_CHECK(def != nullptr);
        POE_CHECK(def->level <= 84);
        if (def->type == AffixType::Prefix) ++pCnt; else ++sCnt;
        POE_CHECK(groups.insert(def->group).second);
    }
    POE_CHECK(pCnt == 3 && sCnt == 3);

    // Divine rerolls values while preserving affix IDs
    const auto beforeIds = armor.affixes;
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Divine, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(armor.affixes.size() == beforeIds.size());
    for (std::size_t i = 0; i < armor.affixes.size(); ++i) {
        POE_CHECK(armor.affixes[i].affixId == beforeIds[i].affixId);
    }

    // Divine NoOp on fixed-value-only affix (MovementVelocity1 has fixed 10%)
    PoEItemRecord fixedBoots;
    fixedBoots.rarity = ItemRarity::Magic;
    fixedBoots.itemLevel = 10;
    fixedBoots.slotGroup = TokenSlotGroup::Boots;
    fixedBoots.defenseSubtype = DefenseSubtype::Evasion;
    fixedBoots.affixes.push_back(RolledAffix{ "MovementVelocity1", { 10.0 } });
    const auto bootsTags = ItemData::BuildItemTags(fixedBoots.slotGroup, fixedBoots.defenseSubtype, "");
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Divine, fixedBoots, bootsTags, data, rng).code == CraftResultCode::NoOp);

    // Chaos rerolls Rare
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Chaos, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(armor.rarity == ItemRarity::Rare);
    POE_CHECK(armor.affixes.size() >= 4 && armor.affixes.size() <= 6);

    // Scouring -> Normal while preserving itemLevel = 84 (Review M1!)
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Scouring, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(armor.rarity == ItemRarity::Normal);
    POE_CHECK(armor.affixes.empty());
    POE_CHECK(armor.itemLevel == 84);

    // Alchemy -> Rare again
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Alchemy, armor, bodyTags, data, rng).IsOk());
    POE_CHECK(armor.rarity == ItemRarity::Rare);
    POE_CHECK(armor.affixes.size() >= 4 && armor.affixes.size() <= 6);

    // Jeweller's Orb: 3->4->5, reject at 5, reject Support gem
    GemInstance cyc;
    cyc.gemId = "Cyclone";
    cyc.socketCount = 3;
    POE_CHECK(ItemCrafting::ApplyJewellerToGem(cyc, GemKind::AttackChannel).IsOk());
    POE_CHECK(cyc.socketCount == 4);
    POE_CHECK(ItemCrafting::ApplyJewellerToGem(cyc, GemKind::AttackChannel).IsOk());
    POE_CHECK(cyc.socketCount == 5);
    POE_CHECK(ItemCrafting::ApplyJewellerToGem(cyc, GemKind::AttackChannel).code == CraftResultCode::AlreadyMaxSockets);

    GemInstance sup;
    sup.gemId = "SupportBrutality";
    sup.socketCount = 0;
    POE_CHECK(ItemCrafting::ApplyJewellerToGem(sup, GemKind::Support).code == CraftResultCode::UnsupportedTarget);
    POE_CHECK(ItemCrafting::ApplyJewellerToGem(sup, GemKind::Aura).code == CraftResultCode::UnsupportedTarget);
    POE_CHECK(ItemCrafting::ApplyJewellerToGem(sup, GemKind::Herald).code == CraftResultCode::UnsupportedTarget);
}

POE_TEST("Stage4 ItemAffixParser local vs global separation and ItemFormatter")
{
    const auto& data = SharedItemData();
    PoEItemRecord sword;
    sword.rarity = ItemRarity::Rare;
    sword.itemLevel = 84;
    sword.slotGroup = TokenSlotGroup::OneHandWeapon;
    sword.weaponTag = "OneHandSword";
    sword.affixes = {
        { "LocalIncreasedPhysicalDamagePercent5", { 110.0 } },
        { "LocalAddedFireDamage3", { 12.0, 24.0 } },
        { "LocalIncreasedAttackSpeed2", { 15.0 } },
        { "Strength4", { 25.0 } },
        { "FireResist4", { 28.0 } },
    };

    const auto stats = ItemAffixParser::ParseItem(sword, data);
    POE_CHECK_NEAR(stats.localIncPhysPct, 110.0, 1e-6);
    POE_CHECK_NEAR(stats.localFlatFireMin, 12.0, 1e-6);
    POE_CHECK_NEAR(stats.localFlatFireMax, 24.0, 1e-6);
    POE_CHECK_NEAR(stats.localIncAttackSpeedPct, 15.0, 1e-6);
    POE_CHECK_NEAR(stats.incPhysicalDamagePct, 0.0, 1e-6);  // Local did not leak into global!
    POE_CHECK_NEAR(stats.incAttackSpeedPct, 0.0, 1e-6);     // Local did not leak into global!
    POE_CHECK_NEAR(stats.flatStr, 25.0, 1e-6);
    POE_CHECK_NEAR(stats.fireResist, 28.0, 1e-6);

    EquippedSlotInput slotIn;
    slotIn.occupied = true;
    slotIn.isCrafted = true;
    slotIn.isWeapon = true;
    slotIn.reqLevel = 50;
    slotIn.reqStr = 80;
    slotIn.baseWeaponPhysMin = 20.0;
    slotIn.baseWeaponPhysMax = 40.0;
    slotIn.stats = stats;

    const auto card = ItemFormatter::FormatItem(&sword, "철 검", EquipSlot::MainHand, slotIn, true, data);
    POE_CHECK(card.rarityBadge == "[희귀]");
    POE_CHECK(card.slotLabel == "주무기");
    POE_CHECK(card.affixLines.size() == 5);
    POE_CHECK(!card.baseStatLines.empty());
    POE_CHECK(!card.requirementText.empty());
}

POE_TEST("Stage4 StatEngine 8-slot gear, fixed-point circular requirement blocking, 5-element attack, and gem reqs")
{
    const auto& core = SharedCorePack();
    Config cfg;
    StatEngine engine;

    Layer1Input l1;
    l1.exileLevel = 50;
    l1.className = "Scion";  // Base 20/20/20

    // Slot 1: Amulet (req 0) grants +40 Str
    auto& amu = l1.equippedGear[static_cast<std::size_t>(EquipSlot::Amulet)];
    amu.occupied = true;
    amu.isCrafted = true;
    amu.reqLevel = 1;
    amu.reqStr = 0;
    amu.stats.flatStr = 40.0;
    amu.stats.flatLife = 50.0;

    // Slot 2: Body Armor requires Str 55 (enabled ONLY because Amulet gave +40 Str -> 20+40=60 >= 55!)
    auto& body = l1.equippedGear[static_cast<std::size_t>(EquipSlot::Body)];
    body.occupied = true;
    body.isCrafted = true;
    body.reqLevel = 40;
    body.reqStr = 55;
    body.baseArmour = 300.0;
    body.stats.localIncArmourPct = 50.0;  // local -> 450 armour
    body.stats.fireResist = 30.0;

    // Slot 3: Helmet with circular self-satisfying Str (reqStr=90, gives +40 Str; 60 < 90 so MUST stay disabled!)
    auto& helm = l1.equippedGear[static_cast<std::size_t>(EquipSlot::Helmet)];
    helm.occupied = true;
    helm.isCrafted = true;
    helm.reqLevel = 40;
    helm.reqStr = 90;
    helm.baseArmour = 500.0;
    helm.stats.flatStr = 40.0;
    helm.stats.flatLife = 100.0;

    // Slot 4: MainHand weapon with local phys + local fire + local attack speed
    auto& mh = l1.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)];
    mh.occupied = true;
    mh.isCrafted = true;
    mh.isWeapon = true;
    mh.reqLevel = 30;
    mh.reqStr = 50;
    mh.baseWeaponPhysMin = 40.0;
    mh.baseWeaponPhysMax = 60.0;
    mh.baseWeaponCritChancePct = 5.0;
    mh.stats.localIncPhysPct = 100.0;  // -> 80..120 phys
    mh.stats.localFlatFireMin = 20.0;
    mh.stats.localFlatFireMax = 30.0;
    mh.stats.localIncAttackSpeedPct = 20.0;  // x1.20

    // Equip Cyclone in slot 0
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 1;
    l1.slots[0].gem.gemId = "Cyclone";
    l1.slots[0].gem.level = 1;
    l1.slots[0].gem.socketCount = 3;

    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    auto snap = engine.GetSnapshot();

    POE_CHECK(snap->gearSlotActive[static_cast<std::size_t>(EquipSlot::Amulet)]);
    POE_CHECK(snap->gearSlotActive[static_cast<std::size_t>(EquipSlot::Body)]);
    POE_CHECK(!snap->gearSlotActive[static_cast<std::size_t>(EquipSlot::Helmet)]);
    POE_CHECK(snap->gearSlotReqFailed[static_cast<std::size_t>(EquipSlot::Helmet)]);
    POE_CHECK(snap->str == 60);  // Helmet's +40 Str NOT included!
    POE_CHECK_NEAR(snap->armour, 450.0, 1e-3);  // Body local 300 * 1.5, Helmet 500 excluded!

    // Cyclone 5-element attack includes weapon fire damage & local attack speed
    const auto& cycStats = snap->slotStats[0];
    POE_CHECK(cycStats.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Physical)].Average() > 0.0);
    POE_CHECK(cycStats.baseHitDamage.byType[static_cast<std::size_t>(DamageType::Fire)].Average() > 0.0);
    POE_CHECK_NEAR(cycStats.attackSpeedMult, 3.0 * 1.20, 1e-4);

    // Gem requirement check: Cyclone Lv1 needs Dex ~46, Scion has Dex 20 -> requirementMet == false!
    POE_CHECK(!cycStats.requirementMet);

    // Add Dex on Ring1 to satisfy Cyclone requirement
    auto& ring = l1.equippedGear[static_cast<std::size_t>(EquipSlot::Ring1)];
    ring.occupied = true;
    ring.isCrafted = true;
    ring.stats.flatDex = 40.0;  // 20 + 40 = 60
    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    snap = engine.GetSnapshot();
    POE_CHECK(snap->slotStats[0].requirementMet);
}

POE_TEST("Stage4 LootTable tier mapping and PassiveTree Orb of Regret cost model")
{
    const auto& data = SharedItemData();
    Config cfg;
    cfg.tokenDropChance = 1.0;
    cfg.currencyDropChance = 1.0;
    std::mt19937_64 rng(12345);

    const auto dropsLow = LootTable::RollKillDrops(15, false, false, cfg, data, rng);
    POE_CHECK(dropsLow.size() >= 2);
    POE_CHECK(dropsLow[0].kind == DropKind::SealedToken && dropsLow[0].tier == 1);

    const auto dropsDragon = LootTable::RollKillDrops(85, true, true, cfg, data, rng);
    POE_CHECK(dropsDragon.size() >= 3);
    bool sawT5 = false;
    for (const auto& d : dropsDragon) {
        if (d.kind == DropKind::SealedToken && d.tier == 5) sawT5 = true;
    }
    POE_CHECK(sawT5);

    // PassiveTree Orb of Regret cost when FreePassiveRefund=false (Review M4 full matrix)
    const auto& tree = SharedTreeData();
    Config paidCfg;
    paidCfg.freePassiveRefund = false;
    paidCfg.ascendancyRefundCost = 5;
    paidCfg.bonusAscendancyPoints = 4;

    PassiveTreeState st;
    POE_CHECK(PassiveTree::SelectClass(tree, st, 1).ok);  // Marauder
    const auto* clsDef = tree.FindClass(1);
    POE_CHECK(clsDef != nullptr && !clsDef->startNeighbors.empty());
    const auto firstNode = clsDef->startNeighbors.front();
    POE_CHECK(PassiveTree::AllocatePath(tree, st, { firstNode }, 30, paidCfg).ok);

    POE_CHECK(PassiveTree::GetRefundNodeCost(tree, st, firstNode, paidCfg) == 1);
    // 0 orbs -> rejected, state unchanged
    const auto failRes = PassiveTree::RefundNode(tree, st, firstNode, 30, paidCfg, 0);
    POE_CHECK(!failRes.ok);
    POE_CHECK(st.allocatedNodes.size() == 1);
    // 1 orb -> succeeds, consumes 1 orb
    const auto okRes = PassiveTree::RefundNode(tree, st, firstNode, 30, paidCfg, 1);
    POE_CHECK(okRes.ok && okRes.regretOrbsConsumed == 1);
    POE_CHECK(st.allocatedNodes.empty());

    // Find a reachable notable that unlocks a mastery with >= 2 effects to test orphan mastery + swap cost
    std::uint32_t notableId = 0;
    std::uint32_t masteryId = 0;
    std::uint32_t effA = 0, effB = 0;
    for (const auto& [nid, nd] : tree.AllNodes()) {
        if (nd.kind == PassiveNodeKind::Mastery && nd.masteryEffectIds.size() >= 2) {
            const auto* notables = tree.FindGroupNotables(nd.groupId);
            if (!notables || notables->empty()) continue;
            auto path = PassiveTree::ComputeShortestPath(tree, st, notables->front());
            if (!path.empty() && path.size() <= 10) {
                notableId = notables->front();
                masteryId = nid;
                effA = nd.masteryEffectIds[0];
                effB = nd.masteryEffectIds[1];
                POE_CHECK(PassiveTree::AllocatePath(tree, st, path, 50, paidCfg).ok);
                break;
            }
        }
    }
    POE_CHECK(notableId != 0 && masteryId != 0);
    // First mastery select is free (costs passive point, 0 regret orbs)
    const auto selA = PassiveTree::SelectMastery(tree, st, masteryId, effA, 50, paidCfg, 0);
    POE_CHECK(selA.ok && selA.regretOrbsConsumed == 0);
    // Re-selecting same effect costs 0 regret orbs
    POE_CHECK(PassiveTree::GetSelectMasterySwapCost(st, masteryId, effA, paidCfg) == 0);
    // Swapping mastery effect costs 1 regret orb (0 orbs fails without mutating, 1 orb succeeds)
    POE_CHECK(PassiveTree::GetSelectMasterySwapCost(st, masteryId, effB, paidCfg) == 1);
    POE_CHECK(!PassiveTree::SelectMastery(tree, st, masteryId, effB, 50, paidCfg, 0).ok);
    POE_CHECK(st.masterySelections[masteryId] == effA);
    const auto swapOk = PassiveTree::SelectMastery(tree, st, masteryId, effB, 50, paidCfg, 1);
    POE_CHECK(swapOk.ok && swapOk.regretOrbsConsumed == 1 && st.masterySelections[masteryId] == effB);

    // Refunding the sole notable unlocking that mastery must cost 1 + 1 = 2 regret orbs!
    POE_CHECK(PassiveTree::GetRefundNodeCost(tree, st, notableId, paidCfg) == 2);
    POE_CHECK(!PassiveTree::RefundNode(tree, st, notableId, 50, paidCfg, 1).ok);
    POE_CHECK(st.masterySelections.contains(masteryId));
    const auto orphanOk = PassiveTree::RefundNode(tree, st, notableId, 50, paidCfg, 2);
    POE_CHECK(orphanOk.ok && orphanOk.autoRefundedMasteries == 1 && orphanOk.regretOrbsConsumed == 2);
    POE_CHECK(!st.masterySelections.contains(masteryId));

    // Ascendancy node refund costs 5 regret orbs + RefundAll(ascendancyOnly=true/false)
    POE_CHECK(PassiveTree::SelectAscendancy(tree, st, "Juggernaut", paidCfg).ok);
    const auto* ascDef = tree.FindAscendancy("Juggernaut");
    POE_CHECK(ascDef != nullptr);
    const auto* ascStart = tree.FindNode(ascDef->startNodeId);
    POE_CHECK(ascStart != nullptr && !ascStart->adjacency.empty());
    const auto ascNode1 = ascStart->adjacency.front();
    POE_CHECK(PassiveTree::AllocatePath(tree, st, { ascNode1 }, 50, paidCfg).ok);
    POE_CHECK(PassiveTree::GetRefundNodeCost(tree, st, ascNode1, paidCfg) == 5);
    POE_CHECK(!PassiveTree::RefundNode(tree, st, ascNode1, 50, paidCfg, 4).ok);
    POE_CHECK(PassiveTree::GetRefundAllCost(tree, st, true, paidCfg) == 5);
    POE_CHECK(!PassiveTree::RefundAll(tree, st, true, 50, paidCfg, 4).ok);
    const auto ascAllOk = PassiveTree::RefundAll(tree, st, true, 50, paidCfg, 5);
    POE_CHECK(ascAllOk.ok && ascAllOk.regretOrbsConsumed == 5 && st.ascendancyId.empty());
    const auto fullCost = PassiveTree::GetRefundAllCost(tree, st, false, paidCfg);
    POE_CHECK(fullCost == st.allocatedNodes.size());
    POE_CHECK(PassiveTree::RefundAll(tree, st, false, 50, paidCfg, fullCost).ok);
    POE_CHECK(st.allocatedNodes.empty());
}

POE_TEST("Stage4 Review regressions: LocalCriticalMultiplier, Regen/MoveSpeed, Warstaff/Crossbow, Brutality, DualWield, EnforceGemReq")
{
    const auto& data = SharedItemData();
    const auto& core = SharedCorePack();

    // C1: LocalCriticalMultiplier1 must parse into globalCritMultiplierPct (not dropped!)
    PoEItemRecord critSword;
    critSword.rarity = ItemRarity::Rare;
    critSword.itemLevel = 84;
    critSword.slotGroup = TokenSlotGroup::OneHandWeapon;
    critSword.weaponTag = "OneHandSword";
    critSword.affixes = {
        { "LocalCriticalMultiplier1", { 12.0 } },
        { "LifeRegeneration3", { 15.0 } },
        { "ManaRegeneration2", { 25.0 } },
        { "MovementVelocity3", { 20.0 } },
    };
    const auto parsed = ItemAffixParser::ParseItem(critSword, data);
    POE_CHECK_NEAR(parsed.globalCritMultiplierPct, 12.0, 1e-6);
    POE_CHECK_NEAR(parsed.lifeRegenPerSec, 15.0, 1e-6);
    POE_CHECK_NEAR(parsed.incManaRegenPct, 25.0, 1e-6);
    POE_CHECK_NEAR(parsed.incMovementSpeedPct, 20.0, 1e-6);

    // M1: Warstaff blocks SpellDamageOnTwoHandWeapon1 via attack_staff=0; Crossbow maps to Bow curve
    const auto warTags = ItemData::BuildItemTags(TokenSlotGroup::TwoHandOrRangedWeapon, DefenseSubtype::None, "Warstaff");
    const auto* sd2h = data.FindAffix("SpellDamageOnTwoHandWeapon1");
    POE_CHECK(sd2h != nullptr && ItemData::ResolveWeight(*sd2h, warTags) == 0);
    const auto xbowTags = ItemData::BuildItemTags(TokenSlotGroup::TwoHandOrRangedWeapon, DefenseSubtype::None, "Crossbow");
    const auto* ias4 = data.FindAffix("LocalIncreasedAttackSpeed4");
    POE_CHECK(ias4 != nullptr && ItemData::ResolveWeight(*ias4, xbowTags) > 0);
    POE_CHECK(data.LookupWeaponBase("Crossbow", 50).physMax == data.LookupWeaponBase("Bow", 50).physMax);
    POE_CHECK(data.LookupWeaponBase("Warstaff", 50).physMax == data.LookupWeaponBase("Staff", 50).physMax);

    // m1: TierLevelBounds(0) clamps to T1 (1..20)
    POE_CHECK(TierLevelBounds(0).first == 1 && TierLevelBounds(0).second == 20);

    // C2 + M3 + M4: Dual Wield average + +20% block + +10% MORE speed, Brutality zeroing local fire, EnforceGemRequirements=true
    Config strictCfg;
    strictCfg.enforceGemRequirements = true;
    StatEngine engine;
    Layer1Input l1;
    l1.exileLevel = 50;
    l1.className = "Scion";  // 20/20/20
    l1.passives.flatStr = 120.0;
    l1.passives.flatDex = 120.0;

    auto& mh = l1.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)];
    mh.occupied = true;
    mh.isCrafted = true;
    mh.isWeapon = true;
    mh.baseWeaponPhysMin = 20.0;
    mh.baseWeaponPhysMax = 40.0;
    mh.stats = parsed;
    mh.stats.localFlatFireMin = 50.0;
    mh.stats.localFlatFireMax = 60.0;

    auto& oh = l1.equippedGear[static_cast<std::size_t>(EquipSlot::OffHand)];
    oh.occupied = true;
    oh.isCrafted = true;
    oh.isWeapon = true;
    oh.baseWeaponPhysMin = 60.0;
    oh.baseWeaponPhysMax = 80.0;

    // Slot 0: Cyclone + SupportBrutality (met) + SupportIncreasedCriticalStrikes (needs Int -> unmet!)
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 1;
    l1.slots[0].gem.gemId = "Cyclone";
    l1.slots[0].gem.level = 1;
    l1.slots[0].gem.socketCount = 3;
    l1.slots[0].gem.socketedGems.push_back(GemInstance{ 2, "SupportBrutality", 1, 0, 0, 0, {} });
    l1.slots[0].gem.socketedGems.push_back(GemInstance{ 3, "SupportIncreasedCriticalStrikes", 20, 0, 0, 0, {} });

    // Slot 1: CoC Lv20 (needs high Int -> unmet!) linked to slot 0
    l1.slots[1].occupied = true;
    l1.slots[1].linkedAttackSlot = 0;
    l1.slots[1].gem.instanceId = 4;
    l1.slots[1].gem.gemId = "SupportCastOnCriticalStrike";
    l1.slots[1].gem.level = 20;
    l1.slots[1].gem.socketCount = 3;
    l1.slots[1].gem.socketedGems.push_back(GemInstance{ 5, "IceNova", 1, 0, 0, 0, {} });

    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(core, strictCfg));
    auto snap = engine.GetSnapshot();

    // C2 fields surfaced
    POE_CHECK_NEAR(snap->lifeRegenPerSec, 15.0, 1e-6);
    POE_CHECK_NEAR(snap->incManaRegenPct, 25.0, 1e-6);
    POE_CHECK_NEAR(snap->incMovementSpeedPct, 20.0, 1e-6);
    // M3 dual-wield bonuses
    POE_CHECK_NEAR(snap->attackBlockChance, 20.0, 1e-6);
    POE_CHECK_NEAR(snap->slotStats[0].attackSpeedMult, 3.0 * 1.10, 1e-4);
    POE_CHECK_NEAR(snap->slotStats[0].critMultiplierPct, 212.0, 1e-4);  // 200 + 12 from LocalCriticalMultiplier1
    // Brutality zeroes local weapon Fire damage
    POE_CHECK_NEAR(snap->slotStats[0].baseHitDamage.byType[static_cast<std::size_t>(DamageType::Fire)].Average(), 0.0, 1e-6);
    // EnforceGemRequirements=true: unmet SupportIncreasedCriticalStrikes excluded (baseCrit stays 5.0), unmet CoC link rejected (triggeredSpellStats empty!)
    POE_CHECK_NEAR(snap->slotStats[0].baseCritChancePct, 5.0, 1e-4);
    POE_CHECK(!snap->slotStats[1].requirementMet);
    POE_CHECK(snap->triggeredSpellStats.empty());

    // Batch 2 review M2 & m2: Skyrim 1H kStaff ("Staff") gets "staff" tag, and TryTriggerOnCrit skips unmet socketed spell
    const auto staff1hTags = ItemData::BuildItemTags(TokenSlotGroup::OneHandWeapon, DefenseSubtype::None, "Staff");
    POE_CHECK(staff1hTags.contains("staff"));

    l1.passives.flatInt = 50.0;                 // Scion 20 + 50 = 70 Int: satisfies CoC Lv1 (~46), fails IceNova Lv20 (~155)
    l1.slots[1].gem.level = 1;                  // CoC trigger Lv1 is met
    l1.slots[1].gem.socketedGems[0].level = 20; // IceNova Lv20 requires high Int -> unmet!
    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(core, strictCfg));
    auto snap2 = engine.GetSnapshot();
    POE_CHECK(snap2->slotStats[1].requirementMet);
    POE_CHECK(snap2->triggeredSpellStats.contains(5));
    POE_CHECK(!snap2->triggeredSpellStats.at(5).requirementMet);
    SkillSystem strictSys;
    const auto trigBlocked = strictSys.TryTriggerOnCrit(0, *snap2, l1.slots, 0.0, 200.0, core, strictCfg, 200.0, 200.0);
    POE_CHECK(!trigBlocked.has_value());
}
