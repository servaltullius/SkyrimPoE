#include "test_harness.h"

#include "core/Config.h"
#include "core/DamageConversion.h"
#include "core/DataPack.h"
#include "core/AilmentSystem.h"
#include "core/EnergyShield.h"
#include "core/MonsterModSystem.h"
#include "core/PassiveStatParser.h"
#include "core/PassiveTree.h"
#include "core/RulesPack.h"
#include "core/PassiveTreeData.h"
#include "core/StatEngine.h"

#include <cctype>
#include <cstdio>
#include <filesystem>

namespace
{
    using namespace SkyrimPoE::Core;

    const DataPack& LoadPack()
    {
        static DataPack pack;
        static bool loaded = false;
        if (!loaded) {
            std::string err;
            const auto p = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "core_3_30.json";
            POE_CHECK(pack.LoadFromFile(p, &err));
            loaded = true;
        }
        return pack;
    }

    const PassiveTreeData& LoadTree()
    {
        static PassiveTreeData tree;
        static bool loaded = false;
        if (!loaded) {
            std::string err;
            const auto p = std::filesystem::path(POE_TEST_DATA_DIR) / "packs" / "tree_3_30.json";
            POE_CHECK(tree.LoadFromFile(p, &err));
            loaded = true;
        }
        return tree;
    }

    const PassiveTreeData& LoadPoe2Tree()
    {
        static const PassiveTreeData tree = [] {
            PassiveTreeData result;
            std::string error;
            POE_CHECK(result.LoadFromFile(std::filesystem::path(POE_TEST_DATA_DIR) / "packs/gen1/tree.json", &error));
            return result;
        }();
        return tree;
    }

    PassiveStatTotals Poe2Keystone(std::string_view name)
    {
        const auto& tree = LoadPoe2Tree();
        PassiveTreeState state;
        POE_CHECK(PassiveTree::SelectClass(tree, state, 0).ok);
        for (const auto& [id, node] : tree.AllNodes()) {
            if (node.kind == PassiveNodeKind::Keystone && node.name == name) {
                state.allocatedNodes.push_back(id);
                return PassiveStatParser::AggregateTree(tree, state);
            }
        }
        throw std::runtime_error("Missing PoE2 keystone " + std::string(name));
    }

    Layer1Input CombatInput()
    {
        Layer1Input input;
        input.passives.poe2 = true;
        input.flatEnergyShield = 90.0;
        input.evasion = 100.0;
        input.armour = 20.0;
        auto& weapon = input.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)];
        weapon.occupied = true;
        weapon.isWeapon = true;
        weapon.baseWeaponPhysMin = 100.0;
        weapon.baseWeaponPhysMax = 100.0;
        for (const auto& [index, gem] : { std::pair{ 0u, "Cyclone" }, { 1u, "Fireball" }, { 2u, "IceNova" } }) {
            auto& slot = input.slots[index];
            slot.occupied = true;
            slot.slotIndex = static_cast<std::uint8_t>(index);
            slot.gem.gemId = gem;
            slot.gem.instanceId = index + 1;
            slot.gem.level = 1;
        }
        return input;
    }

    std::shared_ptr<const StatSnapshot> Snapshot(const Layer1Input& input, const Layer2Input& dynamic = {})
    {
        StatEngine engine;
        engine.SetLayer1(input);
        engine.SetLayer2(dynamic);
        POE_CHECK(engine.RebuildIfDirty(LoadPack(), Config{}));
        return engine.GetSnapshot();
    }
}

POE_TEST("PoE2 Avatar of Fire converts 75 percent of non-fire hit damage and removes the remainder")
{
    auto input = CombatInput();
    const auto before = Snapshot(input)->slotStats[0].baseHitDamage;
    input.passives = Poe2Keystone("Avatar of Fire");
    const auto after = Snapshot(input)->slotStats[0].baseHitDamage;
    POE_CHECK_NEAR(after.TotalAverage(), before.TotalAverage() * 0.75, 1e-8);
    POE_CHECK_NEAR(after.byType[static_cast<std::size_t>(DamageType::Fire)].Average(), after.TotalAverage(), 1e-8);
}

POE_TEST("PoE2 Blackflame Covenant converts fire spells and their ignite becomes chaos damage")
{
    auto input = CombatInput();
    const auto before = Snapshot(input);
    input.passives = Poe2Keystone("Blackflame Covenant");
    const auto after = Snapshot(input);
    const auto& fireball = after->slotStats[1].baseHitDamage.byType;
    POE_CHECK_NEAR(fireball[static_cast<std::size_t>(DamageType::Fire)].Average(), 0.0, 1e-8);
    POE_CHECK_NEAR(fireball[static_cast<std::size_t>(DamageType::Chaos)].Average(),
        before->slotStats[1].baseHitDamage.TotalAverage(), 1e-8);
    POE_CHECK_NEAR(after->slotStats[2].baseHitDamage.TotalAverage(), before->slotStats[2].baseHitDamage.TotalAverage(), 1e-8);
    AilmentSystem system;
    HitAilmentInput hit;
    hit.chaosHitPoe = 100.0;
    hit.igniteRoll01 = 0.0;
    POE_CHECK(!system.ApplyHitAilments(1, hit).appliedIgnite);
    hit.chaosContributesToIgnite = after->chaosFireSpellIgniteMagnitude;
    hit.igniteDealsChaos = after->fireSpellIgniteDealsChaos;
    POE_CHECK(system.ApplyHitAilments(1, hit).appliedIgnite);
    POE_CHECK(system.FlammabilityChancePercent(1) > 0.0);
    const auto dot = system.TickActor(1, 1.0, false);
    POE_CHECK_NEAR(dot.firePoeDamage, 0.0, 1e-8);
    POE_CHECK_NEAR(dot.chaosIgnitePoeDamage, ActiveRuleValues().ignite.damagePercentPerMinute / 60.0, 1e-8);
}

POE_TEST("PoE2 global conversion normalizes the original fire without reconverting its results")
{
    PassiveStatTotals passives = Poe2Keystone("Avatar of Fire");
    POE_CHECK(PassiveStatParser::ParsePoe2Line("Fire Spells Convert 100% of Fire Damage to Chaos Damage", passives, true));

    const auto fire = static_cast<std::size_t>(DamageType::Fire);
    const auto chaos = static_cast<std::size_t>(DamageType::Chaos);
    DamagePacket packet;
    packet.byType[fire] = { 100.0, 100.0 };
    ConvertGlobalHitDamage(packet, passives, true);
    POE_CHECK_NEAR(packet.byType[fire].Average(), 100.0 * 75.0 / 175.0, 1e-8);
    POE_CHECK_NEAR(packet.byType[chaos].Average(), 100.0 * 100.0 / 175.0, 1e-8);
    RestrictToFire(packet);
    POE_CHECK_NEAR(packet.TotalAverage(), 100.0 * 75.0 / 175.0, 1e-8);

    DamagePacket explosion;
    explosion.byType[static_cast<std::size_t>(DamageType::Cold)] = { 100.0, 100.0 };
    ConvertGlobalHitDamage(explosion, passives, false);
    RestrictToFire(explosion);
    POE_CHECK_NEAR(explosion.TotalAverage(), 75.0, 1e-8);
    POE_CHECK_NEAR(explosion.byType[fire].Average(), 75.0, 1e-8);
}

POE_TEST("PoE2 Blood Magic removes mana and retains the existing life cost conversion")
{
    auto input = CombatInput();
    input.passives = Poe2Keystone("Blood Magic");
    const auto after = Snapshot(input);
    POE_CHECK(after->bloodMagic);
    POE_CHECK_NEAR(after->poeMaxMana, 0.0, 1e-8);
    POE_CHECK_NEAR(after->slotStats[1].poeManaCost, 0.0, 1e-8);
    POE_CHECK(ComputeBloodMagicLifeCost(100.0, after->poeMaxLife, after->slotStats[1].poeLifeCost) > 0.0);
}

POE_TEST("PoE2 Chaos Inoculation sets life to one and grants chaos and bleed immunity")
{
    auto input = CombatInput();
    input.passives = Poe2Keystone("Chaos Inoculation");
    const auto after = Snapshot(input);
    POE_CHECK_NEAR(after->poeMaxLife, 1.0, 1e-8);
    POE_CHECK_NEAR(ConvertDefenseResistDamage(20.0, DamageType::Chaos, 1, *after, LoadPack(), Config{}), 0.0, 1e-8);
    AilmentSystem system;
    HitAilmentInput hit;
    hit.physHitPoe = 100.0;
    hit.bleedChancePct = 100.0;
    hit.targetBleedImmune = after->bleedImmune;
    POE_CHECK(!system.ApplyHitAilments(1, hit).appliedBleed);
}

POE_TEST("PoE2 Dance with Death only speeds one-handed attacks and spells with an empty off hand")
{
    auto input = CombatInput();
    const auto before = Snapshot(input);
    input.passives = Poe2Keystone("Dance with Death");
    const auto after = Snapshot(input, Layer2Input{ .mainHandOneHandedMartial = true });
    POE_CHECK_NEAR(after->slotStats[0].attackSpeedMult, before->slotStats[0].attackSpeedMult * 1.25, 1e-8);
    POE_CHECK_NEAR(after->slotStats[1].castSpeedMult, before->slotStats[1].castSpeedMult * 1.25, 1e-8);
    // A torch and an equipped spell both occupy the live off-hand slot.
    const auto occupiedOffHand = Snapshot(input, Layer2Input{ .mainHandOneHandedMartial = true, .offHandOccupied = true });
    POE_CHECK_NEAR(occupiedOffHand->slotStats[1].castSpeedMult, before->slotStats[1].castSpeedMult, 1e-8);
    input.mainWeaponTag = "Staff";
    POE_CHECK_NEAR(Snapshot(input)->slotStats[1].castSpeedMult, before->slotStats[1].castSpeedMult, 1e-8);
    input.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)].isTwoHanded = true;
    POE_CHECK_NEAR(Snapshot(input)->slotStats[1].castSpeedMult, before->slotStats[1].castSpeedMult, 1e-8);
}

POE_TEST("PoE2 Eldritch Battery transfers maximum ES to mana and doubles mana cost")
{
    auto input = CombatInput();
    input.flatEnergyShield = 100.0;
    input.incEnergyShieldPct = 100.0;
    const auto before = Snapshot(input);
    input.passives = Poe2Keystone("Eldritch Battery");
    const auto after = Snapshot(input);
    POE_CHECK_NEAR(after->poeMaxEnergyShield, 0.0, 1e-8);
    POE_CHECK_NEAR(before->poeMaxEnergyShield, 200.0, 1e-8);
    POE_CHECK_NEAR(after->poeMaxMana, before->poeMaxMana + 100.0, 1e-8);
    input.incManaPct = 50.0;
    input.passives.convertEsToManaPct = 0.0;
    const auto scaledMana = Snapshot(input)->poeMaxMana;
    input.passives.convertEsToManaPct = 100.0;
    POE_CHECK_NEAR(Snapshot(input)->poeMaxMana, scaledMana + 150.0, 1e-8);
    POE_CHECK_NEAR(after->slotStats[1].poeManaCost, before->slotStats[1].poeManaCost * 2.0, 1e-8);
}

POE_TEST("PoE2 Glancing Blows gives the probabilities of unlucky evade and lucky deflect")
{
    auto input = CombatInput();
    input.passives = Poe2Keystone("Glancing Blows");
    const auto after = Snapshot(input);
    POE_CHECK_NEAR(ApplyRollBias(0.25, false, after->unluckyEvade), 0.0625, 1e-8);
    POE_CHECK_NEAR(ApplyRollBias(0.25, after->luckyDeflect, false), 0.4375, 1e-8);
}

POE_TEST("PoE2 Iron Reflexes moves all evasion into armour")
{
    auto input = CombatInput();
    const auto before = Snapshot(input);
    input.passives = Poe2Keystone("Iron Reflexes");
    const auto after = Snapshot(input);
    POE_CHECK_NEAR(after->evasion, 0.0, 1e-8);
    POE_CHECK_NEAR(after->armour, before->armour + before->evasion, 1e-8);
}

POE_TEST("PoE2 Mind Over Matter consumes mana before life and halves mana recovery")
{
    auto input = CombatInput();
    const auto before = Snapshot(input);
    input.passives = Poe2Keystone("Mind Over Matter");
    const auto after = Snapshot(input);
    const auto hit = AbsorbWithMindOverMatter(120.0, 1.0, after->momRatio, 90.0);
    POE_CHECK_NEAR(hit.absorbedMagicka, 90.0, 1e-8);
    POE_CHECK_NEAR(hit.remainingRawDamage, 30.0, 1e-8);
    POE_CHECK_NEAR(after->nativeManaRegenMult, before->nativeManaRegenMult * 0.5, 1e-8);
}

POE_TEST("PoE2 Pain Attunement changes only the critical damage bonus according to life")
{
    auto input = CombatInput();
    const double base = Snapshot(input)->slotStats[1].critMultiplierPct - 100.0;
    input.passives = Poe2Keystone("Pain Attunement");
    POE_CHECK_NEAR(Snapshot(input)->slotStats[1].critMultiplierPct, 100.0 + base, 1e-8);
    POE_CHECK_NEAR(Snapshot(input, Layer2Input{ .fullLife = true })->slotStats[1].critMultiplierPct, 100.0 + base * 0.70, 1e-8);
    POE_CHECK_NEAR(Snapshot(input, Layer2Input{ .lowLife = true })->slotStats[1].critMultiplierPct, 100.0 + base * 1.30, 1e-8);
}

POE_TEST("PoE2 life conditions use the rules threshold and the temporary maximum")
{
    const auto snapshot = Snapshot(CombatInput());
    POE_CHECK(!GetPlayerLifeConditions(40.0, 100.0, 100.0, *snapshot).lowLife);
    POE_CHECK(GetPlayerLifeConditions(35.0, 100.0, 100.0, *snapshot).lowLife);
    POE_CHECK(!GetPlayerLifeConditions(120.0, 100.0, 150.0, *snapshot).fullLife);
    POE_CHECK(GetPlayerLifeConditions(150.0, 100.0, 150.0, *snapshot).fullLife);
    StatSnapshot legacy;
    POE_CHECK(GetPlayerLifeConditions(50.0, 100.0, 150.0, legacy).lowLife);
    POE_CHECK(!GetPlayerLifeConditions(51.0, 100.0, 150.0, legacy).lowLife);
}

POE_TEST("PoE2 life regeneration uses distinct life and energy shield units")
{
    StatSnapshot snapshot;
    snapshot.poe2Passives = true;
    snapshot.poeMaxLife = 100.0;
    snapshot.poeMaxEnergyShield = 80.0;
    snapshot.excessLifeRegenToEs = true;
    const auto temporary = ConvertLifeRegeneration(24.0, 120.0, 150.0, 40.0, snapshot);
    POE_CHECK_NEAR(temporary.life, 30.0, 1e-8);
    POE_CHECK_NEAR(temporary.energyShield, 2.0, 1e-8);
    snapshot.chaosInoculation = true;
    snapshot.poeMaxLife = 1.0;
    auto ci = ConvertLifeRegeneration(1.0, 15.0, 15.0, 40.0, snapshot);
    POE_CHECK_NEAR(ci.life, 0.0, 1e-8);
    POE_CHECK_NEAR(ci.energyShield, 0.5, 1e-8);
    snapshot.passiveAilments.esAsFreezeThresholdPct = 100.0;
    POE_CHECK(!PlayerHitMeetsAilmentThreshold(4.49, 15.0, snapshot, DamageType::Cold, 40.0));
    POE_CHECK(PlayerHitMeetsAilmentThreshold(4.5, 15.0, snapshot, DamageType::Cold, 40.0));
}

POE_TEST("PoE2 Price of Freedom applies generic attack efficiency independently to life and mana")
{
    PassiveStatTotals stats;
    POE_CHECK(PassiveStatParser::ParsePoe2Line("15% increased Cost Efficiency of Attacks", stats));
    POE_CHECK(PassiveStatParser::ParsePoe2Line("18% of Skill Mana Costs Converted to Life Costs", stats));
    const auto cost = ComputePoe2SkillCosts(100.0, 0.0, stats, true, false, false);
    POE_CHECK_NEAR(cost.life, 18.0 / 1.15, 1e-8);
    POE_CHECK_NEAR(cost.mana, 71.0, 1e-8);
    POE_CHECK(PassiveStatParser::ParsePoe2Line("10% increased Mana Cost Efficiency", stats));
    const auto manaOnly = ComputePoe2SkillCosts(100.0, 0.0, stats, true, false, false);
    POE_CHECK_NEAR(manaOnly.life, cost.life, 1e-8);
    POE_CHECK_NEAR(manaOnly.mana, 65.0, 1e-8);
    POE_CHECK(PassiveStatParser::ParsePoe2Line("5% increased Life Cost Efficiency", stats));
    const auto lifeOnly = ComputePoe2SkillCosts(100.0, 0.0, stats, true, false, false);
    POE_CHECK_NEAR(lifeOnly.life, 15.0, 1e-8);
    POE_CHECK_NEAR(lifeOnly.mana, manaOnly.mana, 1e-8);
}

POE_TEST("PoE2 evasion conversion and gain use the armour modifiers")
{
    auto input = CombatInput();
    input.passives.incEvasionPct = 100.0;
    input.passives.incArmourPct = 50.0;
    const double baseEvasion = ActiveRuleValues().hit.baseEvasion + input.evasion;
    input.passives.evasionAsExtraArmourPct = 8.0;
    const auto gained = Snapshot(input);
    POE_CHECK_NEAR(gained->evasion, baseEvasion * 2.0, 1e-8);
    POE_CHECK_NEAR(gained->armour, (input.armour + baseEvasion * 0.08) * 1.5, 1e-8);
    input.passives.ironReflexes = true;
    const auto converted = Snapshot(input);
    POE_CHECK_NEAR(converted->evasion, 0.0, 1e-8);
    POE_CHECK_NEAR(converted->armour, (input.armour + baseEvasion * 1.08) * 1.5, 1e-8);
}

POE_TEST("PoE2 increased deflection scales only the defence contribution")
{
    auto input = CombatInput();
    input.deflectionRating = 100.0;
    input.passives.incDeflectionPct = 50.0;
    POE_CHECK_NEAR(Snapshot(input)->deflectionRating, 100.0, 1e-8);
    input.passives.evasionAsDeflectionPct = 10.0;
    const auto gained = Snapshot(input);
    POE_CHECK_NEAR(gained->deflectionRating, 100.0 + gained->evasion * 0.10 * 1.5, 1e-8);
}

POE_TEST("PoE2 uses the native mana regeneration once, including its conditional modifiers")
{
    auto input = CombatInput();
    input.passives.incManaRegenPct = 30.0;
    input.passives.fullLifeManaRegenIncPct = 40.0;
    input.passives.manaRecoveryMult = 0.5;
    const auto snapshot = Snapshot(input, Layer2Input{ .fullLife = true });
    POE_CHECK_NEAR(snapshot->nativeManaRegenMult, 0.85, 1e-8);
    POE_CHECK_NEAR(snapshot->manaRegenPerSec, 0.0, 1e-8);
    input.passives.noInherentManaRegen = true;
    const auto disabled = Snapshot(input);
    POE_CHECK_NEAR(disabled->nativeManaRegenMult, 0.0, 1e-8);
    POE_CHECK_NEAR(disabled->manaRegenPerSec, 0.0, 1e-8);
}

POE_TEST("PoE2 life and motion conditions are consumed by skill and defence stats")
{
    auto input = CombatInput();
    input.passives.fullLifeCastSpeedIncPct = 10.0;
    input.passives.lowLifeCastSpeedIncPct = 20.0;
    input.passives.movingEvasionIncPct = 25.0;
    const auto normal = Snapshot(input);
    const auto full = Snapshot(input, Layer2Input{ .moving = true, .fullLife = true });
    const auto low = Snapshot(input, Layer2Input{ .lowLife = true });
    POE_CHECK_NEAR(full->slotStats[1].castSpeedMult, normal->slotStats[1].castSpeedMult * 1.10, 1e-8);
    POE_CHECK_NEAR(low->slotStats[1].castSpeedMult, normal->slotStats[1].castSpeedMult * 1.20, 1e-8);
    POE_CHECK_NEAR(full->evasion, normal->evasion * 1.25, 1e-8);
}

POE_TEST("PoE2 Resonance exchanges the source of three charge gains without changing existing counts")
{
    auto input = CombatInput();
    input.passives = Poe2Keystone("Resonance");
    const auto after = Snapshot(input);
    POE_CHECK(after->MapChargeGain(ChargeKind::Frenzy) == ChargeKind::Power);
    POE_CHECK(after->MapChargeGain(ChargeKind::Endurance) == ChargeKind::Frenzy);
    POE_CHECK(after->MapChargeGain(ChargeKind::Power) == ChargeKind::Endurance);
}

POE_TEST("PoE2 Zealot's Oath recovers only excess life regeneration and does not recharge ES")
{
    auto input = CombatInput();
    input.passives = Poe2Keystone("Zealot's Oath");
    const auto after = Snapshot(input);
    EnergyShieldPool shield;
    shield.Configure(1.0, 1.0, !after->esCannotRecharge);
    shield.SetMax(100.0);
    (void)shield.AbsorbDamage(60.0, 1.0, false);
    shield.Tick(20.0, false);
    POE_CHECK_NEAR(shield.Snapshot().current, 40.0, 1e-8);
    const auto recovery = SplitLifeRegeneration(24.0, 15.0, after->excessLifeRegenToEs);
    POE_CHECK_NEAR(recovery.life, 15.0, 1e-8);
    const double timer = shield.Snapshot().timeSinceLastHit;
    shield.Recover(recovery.energyShield);
    POE_CHECK_NEAR(shield.Snapshot().current, 49.0, 1e-8);
    POE_CHECK_NEAR(shield.Snapshot().timeSinceLastHit, timer, 1e-8);
    shield.Recover(100.0);
    POE_CHECK_NEAR(shield.Snapshot().current, 100.0, 1e-8);
}

POE_TEST("PoE2 Crimson Assault aggravates stationary bleed for one second at 50 percent more magnitude")
{
    auto input = CombatInput();
    input.passives = Poe2Keystone("Crimson Assault");
    const auto after = Snapshot(input);
    const auto& p = after->passiveAilments;
    HitAilmentInput hit;
    hit.physHitPoe = 100.0;
    hit.bleedChancePct = 100.0;
    hit.aggravatesBleed = p.alwaysAggravateBleed;
    hit.bleedBaseDuration = p.bleedBaseDuration;
    hit.bleedMagnitudeMult = p.bleedMagnitudeMult;
    AilmentSystem system;
    POE_CHECK(system.ApplyHitAilments(1, hit).appliedBleed);
    const auto& rule = ActiveRuleValues().bleed;
    POE_CHECK_NEAR(system.GetState(1)->bleedRemaining, 1.0, 1e-8);
    POE_CHECK_NEAR(system.TickActor(1, 2.0, false).physPoeDamage,
        (rule.damagePercentPerMinute / 60.0) * rule.movingOrAggravatedMultiplier * 1.5, 1e-8);
}

POE_TEST("PoE2 aggregate uses the chosen attribute and class variant from the real tree")
{
    const auto& tree = LoadPoe2Tree();
    const auto* attribute = tree.FindNode(722);
    POE_CHECK(attribute && attribute->attrOptions.size() == 3);
    PassiveTreeState ranger;
    POE_CHECK(PassiveTree::SelectClass(tree, ranger, "cls.Ranger").ok);
    ranger.allocatedNodes = { 722, 56651 };
    for (const auto& option : attribute->attrOptions) {
        ranger.attributeSelections[722] = option.id;
        const auto result = PassiveStatParser::AggregateTree(tree, ranger);
        POE_CHECK_NEAR(result.flatStr + result.flatDex + result.flatInt, 5.0, 1e-8);
        POE_CHECK_NEAR(result.projectileDamageIncPct, 10.0, 1e-8);
    }
    PassiveTreeState huntress;
    POE_CHECK(PassiveTree::SelectClass(tree, huntress, "cls.Huntress").ok);
    huntress.allocatedNodes = { 56651 };
    const auto result = PassiveStatParser::AggregateTree(tree, huntress);
    POE_CHECK_NEAR(result.projectileDamageIncPct, 0.0, 1e-8);
    POE_CHECK_NEAR(result.incAttackDamagePct, 10.0, 1e-8);
}

POE_TEST("PoE2 ordinary block, deflection, critical chance and regeneration modify combat results")
{
    auto input = CombatInput();
    input.attackBlockChance = 20.0;
    const auto before = Snapshot(input);
    for (const auto* line : {
        "10% increased Block Chance",
        "Gain Deflection Rating equal to 50% of Evasion Rating",
        "20% increased Critical Hit Chance",
        "30% increased Mana Regeneration Rate",
        "Regenerate 1% of maximum Life per second",
    }) {
        POE_CHECK(PassiveStatParser::ParsePoe2Line(line, input.passives));
    }
    const auto after = Snapshot(input);
    POE_CHECK_NEAR(after->attackBlockChance, 22.0, 1e-8);
    POE_CHECK_NEAR(after->deflectionRating, before->evasion * 0.5, 1e-8);
    POE_CHECK_NEAR(after->slotStats[0].EffectiveCritChancePct(), before->slotStats[0].EffectiveCritChancePct() * 1.2, 1e-8);
    POE_CHECK_NEAR(after->nativeManaRegenMult, before->nativeManaRegenMult * 1.3, 1e-8);
    POE_CHECK_NEAR(after->lifeRegenPerSec, after->poeMaxLife * 0.01, 1e-8);
    POE_CHECK(PassiveStatParser::ParsePoe2Line("Maximum Physical Damage Reduction is 50%", input.passives));
    const auto capped = Snapshot(input);
    POE_CHECK_NEAR(ArmourPassRatio(100'000.0, 1.0, 0.0, capped->passiveAilments.maximumPhysicalReductionPct), 0.50, 1e-8);
}

POE_TEST("PoE2 penetration applies per hit type against promotion and map resistance")
{
    auto input = CombatInput();
    for (auto* line : { "Damage Penetrates 15% Fire Resistance",
        "Damage Penetrates 10% of Enemy Elemental Resistances", "Damage Penetrates 5% Chaos Resistance" }) {
        POE_CHECK(PassiveStatParser::ParsePoe2Line(line, input.passives));
    }
    const auto snap = Snapshot(input);
    MonsterProfile enemy;
    enemy.modMask = static_cast<std::uint16_t>(1u << static_cast<unsigned>(MonsterModKind::FireResistant));
    DamagePacket packet;
    packet.byType[static_cast<std::size_t>(DamageType::Fire)] = { 70.0, 70.0 };
    packet.byType[static_cast<std::size_t>(DamageType::Cold)] = { 30.0, 30.0 };
    MonsterHitResistance resist;
    resist.poe2 = true;
    resist.mapElementalPct = 20.0;
    resist.mapChaosPct = 12.0;
    resist.penetrationPct = snap->penetrationPct;
    resist.hitTypes = &packet;
    POE_CHECK_NEAR(MonsterModSystem::ApplyPlayerHitToMonster(enemy, 100.0, DamageType::Fire, false, resist)
                       .remainingHealthDamage,
        70.0 * 0.65 + 30.0 * 0.90, 1e-8);
    POE_CHECK_NEAR(MonsterModSystem::IncomingDamageMultiplier(enemy, DamageType::Chaos, resist), 0.93, 1e-8);
    resist.isDamageOverTime = true;
    POE_CHECK_NEAR(MonsterModSystem::IncomingDamageMultiplier(enemy, DamageType::Fire, resist), 0.40, 1e-8);
    POE_CHECK_NEAR(MonsterModSystem::ApplyResistancePenetration(12.0, 25.0), 0.0, 1e-8);
    POE_CHECK_NEAR(MonsterModSystem::ApplyResistancePenetration(-12.0, 25.0), -12.0, 1e-8);
}

POE_TEST("PoE2 enemy ES absorbs chaos at double cost and ailments can bypass it")
{
    MonsterProfile enemy;
    enemy.maxEsSkyrim = enemy.currentEsSkyrim = 100.0;
    MonsterHitResistance resistance;
    resistance.poe2 = true;

    const auto hit = MonsterModSystem::ApplyPlayerHitToMonster(enemy, 30.0, DamageType::Chaos, false, resistance);
    POE_CHECK(hit.hadAnyImpact);
    POE_CHECK_NEAR(enemy.currentEsSkyrim, 40.0, 1e-8);
    POE_CHECK_NEAR(hit.absorbedByEs, 30.0, 1e-8);
    POE_CHECK_NEAR(hit.remainingHealthDamage, 0.0, 1e-8);

    resistance.isDamageOverTime = true;
    enemy.esRechargeCooldown = 1.0;
    const auto bleed = MonsterModSystem::ApplyPlayerHitToMonster(enemy, 10.0, DamageType::Physical, true, resistance);
    const auto poison = MonsterModSystem::ApplyPlayerHitToMonster(enemy, 10.0, DamageType::Chaos, true, resistance);
    POE_CHECK_NEAR(bleed.remainingHealthDamage, 10.0, 1e-8);
    POE_CHECK_NEAR(poison.remainingHealthDamage, 10.0, 1e-8);
    POE_CHECK_NEAR(enemy.currentEsSkyrim, 40.0, 1e-8);
    POE_CHECK_NEAR(enemy.esRechargeCooldown, 1.0, 1e-8);

    const auto chaosIgnite = MonsterModSystem::ApplyPlayerHitToMonster(enemy, 30.0, DamageType::Chaos, false, resistance);
    POE_CHECK_NEAR(chaosIgnite.absorbedByEs, 20.0, 1e-8);
    POE_CHECK_NEAR(chaosIgnite.remainingHealthDamage, 10.0, 1e-8);
    POE_CHECK_NEAR(enemy.currentEsSkyrim, 0.0, 1e-8);
}

POE_TEST("PoE2 mixed enemy hits consume ES per type after resistance")
{
    DamagePacket packet;
    packet.byType[static_cast<std::size_t>(DamageType::Chaos)] = { 45.0, 45.0 };
    packet.byType[static_cast<std::size_t>(DamageType::Fire)] = { 40.0, 40.0 };
    MonsterHitResistance resistance;
    resistance.poe2 = true;
    resistance.mapElementalPct = 25.0;
    resistance.hitTypes = &packet;

    for (const auto representative : { DamageType::Fire, DamageType::Chaos }) {
        MonsterProfile enemy;
        enemy.maxEsSkyrim = enemy.currentEsSkyrim = 100.0;
        const auto hit = MonsterModSystem::ApplyPlayerHitToMonster(enemy, 85.0, representative, false, resistance);
        POE_CHECK_NEAR(enemy.currentEsSkyrim, 0.0, 1e-8);
        POE_CHECK_NEAR(hit.absorbedByEs, 55.0, 1e-8);
        POE_CHECK_NEAR(hit.remainingHealthDamage, 20.0, 1e-8);
    }
}

POE_TEST("Armour break changes only the physical share forwarded to enemy ES")
{
    DamagePacket packet;
    packet.byType[static_cast<std::size_t>(DamageType::Physical)] = { 50.0, 50.0 };
    packet.byType[static_cast<std::size_t>(DamageType::Chaos)] = { 50.0, 50.0 };
    const auto adjusted = MonsterModSystem::AdjustPhysicalHitDamage(packet, 100.0, 110.0);
    MonsterHitResistance resistance;
    resistance.poe2 = true;
    resistance.hitTypes = &adjusted;
    MonsterProfile enemy;
    enemy.maxEsSkyrim = enemy.currentEsSkyrim = 110.0;
    const auto hit = MonsterModSystem::ApplyPlayerHitToMonster(enemy, 110.0, DamageType::Physical, false, resistance);
    POE_CHECK_NEAR(enemy.currentEsSkyrim, 0.0, 1e-8);
    POE_CHECK_NEAR(hit.absorbedByEs, 60.0, 1e-8);
    POE_CHECK_NEAR(hit.remainingHealthDamage, 50.0, 1e-8);
}

POE_TEST("PoE2 gains extra damage from the converted base, uses target modifiers and never cascades gains")
{
    auto input = CombatInput();
    const auto before = Snapshot(input);
    const double fireball = before->slotStats[1].baseHitDamage.TotalAverage();
    input.passives = Poe2Keystone("Blackflame Covenant");
    for (auto* line : { "Gain 20% of Damage as Extra Cold Damage",
        "Gain 30% of Cold Damage as Extra Lightning Damage", "50% increased Cold Damage",
        "100% increased Fire Damage" }) {
        POE_CHECK(PassiveStatParser::ParsePoe2Line(line, input.passives));
    }
    const auto after = Snapshot(input);
    const auto& packet = after->slotStats[1].baseHitDamage;
    POE_CHECK_NEAR(packet.byType[static_cast<std::size_t>(DamageType::Chaos)].Average(), fireball, 1e-8);
    POE_CHECK_NEAR(packet.byType[static_cast<std::size_t>(DamageType::Cold)].Average(), fireball * 0.20 * 1.5, 1e-8);
    POE_CHECK_NEAR(packet.byType[static_cast<std::size_t>(DamageType::Lightning)].Average(), 0.0, 1e-8);
    POE_CHECK(PassiveStatParser::ParsePoe2Line("Gain 9% of Damage as Extra Damage of a random Element", input.passives));
    const auto random = Snapshot(input)->slotStats[1].baseHitDamage;
    POE_CHECK(HasRandomElementExtra(random));
    const auto resolved = ResolveRandomElementExtra(random, 1);
    POE_CHECK_NEAR(resolved.byType[static_cast<std::size_t>(DamageType::Cold)].Average(), fireball * 0.29 * 1.5, 1e-8);
    POE_CHECK(!HasRandomElementExtra(resolved));
}

POE_TEST("PoE2 typed damage gained against an ailment is decided at the target")
{
    auto input = CombatInput();
    POE_CHECK(PassiveStatParser::ParsePoe2Line("Gain 20% of Physical Damage as Extra Cold Damage against Shocked Enemies",
        input.passives));
    const auto stats = Snapshot(input)->slotStats[0];
    const auto roll = RollSpellDamagePacket(stats.baseHitDamage, 0.5, 1.0);
    auto noShock = roll;
    POE_CHECK(!ResolveTargetAilmentExtra(noShock, false, false, true));
    POE_CHECK_NEAR(noShock.TotalAverage(), roll.TotalAverage(), 1e-8);
    auto shocked = roll;
    POE_CHECK(ResolveTargetAilmentExtra(shocked, false, true, false));
    POE_CHECK_NEAR(shocked.byType[static_cast<std::size_t>(DamageType::Cold)].Average(),
        roll.byType[static_cast<std::size_t>(DamageType::Physical)].Average() * 0.20, 1e-8);
    POE_CHECK(!ResolveTargetAilmentExtra(shocked, false, true, false));
}

POE_TEST("PoE2 ailment-conditioned hit increases add to existing increases without amplifying inflicted ailments")
{
    auto input = CombatInput();
    const double base = Snapshot(input)->slotStats[0].baseHitDamage.TotalAverage();
    POE_CHECK(PassiveStatParser::ParsePoe2Line("100% increased Physical Damage", input.passives));
    POE_CHECK(PassiveStatParser::ParsePoe2Line("20% increased Damage with Hits against Enemies affected by Elemental Ailments",
        input.passives));
    const auto stats = Snapshot(input)->slotStats[0];

    DamagePacket hit = stats.baseHitDamage;
    DamagePacket ailmentDamage;
    POE_CHECK(!ResolveTargetAilmentHit(hit, stats.targetAilmentHitDamage, nullptr, ailmentDamage));
    POE_CHECK_NEAR(hit.TotalAverage(), base * 2.0, 1e-8);

    ActorAilmentState state;
    state.activePoisonCount = 1;
    POE_CHECK(!ResolveTargetAilmentHit(hit, stats.targetAilmentHitDamage, &state, ailmentDamage));
    state.igniteRemaining = 1.0;
    state.shockRemaining = 1.0;
    POE_CHECK(ResolveTargetAilmentHit(hit, stats.targetAilmentHitDamage, &state, ailmentDamage));
    POE_CHECK_NEAR(hit.TotalAverage(), base * 2.2, 1e-8);
    POE_CHECK_NEAR(ailmentDamage.TotalAverage(), base * 2.0, 1e-8);
}

POE_TEST("PassiveStatParser: parses core stat lines and skips conditional/weapon-specific/minion lines")
{
    PassiveStatTotals t;
    POE_CHECK(PassiveStatParser::ParseLine("+10 to Strength", t));
    POE_CHECK(PassiveStatParser::ParseLine("+10 to Strength and Intelligence", t));
    POE_CHECK(PassiveStatParser::ParseLine("+8 to all Attributes", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Strength", t));
    POE_CHECK(PassiveStatParser::ParseLine("+30 to maximum Life", t));
    POE_CHECK(PassiveStatParser::ParseLine("6% increased maximum Life", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% more maximum Life", t));
    POE_CHECK(PassiveStatParser::ParseLine("+20 to maximum Mana", t));
    POE_CHECK(PassiveStatParser::ParseLine("8% increased maximum Mana", t));
    POE_CHECK(PassiveStatParser::ParseLine("12% increased maximum Energy Shield", t));
    POE_CHECK(PassiveStatParser::ParseLine("15% increased Energy Shield Recharge Rate", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% faster start of Energy Shield Recharge", t));
    POE_CHECK(PassiveStatParser::ParseLine("+100 to Armour", t));
    POE_CHECK(PassiveStatParser::ParseLine("12% increased Evasion Rating", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Global Defences", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Armour and Evasion", t));
    POE_CHECK(PassiveStatParser::ParseLine("+12% to Fire Resistance", t));
    POE_CHECK(PassiveStatParser::ParseLine("+10% to all Elemental Resistances", t));
    POE_CHECK(PassiveStatParser::ParseLine("+15% to Fire and Cold Resistances", t));
    POE_CHECK(PassiveStatParser::ParseLine("+1% to maximum Fire Resistance", t));
    POE_CHECK(PassiveStatParser::ParseLine("+1% to all maximum Elemental Resistances", t));
    POE_CHECK(PassiveStatParser::ParseLine("+4% Chance to Block Attack Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("+3% Chance to Block Spell Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("+10% chance to Suppress Spell Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("+3% to amount of Suppressed Spell Damage Prevented", t));
    POE_CHECK(PassiveStatParser::ParseLine("3% additional Physical Damage Reduction", t));
    POE_CHECK(PassiveStatParser::ParseLine("12% increased Physical Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("14% increased Melee Physical Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Melee Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("8% increased Elemental Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("10% increased Spell Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("12% increased Area Damage", t));
    POE_CHECK(PassiveStatParser::ParseLine("4% increased Attack Speed", t));
    POE_CHECK(PassiveStatParser::ParseLine("6% increased Cast Speed", t));
    POE_CHECK(PassiveStatParser::ParseLine("25% increased Critical Strike Chance", t));
    POE_CHECK(PassiveStatParser::ParseLine("+15% to Critical Strike Multiplier", t));
    POE_CHECK(PassiveStatParser::ParseLine("8% increased Area of Effect", t));
    POE_CHECK(PassiveStatParser::ParseLine("5% reduced Mana Cost of Skills", t));
    POE_CHECK(PassiveStatParser::ParseLine("+1 to Maximum Endurance Charges", t));

    // 조건부·무기 한정·소환수 스탯은 미매핑(오적용 방지)
    POE_CHECK(!PassiveStatParser::ParseLine("20% increased Damage while Leeching", t));
    POE_CHECK(!PassiveStatParser::ParseLine("10% increased Attack Speed if you've Killed Recently", t));
    POE_CHECK(!PassiveStatParser::ParseLine("30% increased Damage against Chilled Enemies", t));
    POE_CHECK(!PassiveStatParser::ParseLine("10% increased Damage with Wands", t));
    POE_CHECK(!PassiveStatParser::ParseLine("Minions deal 15% increased Damage", t));

    POE_CHECK_NEAR(t.flatStr, 28.0, 1e-9);
    POE_CHECK_NEAR(t.flatDex, 8.0, 1e-9);
    POE_CHECK_NEAR(t.flatInt, 18.0, 1e-9);
    POE_CHECK_NEAR(t.incStrPct, 10.0, 1e-9);
    POE_CHECK_NEAR(t.flatLife, 30.0, 1e-9);
    POE_CHECK_NEAR(t.incLifePct, 6.0, 1e-9);
    POE_CHECK_NEAR(t.moreLifeMult, 1.1, 1e-9);
    POE_CHECK_NEAR(t.incEnergyShieldPct, 22.0, 1e-9);  // 12 + 10 Global Defences
    POE_CHECK_NEAR(t.incArmourPct, 0.0, 1e-9);
    POE_CHECK_NEAR(t.incEvasionPct, 12.0, 1e-9);
    POE_CHECK_NEAR(t.incArmourAndEvasionPct, 20.0, 1e-9);  // 10 Global + 10 Armour & Evasion
    POE_CHECK_NEAR(t.fireResist, 37.0, 1e-9);          // 12 + 10 + 15
    POE_CHECK_NEAR(t.coldResist, 25.0, 1e-9);          // 10 + 15
    POE_CHECK_NEAR(t.lightningResist, 10.0, 1e-9);
    POE_CHECK_NEAR(t.maxFireResistBonus, 2.0, 1e-9);
    POE_CHECK_NEAR(t.maxColdResistBonus, 1.0, 1e-9);
    POE_CHECK_NEAR(t.attackBlockChance, 4.0, 1e-9);
    POE_CHECK_NEAR(t.spellBlockChance, 3.0, 1e-9);
    POE_CHECK_NEAR(t.spellSuppressionChance, 10.0, 1e-9);
    POE_CHECK_NEAR(t.spellSuppressionPrevent, 3.0, 1e-9);
    POE_CHECK_NEAR(t.physDamageReduction, 3.0, 1e-9);
    POE_CHECK_NEAR(t.incPhysicalDamagePct, 12.0, 1e-9);
    POE_CHECK_NEAR(t.incMeleePhysicalDamagePct, 14.0, 1e-9);
    POE_CHECK_NEAR(t.incMeleeDamagePct, 10.0, 1e-9);
    POE_CHECK_NEAR(t.incElementalDamagePct, 8.0, 1e-9);
    POE_CHECK_NEAR(t.incSpellDamagePct, 10.0, 1e-9);
    POE_CHECK_NEAR(t.incAreaDamagePct, 12.0, 1e-9);
    POE_CHECK_NEAR(t.incAttackSpeedPct, 4.0, 1e-9);
    POE_CHECK_NEAR(t.incCastSpeedPct, 6.0, 1e-9);
    POE_CHECK_NEAR(t.incGlobalCritChancePct, 25.0, 1e-9);
    POE_CHECK_NEAR(t.globalCritMultiplierPct, 15.0, 1e-9);
    POE_CHECK_NEAR(t.incAreaOfEffectPct, 8.0, 1e-9);
    POE_CHECK_NEAR(t.reducedManaCostPct, 5.0, 1e-9);
    POE_CHECK(t.maxEnduranceBonus == 1);
}

POE_TEST("PassiveStatParser: AggregateTree recognizes all 10 Stage-3 keystones from real tree_3_30.json")
{
    const auto& tree = LoadTree();
    PassiveTreeState state;
    state.classSelected = true;
    state.allocatedNodes = {
        57279,  // Blood Magic
        11455,  // Chaos Inoculation
        31961,  // Resolute Technique
        10661,  // Iron Reflexes
        40907,  // Unwavering Stance
        54307,  // Acrobatics
        34098,  // Mind Over Matter
        22088,  // Elemental Overload
        31703,  // Pain Attunement
        50288   // Iron Will
    };

    const auto totals = PassiveStatParser::AggregateTree(tree, state);
    POE_CHECK(totals.bloodMagic);
    POE_CHECK_NEAR(totals.moreLifeMult, 1.1, 1e-9);
    POE_CHECK(totals.chaosInoculation);
    POE_CHECK(totals.resoluteTechnique);
    POE_CHECK(totals.ironReflexes);
    POE_CHECK(totals.unwaveringStance);
    POE_CHECK(totals.cannotEvade);
    POE_CHECK(totals.acrobatics);
    POE_CHECK_NEAR(totals.mindOverMatterRatio, 0.40, 1e-9);
    POE_CHECK(totals.elementalOverload);
    POE_CHECK(totals.painAttunement);
    POE_CHECK(totals.ironWill);

    // 검수 Major #4: Precise Technique(63620) 단독 선택 시 Resolute Technique 패널티 오적용 금지
    PassiveTreeState ptState;
    ptState.classSelected = true;
    ptState.allocatedNodes = { 63620 };
    const auto ptTotals = PassiveStatParser::AggregateTree(tree, ptState);
    POE_CHECK(!ptTotals.resoluteTechnique);

    // 검수 Major #1: 실제 tree_3_30.json의 3.30 영문 문구 5종 직접 검증
    PassiveTreeState real330;
    real330.classSelected = true;
    real330.allocatedNodes = {
        9788,   // Nimbleness: +15% to Critical Strike Multiplier for Spell Damage
        5612,   // Physical Attack Damage: 12% increased Attack Physical Damage
        63251,  // Inveterate: Prevent +3% of Suppressed Spell Damage
        5632,   // Bleed and Critical Strike Chance: 15% increased Critical Strike Chance for Attacks
        3533    // Life and Cost Efficiency: 5% increased Cost Efficiency
    };
    const auto r330 = PassiveStatParser::AggregateTree(tree, real330);
    POE_CHECK_NEAR(r330.spellCritMultiplierPct, 15.0, 1e-9);
    POE_CHECK_NEAR(r330.incPhysicalAttackDamagePct, 12.0, 1e-9);
    POE_CHECK_NEAR(r330.spellSuppressionPrevent, 3.0, 1e-9);
    POE_CHECK_NEAR(r330.incAttackCritChancePct, 15.0, 1e-9);
    POE_CHECK_NEAR(r330.costEfficiencyPct, 5.0, 1e-9);
}

POE_TEST("StatEngine: 2-pass attribute convergence, Iron Reflexes, Blood Magic, and dynamic max resists")
{
    const auto& pack = LoadPack();
    Config cfg;
    StatEngine engine;

    Layer1Input l1;
    l1.exileLevel = 10;
    l1.className = "Marauder";  // 32 Str, 14 Dex, 14 Int
    l1.baseWeaponPhysicalMin = 100.0;
    l1.baseWeaponPhysicalMax = 100.0;
    l1.slots[0].slotIndex = 0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 1;
    l1.slots[0].gem.gemId = "Cyclone";
    l1.slots[0].gem.level = 1;

    // 20% 증가와 +68 힘을 합쳐 힘 120, 힘으로부터 생명력 240.
    l1.passives.flatStr = 68.0;
    l1.passives.incStrPct = 20.0;
    l1.passives.flatDex = 86.0;  // 14 + 86 = 100 Dex
    l1.passives.flatArmour = 100.0;
    l1.passives.flatEvasion = 200.0;
    l1.passives.incArmourPct = 50.0;
    l1.passives.incEvasionPct = 50.0;
    l1.passives.ironReflexes = true;  // 회피 -> 방어도 전환
    l1.passives.fireResist = 85.0;
    l1.passives.maxFireResistBonus = 3.0;  // 상한 78%
    l1.passives.coldResist = 95.0;
    l1.passives.maxColdResistBonus = 20.0;  // PoE 하드캡 90%

    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snap = engine.GetSnapshot();

    POE_CHECK(snap->str == 120);
    POE_CHECK(snap->dex == 100);
    POE_CHECK_NEAR(snap->evasion, 0.0, 1e-9);
    POE_CHECK_NEAR(snap->accuracy, 654.0, 1e-9);  // Lv10 기본 54 + 민첩 100 × 6
    // PoE2 기본 회피 7을 반영한다: 100 × 1.5 + (7 + 200) × 2 = 564.
    POE_CHECK_NEAR(snap->armour, 564.0, 1e-9);
    POE_CHECK_NEAR(snap->maxFireResist, 78.0, 1e-9);
    POE_CHECK_NEAR(snap->maxColdResist, 90.0, 1e-9);

    // 동적 최대 저항 반영 검증: 85% 화염 저항 + 78% 상한 -> 22% 통과 (Lv10 기대 저항 100% 기준 비율 검증)
    const double expElem10 = ExpectedElementalPass(10, pack, cfg);
    const double expElem1 = ExpectedElementalPass(1, pack, cfg);
    const double denomElem = std::pow(expElem10, cfg.defenseRollback) * std::pow(expElem1, 1.0 - cfg.defenseRollback);
    const double fireThrough = ConvertDefenseResistDamage(100.0, DamageType::Fire, 10, *snap, pack, cfg);
    POE_CHECK_NEAR(fireThrough, 100.0 * 0.22 / denomElem, 1e-6);
    // 95% 냉기 저항 + 90% 하드캡 -> 10% 통과
    const double coldThrough = ConvertDefenseResistDamage(100.0, DamageType::Cold, 10, *snap, pack, cfg);
    POE_CHECK_NEAR(coldThrough, 100.0 * 0.10 / denomElem, 1e-6);
}

POE_TEST("StatEngine & DamageConversion: Chaos Inoculation, Pain Attunement, Iron Will, RT, EO, Acrobatics, MoM, Blood Magic")
{
    const auto& pack = LoadPack();
    Config cfg;
    StatEngine engine;

    // 1. Chaos Inoculation + Pain Attunement (CI에서는 Low Life여도 Pain Attunement 비활성)
    Layer1Input l1;
    l1.exileLevel = 20;
    l1.className = "Witch";
    l1.slots[0].slotIndex = 0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 10;
    l1.slots[0].gem.gemId = "IceNova";
    l1.slots[0].gem.level = 1;
    l1.passives.chaosInoculation = true;
    l1.passives.painAttunement = true;
    l1.passives.spellSuppressionChance = 100.0;
    l1.passives.acrobatics = true;  // 100% * 0.5 = 50% spell dodge
    engine.SetLayer1(l1);

    Layer2Input l2;
    l2.lowLife = true;
    engine.SetLayer2(l2);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapCi = engine.GetSnapshot();

    POE_CHECK_NEAR(snapCi->poeMaxLife, 1.0, 1e-9);
    POE_CHECK(snapCi->chaosImmune);
    POE_CHECK_NEAR(ConvertDefenseResistDamage(500.0, DamageType::Chaos, 20, *snapCi, pack, cfg), 0.0, 1e-9);
    POE_CHECK_NEAR(ComputeSkyrimMaxHealth(200.0, 1.0, snapCi->expectedPoeLife, true), 25.0, 1e-9);
    POE_CHECK_NEAR(ComputeSkyrimMaxHealth(100.0, 1.0, snapCi->expectedPoeLife, true), 15.0, 1e-9);
    POE_CHECK_NEAR(snapCi->spellSuppressionChance, 0.0, 1e-9);
    POE_CHECK_NEAR(snapCi->spellDodgeChance, 0.0, 1e-9);

    // CI 해제 시 Low Life + Pain Attunement -> 주문 피해 1.3배
    const double iceDmgUnderCi = snapCi->slotStats[0].baseHitDamage.TotalAverage();
    l1.passives.chaosInoculation = false;
    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapPa = engine.GetSnapshot();
    POE_CHECK_NEAR(snapPa->slotStats[0].baseHitDamage.TotalAverage(), iceDmgUnderCi * 1.3, 1e-6);

    // 2. Iron Will + Elemental Overload + Resolute Technique + Unwavering Stance
    Layer1Input l1b;
    l1b.exileLevel = 20;
    l1b.className = "Marauder";
    l1b.passives.flatStr = 68.0; // 힘 100
    l1b.baseWeaponPhysicalMin = 50.0;
    l1b.baseWeaponPhysicalMax = 50.0;
    l1b.slots[0].slotIndex = 0;
    l1b.slots[0].occupied = true;
    l1b.slots[0].gem.instanceId = 20;
    l1b.slots[0].gem.gemId = "Cyclone";
    l1b.slots[0].gem.level = 1;
    l1b.slots[1].slotIndex = 1;
    l1b.slots[1].occupied = true;
    l1b.slots[1].gem.instanceId = 21;
    l1b.slots[1].gem.gemId = "IceNova";
    l1b.slots[1].gem.level = 1;
    engine.SetLayer1(l1b);
    engine.SetLayer2(Layer2Input{});
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapBase = engine.GetSnapshot();

    l1b.passives.ironWill = true;
    l1b.passives.resoluteTechnique = true;
    l1b.passives.elementalOverload = true;
    l1b.passives.unwaveringStance = true;
    l1b.passives.cannotEvade = true;
    l1b.passives.flatEvasion = 500.0;
    l1b.passives.incCastSpeedPct = 20.0;
    l1b.passives.costEfficiencyPct = 25.0;  // cost / 1.25
    engine.SetLayer1(l1b);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapKs = engine.GetSnapshot();

    // PoE2 힘은 Iron Will에 전달될 근접 피해 보너스가 없다.
    POE_CHECK_NEAR(
        snapKs->slotStats[1].baseHitDamage.TotalAverage(),
        snapBase->slotStats[1].baseHitDamage.TotalAverage(),
        1e-6);
    POE_CHECK_NEAR(snapKs->slotStats[1].castSpeedMult, 1.20, 1e-9);
    POE_CHECK_NEAR(snapKs->slotStats[1].poeManaCost, snapBase->slotStats[1].poeManaCost / 1.25, 1e-6);
    // Resolute Technique -> PoE 3.30 원작 규칙: 공격(Cyclone)·주문(IceNova) 모두 치명타 0%
    POE_CHECK_NEAR(snapKs->slotStats[0].baseCritChancePct, 0.0, 1e-9);
    POE_CHECK_NEAR(snapKs->slotStats[1].baseCritChancePct, 0.0, 1e-9);
    // Elemental Overload -> 치명타 배율 100%, 최근 치명타 활성 시 원소 피해 1.40배
    POE_CHECK_NEAR(snapKs->slotStats[1].critMultiplierPct, 100.0, 1e-9);
    Layer2Input l2Eo;
    l2Eo.elementalOverloadActive = true;
    engine.SetLayer2(l2Eo);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapEo = engine.GetSnapshot();
    POE_CHECK_NEAR(
        snapEo->slotStats[1].baseHitDamage.TotalAverage(),
        snapKs->slotStats[1].baseHitDamage.TotalAverage() * 1.40,
        1e-6);
    engine.SetLayer2(Layer2Input{});
    // Unwavering Stance -> 기절 면역 + 회피 0
    POE_CHECK_NEAR(snapKs->evasion, 0.0, 1e-9);
    POE_CHECK(snapKs->unwaveringStance);

    // Juggernaut "Cannot be Stunned"만 있을 때(cannotEvade == false)는 회피가 유지돼야 함
    l1b.passives.cannotEvade = false;
    engine.SetLayer1(l1b);
    POE_CHECK(engine.RebuildIfDirty(pack, cfg));
    const auto snapJug = engine.GetSnapshot();
    POE_CHECK(snapJug->unwaveringStance);
    POE_CHECK(snapJug->evasion > 500.0);

    // 3. Mind Over Matter 난이도 배율 정렬 및 음수 마나 클램프
    const auto momFull = AbsorbWithMindOverMatter(100.0, 1.0, 0.40, 200.0);
    POE_CHECK_NEAR(momFull.absorbedMagicka, 40.0, 1e-9);
    POE_CHECK_NEAR(momFull.remainingRawDamage, 60.0, 1e-9);

    // 전설 난이도(3.0배): incoming=100 -> 실제 체력 피해 300 -> 40% 흡수 목표 120 매지카 -> 매지카 50뿐이면 50 흡수 -> 체력 피해 250 -> 원시 피해 250/3
    const auto momLegendary = AbsorbWithMindOverMatter(100.0, 3.0, 0.40, 50.0);
    POE_CHECK_NEAR(momLegendary.absorbedMagicka, 50.0, 1e-9);
    POE_CHECK_NEAR(momLegendary.remainingRawDamage, 250.0 / 3.0, 1e-6);

    // 음수 매지카 입력 시 0 흡수
    const auto momNeg = AbsorbWithMindOverMatter(100.0, 1.0, 0.40, -10.0);
    POE_CHECK_NEAR(momNeg.absorbedMagicka, 0.0, 1e-9);
    POE_CHECK_NEAR(momNeg.remainingRawDamage, 100.0, 1e-9);

    // 4. Blood Magic 생명력 비용 환산 (skyrimMaxHealth=300, poeMaxLife=200, poeSkillCost=20 -> 30 HP)
    POE_CHECK_NEAR(ComputeBloodMagicLifeCost(300.0, 200.0, 20.0), 30.0, 1e-9);
}

POE_TEST("Config: parses [PassiveTree] section from SkyrimPoE.ini")
{
    const auto iniPath = std::filesystem::path(POE_TEST_PLUGIN_DIR) / "dist" / "SKSE" / "Plugins" / "SkyrimPoE.ini";
    const auto loaded = LoadConfigFromFile(iniPath);
    POE_CHECK(loaded.fileFound);
    const auto& cfg = loaded.config;
    POE_CHECK(cfg.treePanelKey == 0x26);
    POE_CHECK(!cfg.freePassiveRefund);
    POE_CHECK(cfg.simpleImeMode == "auto");
    POE_CHECK(cfg.gearPanelKey == 0x18);
    POE_CHECK(cfg.ascendancyRefundCost == 5);
    POE_CHECK(cfg.worldDropEnabled);
    POE_CHECK(cfg.worldDropMaxCount == 16);
    POE_CHECK(cfg.bonusPassivePoints == 0);
    POE_CHECK(cfg.bonusAscendancyPoints == 0);
}

POE_TEST("PassiveStatParser: every ailment-chance line in real tree_3_30.json is parsed (review R-1)")
{
    // 재검수 R-1: 트리 원문의 상태이상 확률 줄(예: "Attacks have 15% chance to cause Bleeding")이
    // 하나도 빠짐없이 파싱되는지 실제 데이터로 확인한다. 시험용으로 지어낸 문구로는 이 결함을 잡지 못했다.
    const auto& tree = LoadTree();
    std::size_t ailmentLines = 0;
    std::size_t bleedLines = 0;
    std::size_t attackIgniteLines = 0;
    for (const auto& [id, node] : tree.AllNodes()) {
        for (const auto& line : node.stats) {
            std::string low(line);
            for (auto& c : low) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const bool simpleChanceLine =
                low.find("chance to cause bleeding") != std::string::npos ||
                low.find("chance to ignite") != std::string::npos ||
                low.find("chance to poison on hit") != std::string::npos;
            // 조건부 문구(저주받은 적, 냉각된 적, 주기 발동 등)는 단순 전역 확률이 아니므로 제외한다.
            if (!simpleChanceLine || low.find("enemies") != std::string::npos || low.find("every ") != std::string::npos) {
                continue;
            }
            ++ailmentLines;
            PassiveStatTotals one;
            const bool parsed = PassiveStatParser::ParseLine(line, one);
            if (!parsed) {
                std::fprintf(stderr, "  unparsed ailment line (node %u): %s\n", id, line.c_str());
            }
            POE_CHECK(parsed);
            if (low.starts_with("attacks have") && low.find("bleeding") != std::string::npos) {
                ++bleedLines;
                POE_CHECK(one.attackBleedChancePct > 0.0 && one.bleedChancePct == 0.0);
            }
            if (low.starts_with("attacks have") && low.find("ignite") != std::string::npos) {
                ++attackIgniteLines;
                POE_CHECK(one.attackIgniteChancePct > 0.0 && one.igniteChancePct == 0.0);
            }
        }
    }
    // PoE 3.30 트리 기준: 출혈 12줄(15% 9, 10% 3), 공격 점화 7줄
    POE_CHECK(bleedLines == 12);
    POE_CHECK(attackIgniteLines == 7);
    POE_CHECK(ailmentLines >= bleedLines + attackIgniteLines);

    // 공격·주문 한정 중독은 각자의 칸으로 간다.
    PassiveStatTotals poison;
    POE_CHECK(PassiveStatParser::ParseLine("10% chance to Poison on Hit with Attacks", poison));
    POE_CHECK(PassiveStatParser::ParseLine("15% chance to Poison on Hit with Spell Damage", poison));
    POE_CHECK(PassiveStatParser::ParseLine("20% chance to Poison on Hit", poison));
    POE_CHECK_NEAR(poison.attackPoisonChancePct, 10.0, 1e-9);
    POE_CHECK_NEAR(poison.spellPoisonChancePct, 15.0, 1e-9);
    POE_CHECK_NEAR(poison.poisonChancePct, 20.0, 1e-9);
}
