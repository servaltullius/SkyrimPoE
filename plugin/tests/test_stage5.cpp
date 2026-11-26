#include "test_harness.h"

#include "core/AilmentSystem.h"
#include "core/CharmSystem.h"
#include "core/Config.h"
#include "core/DataPack.h"
#include "core/ItemAffixParser.h"
#include "core/ItemCrafting.h"
#include "core/ItemData.h"
#include "core/ItemFormatter.h"
#include "core/LootTable.h"
#include "core/MonsterModSystem.h"
#include "core/RulesPack.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"

#include <cmath>
#include <filesystem>
#include <random>

namespace
{
    using namespace SkyrimPoE::Core;

    const ItemData& Stage5ItemData()
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

    const DataPack& Stage5CorePack()
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
}

POE_TEST("Stage5 ItemData v2: belts, charms, monsterMods, 16 uniques, and jewel isolation")
{
    const auto& data = Stage5ItemData();
    POE_CHECK(data.AllBelts().size() == 5);
    POE_CHECK(data.AllCharms().size() == 6);
    POE_CHECK(data.AllMonsterMods().size() == 12);
    POE_CHECK(data.AllUniques().size() == 16);

    POE_CHECK(data.FindBelt("StygianVise") != nullptr && data.FindBelt("StygianVise")->bonusCharmSlot);
    POE_CHECK(data.FindBeltByLocalFormId(0x890) != nullptr && data.FindBeltByLocalFormId(0x890)->id == "LeatherBelt");
    POE_CHECK(data.FindCharm(CharmKind::Bastion) != nullptr);
    POE_CHECK(data.FindUnique("Headhunter") != nullptr);
    POE_CHECK(data.FindUnique("Starforge") != nullptr);
    POE_CHECK(data.FindUnique("ShavronnesWrappings") != nullptr);
    POE_CHECK(data.FindUnique("Praxis") != nullptr);
    POE_CHECK(data.FindUnique("PrismGuardian") != nullptr);

    // Jewel tags must NOT include "default" so normal armor/weapon mods cannot roll on jewels!
    const auto crimsonTags = ItemData::BuildItemTags(TokenSlotGroup::Jewel, DefenseSubtype::None, "", false, 0);
    POE_CHECK(crimsonTags.contains("jewel"));
    POE_CHECK(crimsonTags.contains("not_dex"));
    POE_CHECK(crimsonTags.contains("not_int"));
    POE_CHECK(!crimsonTags.contains("default"));

    const auto* life0 = data.FindAffix("IncreasedLife0");
    POE_CHECK(life0 != nullptr && ItemData::ResolveWeight(*life0, crimsonTags) == 0);

    const auto jewelPrefixes = data.CollectCandidates(AffixType::Prefix, 80, crimsonTags, {});
    const auto jewelSuffixes = data.CollectCandidates(AffixType::Suffix, 80, crimsonTags, {});
    POE_CHECK(!jewelPrefixes.empty() && !jewelSuffixes.empty());
}

POE_TEST("Stage5 Crafting: Belt implicit preservation, Jewel 2P/2S cap, Unique Divine-only & special flags")
{
    const auto& data = Stage5ItemData();
    std::mt19937_64 rng(0x55AA1234ULL);

    // 1. Belt implicit survives Alteration, Chaos, Scouring!
    const auto* leatherBelt = data.FindBelt("LeatherBelt");
    POE_CHECK(leatherBelt != nullptr);
    auto beltRec = ItemCrafting::IdentifyToken(
        4, ItemRarity::Rare, TokenSlotGroup::Belt, DefenseSubtype::None, "", false,
        leatherBelt->localFormId, 70, data, rng, leatherBelt->implicitAffixId);
    POE_CHECK(beltRec.implicitAffixes.size() == 1);
    POE_CHECK(beltRec.implicitAffixes[0].affixId == "Implicit_Belt_LeatherBelt");
    const auto beltTags = ItemData::BuildItemTags(TokenSlotGroup::Belt, DefenseSubtype::None, "");

    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Chaos, beltRec, beltTags, data, rng).IsOk());
    POE_CHECK(beltRec.implicitAffixes.size() == 1);
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Scouring, beltRec, beltTags, data, rng).IsOk());
    POE_CHECK(beltRec.rarity == ItemRarity::Normal);
    POE_CHECK(beltRec.affixes.empty());
    POE_CHECK(beltRec.implicitAffixes.size() == 1);  // Implicit preserved!
    const auto beltStatsAfterScour = ItemAffixParser::ParseItem(beltRec, data);
    POE_CHECK(beltStatsAfterScour.flatLife >= 25.0 && beltStatsAfterScour.flatLife <= 40.0);

    // Belt charm slot tiers
    POE_CHECK(ItemCrafting::ComputeBeltCharmSlots(false, nullptr, false) == 0);
    POE_CHECK(ItemCrafting::ComputeBeltCharmSlots(true, &beltRec, false) == 1);  // Normal -> 1
    POE_CHECK(ItemCrafting::ComputeBeltCharmSlots(true, &beltRec, true) == 3);   // Stygian -> 3
    beltRec.rarity = ItemRarity::Rare;
    beltRec.itemLevel = 35;  // T2 Rare -> 2
    POE_CHECK(ItemCrafting::ComputeBeltCharmSlots(true, &beltRec, false) == 2);
    beltRec.itemLevel = 70;  // T4 Rare -> 3
    POE_CHECK(ItemCrafting::ComputeBeltCharmSlots(true, &beltRec, false) == 3);

    // 2. Jewel rare cap is 2P / 2S (max 4 affixes)
    PoEItemRecord jewel;
    jewel.baseFormId = 0x89F;
    jewel.rarity = ItemRarity::Normal;
    jewel.itemLevel = 80;
    jewel.slotGroup = TokenSlotGroup::Jewel;
    jewel.jewelColor = 0;  // Crimson
    const auto jTags = ItemData::BuildItemTags(TokenSlotGroup::Jewel, DefenseSubtype::None, "", false, 0);
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Alchemy, jewel, jTags, data, rng).IsOk());
    POE_CHECK(jewel.rarity == ItemRarity::Rare);
    POE_CHECK(jewel.affixes.size() >= 3 && jewel.affixes.size() <= 4);
    while (jewel.affixes.size() < 4) {
        POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Exalted, jewel, jTags, data, rng).IsOk());
    }
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Exalted, jewel, jTags, data, rng).code == CraftResultCode::AffixLimitReached);

    // 3. Unique items: Divine-only guard + Formatter [고유] + Special flags (Starforge, Praxis, Shavronne, Headhunter)
    const auto* starforgeDef = data.FindUnique("Starforge");
    POE_CHECK(starforgeDef != nullptr);
    auto sfRec = ItemCrafting::RollUniqueItem(*starforgeDef, starforgeDef->baseFormId, 80, data, rng);
    POE_CHECK(sfRec.rarity == ItemRarity::Unique);
    POE_CHECK(sfRec.uniqueId == "Starforge");
    POE_CHECK(!sfRec.implicitAffixes.empty() && !sfRec.affixes.empty());

    const auto sfTags = ItemData::BuildItemTags(sfRec.slotGroup, sfRec.defenseSubtype, sfRec.weaponTag);
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Chaos, sfRec, sfTags, data, rng).code == CraftResultCode::InvalidRarity);
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Scouring, sfRec, sfTags, data, rng).code == CraftResultCode::InvalidRarity);
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Exalted, sfRec, sfTags, data, rng).code == CraftResultCode::InvalidRarity);
    POE_CHECK(ItemCrafting::ApplyOrb(CurrencyKind::Divine, sfRec, sfTags, data, rng).IsOk());

    const auto sfStats = ItemAffixParser::ParseItem(sfRec, data);
    POE_CHECK(sfStats.localIncPhysPct >= 400.0 && sfStats.localIncPhysPct <= 450.0);
    POE_CHECK_NEAR(sfStats.incPhysicalDamagePct, 30.0, 1e-6);  // Implicit 30% Global Phys
    POE_CHECK(sfStats.starforgeShock);
    POE_CHECK(sfStats.dealNoElemental);

    EquippedSlotInput sfSlot;
    sfSlot.occupied = true;
    sfSlot.isCrafted = true;
    sfSlot.isWeapon = true;
    sfSlot.stats = sfStats;
    const auto sfCard = ItemFormatter::FormatItem(&sfRec, "양손검", EquipSlot::MainHand, sfSlot, true, data);
    POE_CHECK(sfCard.rarityBadge == "[고유]");
    POE_CHECK(!sfCard.implicitLines.empty());
    POE_CHECK(sfCard.implicitLines[0].tierBadge == "[고정]");
    POE_CHECK(!sfCard.affixLines.empty() && sfCard.affixLines[0].tierBadge == "[고유]");

    // Praxis flat mana cost reduction
    const auto* praxisDef = data.FindUnique("Praxis");
    POE_CHECK(praxisDef != nullptr);
    const auto praxisRec = ItemCrafting::RollUniqueItem(*praxisDef, praxisDef->baseFormId, 40, data, rng);
    const auto praxisStats = ItemAffixParser::ParseItem(praxisRec, data);
    POE_CHECK(praxisStats.flatManaCostDelta <= -4.0 && praxisStats.flatManaCostDelta >= -8.0);
    POE_CHECK(praxisStats.manaRegenPerSec >= 3.0 && praxisStats.manaRegenPerSec <= 6.0);
}

POE_TEST("Stage5 StatEngine: Spirit reservation, auto-disable, 10-slot Ring2/Belt, pre-loop Jewels, 28-gem skills")
{
    const auto& core = Stage5CorePack();
    const auto& itemData = Stage5ItemData();
    Config cfg;
    POE_CHECK(cfg.enforceGemRequirements);  // U2 확정 기본값 true 확인
    cfg.enforceGemRequirements = false;     // 이 테스트는 일부러 Dex=20으로 방패 reqDex=50 실패를 검증하므로 젬 요구치 분리
    StatEngine engine;

    Layer1Input l1;
    l1.exileLevel = 60;
    l1.className = "Scion";  // 20/20/20
    l1.achievementMask = 0x3;  // 100 + 60 = 160 Spirit

    // Passive Jewel gives +40 Str BEFORE gear fixed-point loop -> satisfies Body reqStr=55!
    ItemStatTotals jewel;
    jewel.flatStr = 40.0;
    jewel.incSpellDamagePct = 15.0;
    l1.socketedJewelStats.push_back(jewel);

    auto& body = l1.equippedGear[static_cast<std::size_t>(EquipSlot::Body)];
    body.occupied = true;
    body.isCrafted = true;
    body.reqLevel = 40;
    body.reqStr = 55;
    body.baseArmour = 500.0;

    // Virtual Ring2 (Praxis -6 mana cost) + Belt (+50 Life)
    auto& ring2 = l1.equippedGear[static_cast<std::size_t>(EquipSlot::Ring2)];
    ring2.occupied = true;
    ring2.isCrafted = true;
    ring2.stats.flatManaCostDelta = -6.0;
    ring2.stats.fireResist = 25.0;

    auto& belt = l1.equippedGear[static_cast<std::size_t>(EquipSlot::Belt)];
    belt.occupied = true;
    belt.isCrafted = true;
    belt.stats.flatLife = 60.0;

    // Prism Guardian in OffHand (+40 Spirit, reqDex=50)
    auto& shield = l1.equippedGear[static_cast<std::size_t>(EquipSlot::OffHand)];
    shield.occupied = true;
    shield.isCrafted = true;
    shield.reqDex = 50;
    shield.stats.flatSpirit = 40.0;

    // Slot 0: Fireball + GMP + Chain + SpellEcho
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 10;
    l1.slots[0].gem.gemId = "Fireball";
    l1.slots[0].gem.level = 10;
    l1.slots[0].gem.socketCount = 4;
    l1.slots[0].gem.socketedGems = {
        GemInstance{ 11, "SupportGreaterMultipleProjectiles", 10, 0, 0, 0, {} },
        GemInstance{ 12, "SupportChain", 10, 0, 0, 0, {} },
        GemInstance{ 13, "SupportSpellEcho", 10, 0, 0, 0, {} }
    };

    // Slot 1: Determination (50), Slot 2: PurityOfElements (50), Slot 3: Discipline (35), Slot 4: Grace (50) -> Total 185 > 160!
    // Slot 5: Duplicate Determination Lv1 (must deduplicate cleanly!)
    // Slot 6: MoltenShell
    auto setSlot = [&](std::size_t idx, std::uint64_t id, const char* gid, std::uint8_t lv) {
        l1.slots[idx].occupied = true;
        l1.slots[idx].gem.instanceId = id;
        l1.slots[idx].gem.gemId = gid;
        l1.slots[idx].gem.level = lv;
        l1.slots[idx].gem.socketCount = 3;
    };
    setSlot(1, 21, "Determination", 10);
    setSlot(2, 22, "PurityOfElements", 10);
    setSlot(3, 23, "Discipline", 10);
    setSlot(4, 24, "Grace", 10);
    setSlot(5, 25, "Determination", 1);
    setSlot(6, 26, "MoltenShell", 10);

    l1.activeAuraMask = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5);

    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    auto snap = engine.GetSnapshot();

    // Jewel Str unlocked Body, Shield failed reqDex=50 (20 < 50) -> maxSpirit = 160!
    POE_CHECK(snap->gearSlotActive[static_cast<std::size_t>(EquipSlot::Body)]);
    POE_CHECK(snap->gearSlotActive[static_cast<std::size_t>(EquipSlot::Ring2)]);
    POE_CHECK(snap->gearSlotActive[static_cast<std::size_t>(EquipSlot::Belt)]);
    POE_CHECK(!snap->gearSlotActive[static_cast<std::size_t>(EquipSlot::OffHand)]);
    POE_CHECK(snap->maxSpirit == 160);
    // Slot 4 Grace (50) auto-disabled from highest slot down -> 50 + 50 + 35 = 135 <= 160! Duplicate slot 5 ignored!
    POE_CHECK(snap->reservedSpirit == 135);
    POE_CHECK((snap->effectiveActiveAuraMask & (1u << 1)) != 0);
    POE_CHECK((snap->effectiveActiveAuraMask & (1u << 2)) != 0);
    POE_CHECK((snap->effectiveActiveAuraMask & (1u << 3)) != 0);
    POE_CHECK((snap->effectiveActiveAuraMask & (1u << 4)) == 0);
    POE_CHECK((snap->effectiveActiveAuraMask & (1u << 5)) == 0);
    POE_CHECK(snap->elementalAilmentImmune);
    POE_CHECK(snap->cannotBeFrozen);
    POE_CHECK(snap->armour > 500.0 * 1.4);  // Determination flat + MORE armour!
    POE_CHECK(snap->poeMaxEnergyShield > 100.0);  // Discipline flat ES!

    // Fireball supports + Praxis flat mana cost reduction
    const auto& fb = snap->slotStats[0];
    POE_CHECK(fb.projectileCount == 5);  // 1 + 4 GMP
    POE_CHECK(fb.chainCount == 2);       // +2 Chain
    POE_CHECK(fb.spellRepeatCount == 1); // +1 SpellEcho
    POE_CHECK(fb.castSpeedMult > 1.35);  // SpellEcho MORE cast speed
    POE_CHECK_NEAR(fb.igniteChancePct, 25.0, 1e-6);

    // MoltenShell guard stats
    const auto& ms = snap->slotStats[6];
    POE_CHECK(ms.kind == GemKind::Guard);
    POE_CHECK_NEAR(ms.moltenShellAbsorbPct, 75.0, 1e-6);
    POE_CHECK(ms.moltenShellAbsorbCapPoe > 50.0);
    POE_CHECK(ms.cooldownSec >= 4.0);

    // Satisfy Prism Guardian reqDex=50 -> maxSpirit becomes 200 -> Grace turns ON automatically!
    l1.equippedGear[static_cast<std::size_t>(EquipSlot::Ring2)].stats.flatDex = 35.0;
    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    snap = engine.GetSnapshot();
    POE_CHECK(snap->gearSlotActive[static_cast<std::size_t>(EquipSlot::OffHand)]);
    POE_CHECK(snap->maxSpirit == 200);
    POE_CHECK(snap->reservedSpirit == 185);
    POE_CHECK((snap->effectiveActiveAuraMask & (1u << 4)) != 0);

    // Unused variable guard
    POE_CHECK(itemData.IsLoaded());
}

POE_TEST("P2b physical spell hits use global bleed chance and exclude attack-only chance")
{
    using namespace SkyrimPoE::Core;

    Layer1Input input;
    POE_CHECK(PassiveStatParser::ParseLine("40% chance to cause Bleeding", input.passives));
    POE_CHECK(PassiveStatParser::ParseLine("Attacks have 35% chance to cause Bleeding", input.passives));
    POE_CHECK(PassiveStatParser::ParseLine("25% chance to cause Bleeding with Attacks", input.passives));

    StatEngine engine;
    engine.SetLayer1(input);
    POE_CHECK(engine.RebuildIfDirty(Stage5CorePack(), Config{}));
    const auto snapshot = engine.GetSnapshot();
    POE_CHECK(snapshot != nullptr);
    POE_CHECK_NEAR(snapshot->bleedChancePct, 40.0, 1e-9);
    POE_CHECK_NEAR(snapshot->attackBleedChancePct, 60.0, 1e-9);

    AilmentSystem ailments;
    HitAilmentInput hit;
    hit.physHitPoe = 100.0;
    hit.bleedChancePct = snapshot->BleedChanceForHitPct(false);
    hit.bleedRoll01 = 0.25;
    POE_CHECK(ailments.ApplyHitAilments(1, hit).appliedBleed);
    POE_CHECK_NEAR(ailments.TickActor(1, 1.0, false).physPoeDamage, 15.0, 1e-9);

    hit.bleedRoll01 = 0.75;
    POE_CHECK(!ailments.ApplyHitAilments(2, hit).appliedBleed);
    hit.bleedChancePct = snapshot->BleedChanceForHitPct(true);
    POE_CHECK(ailments.ApplyHitAilments(3, hit).appliedBleed);
}

POE_TEST("PoE2 chill applies increased effect before both limits")
{
    AilmentSystem ailments;
    HitAilmentInput hit;
    hit.targetAilmentThresholdPoe = 100.0;
    hit.ailmentMagnitudeIncPct = 40.0;
    const auto& rule = ActiveRuleValues().chill;

    hit.coldHitPoe = 50.0 * hit.targetAilmentThresholdPoe / rule.effectMultiplier;
    POE_CHECK(ailments.ApplyHitAilments(1, hit).appliedChill);
    POE_CHECK_NEAR(ailments.ChillSlowRatio(1), 0.50, 1e-8);

    hit.coldHitPoe = 25.0 * hit.targetAilmentThresholdPoe / rule.effectMultiplier;
    POE_CHECK(ailments.ApplyHitAilments(2, hit).appliedChill);
    POE_CHECK_NEAR(ailments.ChillSlowRatio(2), 0.35, 1e-8);
}

POE_TEST("Stage5 CharmSystem, AilmentSystem, MonsterModSystem, and MoltenShell Guard lifecycle")
{
    const auto& core = Stage5CorePack();
    const auto& itemData = Stage5ItemData();
    Config cfg;

    // 1. CharmSystem auto-trigger + kill recharge + expiry
    CharmSystem charms;
    std::array<CharmSlotState, kMaxCharmSlots> cSlots{};
    cSlots[0] = CharmSlotState{ true, CharmKind::Ruby, 40.0 };
    cSlots[1] = CharmSlotState{ true, CharmKind::Purifying, 20.0 };
    cSlots[2] = CharmSlotState{ true, CharmKind::Bastion, 40.0 };
    charms.SetSlots(cSlots, 3);

    POE_CHECK(charms.TryTriggerOnIncomingDamage(DamageType::Fire, true));
    POE_CHECK(!charms.TryTriggerOnIncomingDamage(DamageType::Fire, true));  // Already active, don't double consume
    POE_CHECK_NEAR(charms.Slots()[0].charges, 20.0, 1e-6);
    POE_CHECK(charms.TryTriggerOnAilment(AilmentKind::Freeze, true));
    POE_CHECK(charms.TryTriggerOnLowLife(0.35, true));
    const auto cb = charms.ComputeActiveBonuses();
    POE_CHECK_NEAR(cb.fireResist, 25.0, 1e-6);
    POE_CHECK_NEAR(cb.maxFireResist, 2.0, 1e-6);
    POE_CHECK(cb.freezeImmune && cb.bleedImmune);
    POE_CHECK_NEAR(cb.physDamageReduction, 15.0, 1e-6);
    POE_CHECK_NEAR(cb.flatArmour, 600.0, 1e-6);

    charms.OnKillRecharge(MonsterRarity::Rare, false, false);  // +8
    POE_CHECK_NEAR(charms.Slots()[0].charges, 28.0, 1e-6);
    charms.OnKillRecharge(MonsterRarity::Normal, true, false); // Boss +15 -> clamped 40
    POE_CHECK_NEAR(charms.Slots()[0].charges, 40.0, 1e-6);
    POE_CHECK(charms.Tick(4.5, false));  // Expired
    POE_CHECK_NEAR(charms.ComputeActiveBonuses().fireResist, 0.0, 1e-6);

    // 2. AilmentSystem: PoE2 boss buildup, moving bleed x2, Starforge physical shock
    AilmentSystem ailments;
    HitAilmentInput hit{};
    hit.physHitPoe = 100.0;
    hit.fireHitPoe = 100.0;
    hit.coldHitPoe = 100.0;
    hit.lightningHitPoe = 0.0;
    hit.chaosHitPoe = 50.0;
    hit.targetAilmentThresholdPoe = 200.0;
    hit.isCrit = true;
    hit.starforgeShock = true;
    hit.bleedChancePct = 100.0;
    hit.poisonChancePct = 100.0;
    hit.igniteRoll01 = 0.09;
    hit.shockRoll01 = 0.09;

    // Normal mob freezes
    const auto rMob = ailments.ApplyHitAilments(1001, hit);
    POE_CHECK(rMob.appliedIgnite && rMob.appliedChill && rMob.appliedFreeze && rMob.appliedShock && rMob.appliedBleed && rMob.appliedPoison);
    POE_CHECK(ailments.IsFrozen(1001));
    POE_CHECK_NEAR(ailments.ShockDamageMultiplier(1001), 1.20, 1e-6);

    // The unique threshold takes three hits to fill. The unique slow reduction halves 50% chill.
    hit.isBossOrDragon = true;
    const auto rBoss = ailments.ApplyHitAilments(2002, hit);
    POE_CHECK(!rBoss.appliedFreeze);
    POE_CHECK(!ailments.IsFrozen(2002));
    POE_CHECK_NEAR(ailments.ChillSlowRatio(2002), 0.25, 1e-6);
    POE_CHECK(!ailments.ApplyHitAilments(2002, hit).appliedFreeze);
    POE_CHECK(ailments.ApplyHitAilments(2002, hit).appliedFreeze);

    // PoB-PoE2 CalcSetup.lua:710: moving or aggravated bleed deals double damage.
    const auto tickStand = ailments.TickActor(1001, 0.5, false);
    const auto tickMove = ailments.TickActor(2002, 0.5, true);
    POE_CHECK_NEAR(tickMove.physPoeDamage, tickStand.physPoeDamage * 2.0, 1e-4);
    POE_CHECK(tickStand.firePoeDamage > 0.0 && tickStand.chaosPoeDamage > 0.0);

    // 3. MonsterModSystem: promotion, enemy ES absorption (Chaos bypasses), KillTracker impact flag
    MonsterProfile rareMob{};
    rareMob.rarity = MonsterRarity::Rare;
    rareMob.modMask = static_cast<std::uint16_t>(
        (1u << static_cast<std::uint8_t>(MonsterModKind::EnergyShielded)) |
        (1u << static_cast<std::uint8_t>(MonsterModKind::FireResistant)) |
        (1u << static_cast<std::uint8_t>(MonsterModKind::Deadly)));
    rareMob.maxEsSkyrim = 100.0;
    rareMob.currentEsSkyrim = 100.0;

    // Full ES absorb still marks hadAnyImpact = true!
    const auto hit1 = MonsterModSystem::ApplyPlayerHitToMonster(rareMob, 50.0, DamageType::Fire);
    POE_CHECK(hit1.hadAnyImpact);
    POE_CHECK(hit1.absorbedByEs > 0.0);
    POE_CHECK_NEAR(hit1.remainingHealthDamage, 0.0, 1e-6);

    // Chaos bypasses enemy ES!
    const auto hitChaos = MonsterModSystem::ApplyPlayerHitToMonster(rareMob, 40.0, DamageType::Chaos);
    POE_CHECK(hitChaos.hadAnyImpact);
    POE_CHECK_NEAR(hitChaos.absorbedByEs, 0.0, 1e-6);
    POE_CHECK(hitChaos.remainingHealthDamage > 0.0);

    // ES recharges after 4s delay
    const double esBefore = rareMob.currentEsSkyrim;
    MonsterModSystem::TickMonsterEs(rareMob, 4.0);
    POE_CHECK_NEAR(rareMob.currentEsSkyrim, esBefore, 1e-6);
    MonsterModSystem::TickMonsterEs(rareMob, 1.0);
    POE_CHECK(rareMob.currentEsSkyrim > esBefore);
    POE_CHECK(!MonsterModSystem::BuildModSummaryKo(rareMob, itemData).empty());

    // 4. Molten Shell guard activation -> 75% hit absorb (DoT exempt) -> break -> 4s CD + reflect
    StatEngine engine;
    Layer1Input l1;
    l1.exileLevel = 60;
    l1.passives.flatStr = 150.0;
    l1.armour = 2000.0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem.instanceId = 99;
    l1.slots[0].gem.gemId = "MoltenShell";
    l1.slots[0].gem.level = 10;
    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    const auto snap = engine.GetSnapshot();

    SkillSystem skillSys;
    const auto act = skillSys.TryActivateMoltenShell(0, *snap, l1.slots, 100.0, 100.0, 200.0, core, cfg, 200.0, 200.0);
    POE_CHECK(act.ok && act.maxShieldSkyrim > 0.0);
    // DoT bypasses Molten Shell
    const auto dotRes = skillSys.AbsorbWithGuardShield(20.0, 1.0, true);
    POE_CHECK_NEAR(dotRes.absorbedEffective, 0.0, 1e-6);
    POE_CHECK_NEAR(dotRes.remainingRawDamage, 20.0, 1e-6);
    // Hit absorbs 75% up to shield cap, breaks on huge hit and triggers 4s CD + reflected fire
    const auto bigHit = skillSys.AbsorbWithGuardShield(act.maxShieldSkyrim * 4.0, 1.0, false);
    POE_CHECK(bigHit.brokeNow);
    POE_CHECK_NEAR(bigHit.absorbedEffective, act.maxShieldSkyrim, 1e-4);
    POE_CHECK(bigHit.reflectedFireSkyrim > 0.0);
    POE_CHECK(skillSys.GuardSnapshot().cooldownRemainingSec >= 4.0 - 1e-6);
    // Cannot reactivate during 4s cooldown
    const auto retry = skillSys.TryActivateMoltenShell(0, *snap, l1.slots, 100.0, 100.0, 200.0, core, cfg, 200.0, 200.0);
    POE_CHECK(!retry.ok);
}

POE_TEST("Stage5 Review #1 regressions: M1-M7 & m1-m6")
{
    const auto& core = Stage5CorePack();
    const auto& itemData = Stage5ItemData();
    Config cfg;

    // M5: No Delve jewel leak; MeleeDamageJewel & MeleeCritMultiplier rollable & parsed
    POE_CHECK(itemData.FindAffix("DelveJewelChaosDamage1") == nullptr);
    const auto* meleeJewel = itemData.FindAffix("MeleeDamageJewel_");
    const auto* meleeCritJewel = itemData.FindAffix("MeleeCritMultiplier");
    POE_CHECK(meleeJewel != nullptr && meleeJewel->rollable);
    POE_CHECK(meleeCritJewel != nullptr && meleeCritJewel->rollable);
    PoEItemRecord jRec;
    jRec.slotGroup = TokenSlotGroup::Jewel;
    jRec.rarity = ItemRarity::Rare;
    jRec.affixes = { { "MeleeDamageJewel_", { 12.0 } }, { "MeleeCritMultiplier", { 16.0 } } };
    const auto jStats = ItemAffixParser::ParseItem(jRec, itemData);
    POE_CHECK_NEAR(jStats.incMeleeDamagePct, 12.0, 1e-6);
    POE_CHECK_NEAR(jStats.meleeCritMultiplierPct, 16.0, 1e-6);

    // M1: Hatred gained-as-cold inherits physical %inc & SupportMeleePhysicalDamage %more
    StatEngine engine;
    Layer1Input l1;
    l1.exileLevel = 70;
    l1.className = "Scion";
    l1.passives.flatStr = 180.0;
    l1.passives.flatDex = 180.0;
    l1.passives.flatInt = 180.0;
    l1.passives.incPhysicalDamagePct = 100.0;
    l1.slots[0].occupied = true;
    l1.slots[0].gem = GemInstance{ 1, "Cyclone", 10, 0, 0, 3, { GemInstance{ 2, "SupportMeleePhysicalDamage", 10, 0, 0, 0, {} } } };
    l1.slots[1].occupied = true;
    l1.slots[1].gem = GemInstance{ 3, "Hatred", 10, 0, 0, 0, {} };
    l1.slots[2].occupied = true;
    l1.slots[2].gem = GemInstance{ 4, "HeraldOfAsh", 20, 0, 0, 0, {} };
    l1.activeAuraMask = (1u << 1) | (1u << 2);
    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    auto snap = engine.GetSnapshot();
    const double physAvg = snap->slotStats[0].baseHitDamage.byType[static_cast<std::size_t>(DamageType::Physical)].Average();
    const double coldAvg = snap->slotStats[0].baseHitDamage.byType[static_cast<std::size_t>(DamageType::Cold)].Average();
    const double hatredPct = core.FindGem("Hatred")->GetLevel(10)->GetStat("physical_damage_%_to_add_as_cold", 0.0);
    POE_CHECK_NEAR(coldAvg, physAvg * (hatredPct / 100.0), physAvg * 0.01);
    // m2: HeraldOfAsh Lv20 burning MORE scales overkill burn pct above Lv1 base 25.0
    POE_CHECK(snap->heraldOfAshOverkillPct > 30.0);

    // Starforge dealNoElemental zeroes Hatred/HeraldOfAsh elemental damage
    l1.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)].occupied = true;
    l1.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)].isCrafted = true;
    l1.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)].isWeapon = true;
    l1.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)].baseWeaponPhysMin = 40.0;
    l1.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)].baseWeaponPhysMax = 60.0;
    l1.equippedGear[static_cast<std::size_t>(EquipSlot::MainHand)].stats.dealNoElemental = true;
    engine.SetLayer1(l1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    snap = engine.GetSnapshot();
    POE_CHECK_NEAR(snap->slotStats[0].baseHitDamage.byType[static_cast<std::size_t>(DamageType::Cold)].Average(), 0.0, 1e-6);
    POE_CHECK_NEAR(snap->slotStats[0].baseHitDamage.byType[static_cast<std::size_t>(DamageType::Fire)].Average(), 0.0, 1e-6);

    // M2: Iron Reflexes + Grace + Determination applies BOTH moreEvasionMult and moreArmourMult
    Layer1Input irL1;
    irL1.exileLevel = 70;
    irL1.passives.flatStr = 180.0;
    irL1.passives.flatDex = 180.0;
    irL1.evasion = 1000.0;
    irL1.passives.ironReflexes = true;
    irL1.slots[0].occupied = true;
    irL1.slots[0].gem = GemInstance{ 10, "Determination", 10, 0, 0, 0, {} };
    irL1.slots[1].occupied = true;
    irL1.slots[1].gem = GemInstance{ 11, "Grace", 10, 0, 0, 0, {} };
    irL1.slots[2].occupied = true;
    irL1.slots[2].gem = GemInstance{ 12, "MoltenShell", 10, 0, 0, 0, {} };
    irL1.activeAuraMask = (1u << 0) | (1u << 1);
    engine.SetLayer1(irL1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    const auto irSnap = engine.GetSnapshot();
    const auto* detLv = core.FindGem("Determination")->GetLevel(10);
    const auto* grcLv = core.FindGem("Grace")->GetLevel(10);
    const double detMore = 1.0 + detLv->GetStat("determination_aura_armour_+%_final", 0.0) / 100.0;
    const double grcMore = 1.0 + grcLv->GetStat("grace_aura_evasion_rating_+%_final", 0.0) / 100.0;
    // PoE2 기본 회피 7 포함
    const double expectedIrArmour = (detLv->GetStat("base_physical_damage_reduction_rating", 0.0) +
        (7.0 + 1000.0 + grcLv->GetStat("base_evasion_rating", 0.0)) * grcMore) * detMore;
    POE_CHECK_NEAR(irSnap->armour, expectedIrArmour, 1e-2);
    POE_CHECK_NEAR(irSnap->evasion, 0.0, 1e-6);

    // M3: MoltenShell reflect damage is independent of baseSkyrimMaxHealth!
    SkillSystem sA, sB;
    const auto actA = sA.TryActivateMoltenShell(2, *irSnap, irL1.slots, 100.0, 100.0, 100.0, core, cfg, 100.0, 100.0);
    const auto actB = sB.TryActivateMoltenShell(2, *irSnap, irL1.slots, 100.0, 100.0, 400.0, core, cfg, 400.0, 400.0);
    POE_CHECK(actA.ok && actB.ok);
    const auto brkA = sA.AbsorbWithGuardShield(actA.maxShieldSkyrim * 4.0, 1.0, false);
    const auto brkB = sB.AbsorbWithGuardShield(actB.maxShieldSkyrim * 4.0, 1.0, false);
    POE_CHECK(brkA.brokeNow && brkB.brokeNow);
    POE_CHECK_NEAR(brkA.reflectedFireSkyrim, brkB.reflectedFireSkyrim, 1e-3);

    // CoC + SpellEcho exclusion & TryBuildHandCastSpell
    Layer1Input cocL1;
    cocL1.exileLevel = 70;
    cocL1.passives.flatStr = 180.0;
    cocL1.passives.flatDex = 180.0;
    cocL1.passives.flatInt = 180.0;
    cocL1.slots[0].occupied = true;
    cocL1.slots[0].gem = GemInstance{ 20, "Cyclone", 10, 0, 0, 3, {} };
    cocL1.slots[1].occupied = true;
    cocL1.slots[1].linkedAttackSlot = 0;
    cocL1.slots[1].gem = GemInstance{ 21, "SupportCastOnCriticalStrike", 10, 0, 0, 3, {
        GemInstance{ 22, "Fireball", 10, 0, 0, 3, { GemInstance{ 23, "SupportSpellEcho", 10, 0, 0, 0, {} } } }
    } };
    cocL1.slots[2].occupied = true;
    cocL1.slots[2].gem = GemInstance{ 24, "Fireball", 10, 0, 0, 3, { GemInstance{ 25, "SupportSpellEcho", 10, 0, 0, 0, {} } } };
    engine.SetLayer1(cocL1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    const auto cocSnap = engine.GetSnapshot();
    POE_CHECK(cocSnap->triggeredSpellStats.at(22).spellRepeatCount == 0);  // SpellEcho blocked on CoC Triggered!
    POE_CHECK(cocSnap->slotStats[2].spellRepeatCount == 1);                // Hand-cast Fireball gets SpellEcho!
    SkillSystem handSys;
    const auto hc = handSys.TryBuildHandCastSpell(2, *cocSnap, cocL1.slots, 200.0, 200.0, core, cfg, 200.0, 200.0);
    POE_CHECK(hc.has_value() && hc->isHandCast && hc->spellRepeatCount == 1);

    // Headhunter Layer2 stolen mods boost stats
    Layer2Input hhL2;
    hhL2.headhunterModMask = static_cast<std::uint16_t>(
        (1u << static_cast<std::uint8_t>(MonsterModKind::Hasted)) |
        (1u << static_cast<std::uint8_t>(MonsterModKind::FireResistant)));
    engine.SetLayer2(hhL2);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    const auto hhSnap = engine.GetSnapshot();
    POE_CHECK(hhSnap->incMovementSpeedPct >= 20.0);
    POE_CHECK(hhSnap->fireResist >= cocSnap->fireResist + 29.0);

    // M6 & m5: MonsterModSystem exact multipliers & leftover recharge delta
    MonsterProfile mProf;
    mProf.rarity = MonsterRarity::Normal;
    mProf.modMask = static_cast<std::uint16_t>(
        (1u << static_cast<std::uint8_t>(MonsterModKind::Armoured)) |
        (1u << static_cast<std::uint8_t>(MonsterModKind::ChaosResistant)) |
        (1u << static_cast<std::uint8_t>(MonsterModKind::Deadly)));
    POE_CHECK_NEAR(MonsterModSystem::IncomingDamageMultiplier(mProf, DamageType::Physical), 0.75, 1e-6);
    POE_CHECK_NEAR(MonsterModSystem::IncomingDamageMultiplier(mProf, DamageType::Chaos), 0.60, 1e-6);
    POE_CHECK_NEAR(MonsterModSystem::ComputeOutgoingModifier(mProf).damageMultiplier, 1.20, 1e-6);
    mProf.maxEsSkyrim = 100.0;
    mProf.currentEsSkyrim = 0.0;
    mProf.esRechargeCooldown = 0.25;
    MonsterModSystem::TickMonsterEs(mProf, 1.25);  // 0.25s clears CD, 1.0s recharges 15 ES
    POE_CHECK_NEAR(mProf.currentEsSkyrim, 15.0, 1e-6);

    // M7, m3, m4: AilmentSystem Inf/NaN guard, 100% chance succeeds at a roll of 1.0, boss micro cold crit, weaker bleed queues
    AilmentSystem ail;
    HitAilmentInput infHit;
    infHit.fireHitPoe = std::numeric_limits<double>::infinity();
    infHit.isCrit = true;
    const auto infRes = ail.ApplyHitAilments(5001, infHit);
    POE_CHECK(!infRes.appliedIgnite && ail.GetState(5001) == nullptr);

    HitAilmentInput microBoss;
    microBoss.coldHitPoe = 1.0;
    microBoss.targetAilmentThresholdPoe = 1000.0;
    microBoss.isCrit = true;
    microBoss.isBossOrDragon = true;
    microBoss.bleedChancePct = 100.0;
    microBoss.physHitPoe = 50.0;
    microBoss.bleedRoll01 = 1.0;  // 100% chance must still hit at roll01==1.0!
    const auto mbRes = ail.ApplyHitAilments(5002, microBoss);
    POE_CHECK(mbRes.appliedBleed);
    POE_CHECK(!mbRes.appliedFreeze && ail.ChillSlowRatio(5002) == 0.0);

    // A weaker bleed is inflicted and waits for the stronger one to expire.
    microBoss.physHitPoe = 10.0;
    POE_CHECK(ail.ApplyHitAilments(5002, microBoss).appliedBleed);

    // m1: Ventor's Gamble negative roll renders "-20%" instead of "+-20%"
    const std::string negRendered = ItemFormatter::RenderLineTemplate(
        "화염 저항 +{0}%", { -20.0 }, 0, { AffixRange{ -25.0, 50.0, false } });
    POE_CHECK(negRendered == "화염 저항 -20%");

    // Review fixes C-9, C-18, N-1 regressions:
    // 1) 결정 209 ⑧: PoE2 지능은 최대 마나를 올린다.
    Layer1Input intL1;
    intL1.exileLevel = 10;
    intL1.className = "Witch";
    intL1.flatEnergyShield = 100.0;
    intL1.passives.flatInt = 68.0;  // Witch 32 + 68 = 100 Int
    engine.SetLayer1(intL1);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    POE_CHECK_NEAR(engine.GetSnapshot()->poeMaxEnergyShield, 100.0, 1e-6);

    // 2) C-18: Normal/Magic monsters NEVER drop Unique tokens (Decision 164)
    Config uniqCfg = cfg;
    uniqCfg.uniqueDropChance = 1.0;
    std::mt19937_64 dropRng(42);
    for (int i = 0; i < 64; ++i) {
        const auto dNorm = LootTable::RollKillDrops(50, false, false, uniqCfg, itemData, dropRng, MonsterRarity::Normal);
        const auto dMag = LootTable::RollKillDrops(50, false, false, uniqCfg, itemData, dropRng, MonsterRarity::Magic);
        for (const auto& e : dNorm) POE_CHECK(e.kind != DropKind::UniqueToken);
        for (const auto& e : dMag) POE_CHECK(e.kind != DropKind::UniqueToken);
    }

    // 3) N-1: PassiveStatParser parses ailment chance lines into StatSnapshot
    PassiveStatTotals ailTot{};
    POE_CHECK(PassiveStatParser::ParseLine("15% chance to Ignite", ailTot));
    POE_CHECK(PassiveStatParser::ParseLine("10% chance to Freeze, Shock and Ignite", ailTot));
    POE_CHECK(PassiveStatParser::ParseLine("25% chance to cause Bleeding", ailTot));
    POE_CHECK(PassiveStatParser::ParseLine("20% chance to Poison on Hit", ailTot));
    POE_CHECK_NEAR(ailTot.igniteChancePct, 25.0, 1e-9);
    POE_CHECK_NEAR(ailTot.freezeChancePct, 10.0, 1e-9);
    POE_CHECK_NEAR(ailTot.shockChancePct, 10.0, 1e-9);
    POE_CHECK_NEAR(ailTot.bleedChancePct, 25.0, 1e-9);
    POE_CHECK_NEAR(ailTot.poisonChancePct, 20.0, 1e-9);

    // 4) U5 확정: Starkonja Low Life 50% reduced Damage + 150% inc Global Evasion & Astramentis -4 Phys Attack Taken
    const auto* starkDef = itemData.FindUnique("StarkonjasHead");
    const auto* astraDef = itemData.FindUnique("Astramentis");
    POE_CHECK(starkDef != nullptr && astraDef != nullptr);
    PoEItemRecord starkRec{};
    starkRec.rarity = ItemRarity::Unique;
    starkRec.uniqueId = starkDef->id;
    for (const auto& aid : starkDef->explicitAffixIds) {
        if (const auto* ad = itemData.FindAffix(aid)) {
            std::vector<double> vals;
            for (const auto& ln : ad->lines) for (const auto& rg : ln.ranges) vals.push_back(rg.minVal);
            starkRec.affixes.push_back(RolledAffix{ aid, std::move(vals) });
        }
    }
    const auto starkStats = ItemAffixParser::ParseItem(starkRec, itemData);
    POE_CHECK_NEAR(starkStats.reducedDamageOnLowLifePct, 50.0, 1e-6);
    POE_CHECK_NEAR(starkStats.incEvasionOnLowLifePct, 150.0, 1e-6);

    PoEItemRecord astraRec{};
    astraRec.rarity = ItemRarity::Unique;
    astraRec.uniqueId = astraDef->id;
    for (const auto& aid : astraDef->explicitAffixIds) {
        if (const auto* ad = itemData.FindAffix(aid)) {
            std::vector<double> vals;
            for (const auto& ln : ad->lines) for (const auto& rg : ln.ranges) vals.push_back(rg.minVal);
            astraRec.affixes.push_back(RolledAffix{ aid, std::move(vals) });
        }
    }
    const auto astraStats = ItemAffixParser::ParseItem(astraRec, itemData);
    POE_CHECK_NEAR(astraStats.flatPhysTakenFromAttacksDelta, -4.0, 1e-6);

    // Verify Starkonja Low Life penalty actually reduces damage and boosts evasion in StatEngine Layer2
    Layer1Input u5L1;
    u5L1.exileLevel = 60;
    u5L1.className = "Scion";
    u5L1.slots[0].occupied = true;
    u5L1.slots[0].gem = GemInstance{ 1, "Fireball", 1, 0, 0, 0, {} };
    u5L1.equippedGear[static_cast<std::size_t>(EquipSlot::Helmet)].occupied = true;
    u5L1.equippedGear[static_cast<std::size_t>(EquipSlot::Helmet)].stats = starkStats;
    u5L1.equippedGear[static_cast<std::size_t>(EquipSlot::Amulet)].occupied = true;
    u5L1.equippedGear[static_cast<std::size_t>(EquipSlot::Amulet)].stats = astraStats;
    engine.SetLayer1(u5L1);
    Layer2Input u5L2;
    u5L2.lowLife = false;
    engine.SetLayer2(u5L2);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    const auto snapFull = engine.GetSnapshot();
    u5L2.lowLife = true;
    engine.SetLayer2(u5L2);
    POE_CHECK(engine.RebuildIfDirty(core, cfg));
    const auto snapLow = engine.GetSnapshot();
    const double dmgFull = snapFull->slotStats[0].baseHitDamage.byType[static_cast<std::size_t>(DamageType::Fire)].Average();
    const double dmgLow = snapLow->slotStats[0].baseHitDamage.byType[static_cast<std::size_t>(DamageType::Fire)].Average();
    POE_CHECK_NEAR(dmgLow, dmgFull * 0.5, dmgFull * 0.01);
    POE_CHECK(snapLow->evasion > snapFull->evasion * 1.5);
    POE_CHECK_NEAR(snapFull->flatPhysTakenFromAttacksDelta, -4.0, 1e-6);
}

