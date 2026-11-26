#include "test_harness.h"

#include "core/ContentCheck.h"
#include "core/EspFormCheck.h"
#include "core/GemForms.h"
#include "core/GemTransaction.h"
#include "core/KillTracker.h"
#include "core/LootTable.h"
#include "core/ProjectileStats.h"
#include "core/SaveRecordFormat.h"
#include "core/SkillSystem.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

using namespace SkyrimPoE::Core;
using namespace SkyrimPoE::Core::GemForms;
using namespace SkyrimPoE::Core::SaveRecord;

namespace
{
    constexpr std::array<LegacyGemKey, 28> kExpectedLegacy{{
        { "Cyclone", 0x814 }, { "SupportCastOnCriticalStrike", 0x815 }, { "IceNova", 0x816 },
        { "ShockNova", 0x817 }, { "SupportIncreasedCriticalStrikes", 0x818 }, { "SupportMeleePhysicalDamage", 0x819 },
        { "SupportBrutality", 0x81A }, { "SupportIncreasedAreaOfEffect", 0x81B }, { "SupportConcentratedEffect", 0x81C },
        { "Fireball", 0x87D }, { "FreezingPulse", 0x87E }, { "Arc", 0x87F }, { "Spark", 0x880 },
        { "SupportGreaterMultipleProjectiles", 0x881 }, { "SupportChain", 0x882 }, { "SupportFork", 0x883 },
        { "SupportPierce", 0x884 }, { "SupportSpellEcho", 0x885 }, { "Determination", 0x886 },
        { "Grace", 0x887 }, { "Discipline", 0x888 }, { "Hatred", 0x889 }, { "Wrath", 0x88A },
        { "Anger", 0x88B }, { "PurityOfElements", 0x88C }, { "HeraldOfIce", 0x88D },
        { "HeraldOfAsh", 0x88E }, { "MoltenShell", 0x88F },
    }};

    const SaveGeneration kGen0{ "poe1", 0, "3.30", "legacy", "0.1.0" };
    const SaveGeneration kGen1{ "poe2", 1, "0.5", "content", "0.1.0" };

    DataPack LoadPack(std::uint32_t generation)
    {
        DataPack pack;
        std::string error;
        const std::filesystem::path data(POE_TEST_DATA_DIR);
        const auto rel = generation == 0 ? "packs/gen0/core.json" : "packs/gen1/core.json";
        if (!pack.LoadFromFile(data / rel, &error)) {
            throw std::runtime_error(error);
        }
        return pack;
    }

    class Inventory : public IGemInventoryAdapter
    {
    public:
        std::optional<GemInstance> TakeGem(const InventoryItemKey&, std::string_view id,
            std::uint8_t sockets, std::uint64_t& nextId) override
        {
            ++takes;
            return GemInstance{ .instanceId = nextId++, .gemId = std::string(id), .socketCount = sockets };
        }

        std::optional<InventoryItemKey> ReturnGem(GemInstance) override
        {
            ++returns;
            return InventoryItemKey{ 0x14, 1 };
        }

        unsigned takes = 0;
        unsigned returns = 0;
    };

    SaveRecordBatch MakeSave(const SaveGeneration& generation)
    {
        const bool legacy = generation.contentGeneration == 0;
        GemInstance gem{ .instanceId = 42, .gemId = legacy ? "SupportCastOnCriticalStrike" : "gem.CastOnCriticalStrike",
            .level = 6, .quality = 7, .xp = 456, .socketCount = 2 };
        gem.socketedGems.push_back({ .instanceId = 43, .gemId = legacy ? "Spark" : "gem.Spark",
            .level = 5, .quality = 4, .xp = 234, .socketCount = 1 });
        gem.socketedGems.front().socketedGems.push_back({
            .instanceId = 44, .gemId = legacy ? "SupportChain" : "sup.Chain1", .level = 3, .quality = 2, .xp = 12
        });
        Snapshot snapshot;
        snapshot.state.tree.generation = generation.contentGeneration;
        snapshot.state.slots[0] = { .occupied = true, .gem = gem };
        snapshot.tracked.push_back({ { 0xFF001002, 1234 }, true, gem });
        SaveRecordBatch batch;
        batch.complete = WriteSnapshot(snapshot, generation, [&](RawSaveRecord&& record) {
            batch.records.push_back(std::move(record));
            return true;
        });
        POE_CHECK(batch.complete);
        return batch;
    }

    RawSaveRecord& Record(SaveRecordBatch& batch, std::uint32_t type)
    {
        const auto it = std::ranges::find(batch.records, type, &RawSaveRecord::type);
        POE_CHECK(it != batch.records.end());
        return *it;
    }

    void ReplaceKey(RawSaveRecord& record, std::string_view oldKey, std::string_view newKey)
    {
        auto& bytes = record.bytes;
        const auto text = std::search(bytes.begin(), bytes.end(), oldKey.begin(), oldKey.end());
        POE_CHECK(text != bytes.end() && std::distance(bytes.begin(), text) >= sizeof(std::uint32_t));
        const auto length = static_cast<std::uint32_t>(newKey.size());
        std::memcpy(&*(text - sizeof(length)), &length, sizeof(length));
        const auto insertAt = bytes.erase(text, text + oldKey.size());
        bytes.insert(insertAt, newKey.begin(), newKey.end());
    }
}

POE_TEST("Gem form tables round trip each legacy key, content ID and physical form")
{
    POE_CHECK(kLegacyKeys.size() == 28 && kContentKeys.size() == 43 && kPhysicalForms.size() == 62);
    for (std::size_t i = 0; i < kExpectedLegacy.size(); ++i) {
        const auto& expected = kExpectedLegacy[i];
        const auto* physical = FindPhysical(expected.localFormId);
        POE_CHECK(physical && physical->legacyId == expected.gemId);
        POE_CHECK(FindLegacy(expected.gemId) == &kLegacyKeys[i]);
        POE_CHECK(kLegacyKeys[i].localFormId == expected.localFormId);
    }
    for (const auto& entry : kContentKeys) {
        POE_CHECK(FindContent(entry.contentId) == &entry);
        const auto* physical = FindPhysical(entry.localFormId);
        POE_CHECK(physical && physical->contentId == entry.contentId);
    }
    for (const auto& physical : kPhysicalForms) {
        POE_CHECK(FindPhysical(physical.localFormId) == &physical);
        POE_CHECK(!physical.legacyId.empty() || !physical.contentId.empty());
        if (!physical.legacyId.empty()) POE_CHECK(LegacyReturnForm(physical.legacyId) == physical.localFormId);
        if (!physical.contentId.empty()) POE_CHECK(FindContent(physical.contentId)->localFormId == physical.localFormId);
    }
    // G1(결정 204·231): houseCARL이 차례로 준 세대 GLOB 번호를 장부에서 생성한다.
    POE_CHECK(kGenerationGlobal == 0x8E2u);
}

POE_TEST("Gem consumers keep the generation-zero IDs and apply the generation-one sets")
{
    POE_CHECK(StarterForms(0).size() == 28);
    POE_CHECK(StarterForms(0, true).size() == 19);
    POE_CHECK(StarterForms(1).empty() && StarterForms(1, true).empty());
    std::set<std::string> legacyDrops;
    for (std::size_t i = 0; i < kExpectedLegacy.size(); ++i) {
        const auto& expected = kExpectedLegacy[i];
        POE_CHECK(StarterForms(0)[i].gemId == expected.gemId);
        POE_CHECK(ActiveForm(0, expected.gemId) == expected.localFormId);
        POE_CHECK(LegacyReturnForm(expected.gemId) == expected.localFormId);
        POE_CHECK(RecoveryForm(0, expected.gemId) == expected.localFormId);
        if (i >= 9) {
            POE_CHECK(StarterForms(0, true)[i - 9].gemId == expected.gemId);
            POE_CHECK(DropIds(0)[i - 9] == expected.gemId);
            legacyDrops.emplace(expected.gemId);
        }
    }
    for (const auto& physical : kPhysicalForms) {
        const auto known = std::ranges::find(kExpectedLegacy, physical.localFormId, &LegacyGemKey::localFormId);
        POE_CHECK(IsRecoveryForm(0, physical) == (known != kExpectedLegacy.end()));
        POE_CHECK(IsRecoveryForm(1, physical));
        POE_CHECK(RecoveryForm(1, InventoryId(1, physical)) == physical.localFormId);
    }
    for (const auto& entry : kContentKeys) {
        POE_CHECK(ActiveForm(0, entry.contentId) == 0 && RecoveryForm(0, entry.contentId) == 0);
        POE_CHECK(ActiveForm(1, entry.contentId) == (entry.contentId == "gem.Spark" ? entry.localFormId : 0));
    }

    const ItemData items;
    Config config;
    config.enableLootDrops = true;
    for (const auto generation : { 0u, 1u }) {
        std::mt19937_64 rng(79281);
        std::set<std::string> observed;
        for (int i = 0; i < 15000; ++i) {
            for (const auto& drop : LootTable::RollKillDrops(35, true, false, config, items, rng,
                     MonsterRarity::Rare, 0, 0, 0, false, generation)) {
                if (drop.kind == DropKind::SkillGem) observed.insert(drop.gemId);
            }
        }
        POE_CHECK(observed == (generation == 0 ? legacyDrops : std::set<std::string>{ "gem.Spark" }));
    }
}

POE_TEST("Gem catalog loads uninfused Spark from gen1 core and keeps the legacy catalog independent")
{
    const auto pack0 = LoadPack(0);
    const auto pack1 = LoadPack(1);
    for (const auto& key : kContentKeys) POE_CHECK(pack1.FindGem(std::string(key.contentId), 1));
    POE_CHECK(pack0.AllGems().size() == 28 && pack0.FindGem("Spark", 0));
    POE_CHECK(pack1.AllGems().size() == 43);
    POE_CHECK(!pack1.FindGem("Spark", 1) && !pack0.FindGem("gem.Spark", 0) && !pack1.FindGem("gem.Spark", 0));
    const auto* spark = pack1.FindGem("gem.Spark", 1);
    POE_CHECK(spark && spark->kind == GemKind::Spell);
    const auto* level = spark->GetLevel(1);
    POE_CHECK(level && level->manaCost == 5);
    POE_CHECK(level->GetStat("spell_minimum_base_lightning_damage") == 1);
    POE_CHECK(level->GetStat("spell_maximum_base_lightning_damage") == 10);
    POE_CHECK(level->GetStat("base_number_of_projectiles") == 4);
}

POE_TEST("Spark hand cast takes cost, lightning range and projectile count from the world catalog")
{
    const auto pack0 = LoadPack(0);
    const auto pack1 = LoadPack(1);
    Config config;
    config.enforceGemRequirements = true;
    StatEngine stats;
    SkillSystem skills;
    constexpr double baseMagicka = 100.0;
    for (std::uint32_t generation : { 0u, 1u, 0u }) {
        const auto& pack = generation == 0 ? pack0 : pack1;
        Layer1Input input;
        input.gemGeneration = generation;
        input.passives.resoluteTechnique = true;
        auto& slot = input.slots[0];
        slot = { .occupied = true, .gem = { .instanceId = 17, .gemId = generation == 0 ? "Spark" : "gem.Spark" } };
        stats.SetLayer1(input);
        POE_CHECK(stats.RebuildIfDirty(pack, config));
        const auto snapshot = stats.GetSnapshot();
        POE_CHECK(!snapshot->poe2Passives && snapshot->slotStats[0].requirementMet);
        const auto cast = skills.TryBuildHandCastSpell(0, *snapshot, input.slots, baseMagicka, baseMagicka, pack, config);
        POE_CHECK(cast && cast->isHandCast && cast->gemId == slot.gem.gemId);

        const auto expectedCost = ComputeMagickaScaling(baseMagicka, snapshot->poeMaxMana,
            generation == 0 ? 6.0 : 5.0, snapshot->expectedPoeMana);
        POE_CHECK_NEAR(cast->skyrimMagickaCost, expectedCost.skyrimSkillCost, 1e-9);
        POE_CHECK(cast->skyrimHealthCost == 0.0);
        const auto lightning = cast->poeBaseHitDamage.byType[static_cast<std::size_t>(DamageType::Lightning)];
        const double expectedMax = generation == 0 ? 28.0 : 10.0;
        POE_CHECK(lightning.min == 1.0 && lightning.max == expectedMax);
        bool crit = true;
        POE_CHECK_NEAR(SkillSystem::RollTriggeredSpellSkyrimDamage(*cast, pack, config, 0.0, 0.0, crit),
            ConvertOffenseHit(1.0, snapshot->exileLevel, pack, config), 1e-9);
        POE_CHECK(!crit);
        POE_CHECK_NEAR(SkillSystem::RollTriggeredSpellSkyrimDamage(*cast, pack, config, 1.0, 0.0, crit),
            ConvertOffenseHit(expectedMax, snapshot->exileLevel, pack, config), 1e-9);
        POE_CHECK(!crit);
        const auto launched = RollCastProjectileCount(cast->projectileCount, cast->additionalProjectileChancePct,
            cast->additionalSpellProjectilesChancePct, [] { return 0.0; });
        POE_CHECK(launched == 4);
    }
}

POE_TEST("Gem use distinguishes legacy, not-ready and available while recognizing each physical form")
{
    POE_CHECK(InventoryId(0, *FindPhysical(0x880)) == "Spark");
    POE_CHECK(InventoryId(1, *FindPhysical(0x880)) == "gem.Spark");
    POE_CHECK(InventoryId(1, *FindPhysical(0x817)) == "ShockNova");
    for (const auto& entry : kLegacyKeys) {
        const auto use = ClassifyUse(0, entry.gemId);
        POE_CHECK(use.recognized && use.reason == GemUseReason::Available && UseReasonText(use.reason).empty());
    }
    const auto legacy = ClassifyUse(1, "ShockNova");
    const auto pending = ClassifyUse(1, "gem.FrostBolt");
    const auto spark = ClassifyUse(1, "gem.Spark");
    POE_CHECK(legacy.recognized && legacy.reason == GemUseReason::Legacy && !legacy.equip && !legacy.socket && !legacy.execute);
    POE_CHECK(pending.recognized && pending.reason == GemUseReason::NotReady && !pending.equip && !pending.socket && !pending.execute);
    POE_CHECK(spark.recognized && spark.reason == GemUseReason::Available && spark.equip && spark.socket && spark.execute);
    POE_CHECK(!ClassifyUse(1, "gem.Unknown").recognized);
}

POE_TEST("Generation-one equip and socket reject before touching the inventory or slots")
{
    const auto pack0 = LoadPack(0);
    const auto pack1 = LoadPack(1);
    std::array<SkillSlot, kSkillSlotCount> slots{};
    std::uint64_t nextId = 1;
    Inventory inventory;
    POE_CHECK(GemTransaction::EquipSkillGem(slots, nextId, 0, {}, "gem.Spark", 0, pack1, inventory, 1).ok);
    for (const auto id : { "ShockNova", "gem.FrostBolt", "SupportChain", "sup.Chain1" }) {
        const auto expected = UseReasonText(ClassifyUse(1, id).reason);
        const auto equip = GemTransaction::EquipSkillGem(slots, nextId, 0, {}, id, 0, pack1, inventory, 1);
        const auto socket = GemTransaction::SocketSubGem(slots, nextId, 0, -1, {}, id, 0, pack1, inventory, 1);
        POE_CHECK(!equip.ok && equip.message == expected && !socket.ok && socket.message == expected);
        POE_CHECK(inventory.takes == 1 && inventory.returns == 0 && nextId == 2);
        POE_CHECK(slots[0].gem.gemId == "gem.Spark" && slots[0].gem.instanceId == 1 && slots[0].gem.socketedGems.empty());
    }

    // 발동 젬 연결도 세계 세대의 표로 판정한다. 구판 발동 젬과 구판 공격 칸은 세대 1에서 연결되지 않는다.
    slots[1] = { .occupied = true, .gem = { .instanceId = 2, .gemId = "SupportCastOnCriticalStrike" } };
    slots[2] = { .occupied = true, .gem = { .instanceId = 3, .gemId = "Cyclone" } };
    const auto legacyText = UseReasonText(GemUseReason::Legacy);
    const auto legacyTrigger = GemTransaction::LinkTriggerAttack(slots, 1, 2, pack1, 1);
    POE_CHECK(!legacyTrigger.ok && legacyTrigger.message == legacyText && slots[1].linkedAttackSlot == -1);
    POE_CHECK(GemTransaction::LinkTriggerAttack(slots, 1, 2, pack0, 0).ok && slots[1].linkedAttackSlot == 2);
}

POE_TEST("Generation-one execution rejects before cost and cooldown, including stale computed stats")
{
    const auto pack = LoadPack(1);
    const Config config;
    SkillSystem skills;
    StatEngine stats;
    Layer1Input input;
    input.gemGeneration = 1;
    auto& slot = input.slots[0];
    slot = { .occupied = true, .gem = { .instanceId = 17, .gemId = "gem.Spark" } };
    stats.SetLayer1(input);
    POE_CHECK(stats.RebuildIfDirty(pack, config));
    const auto snap = stats.GetSnapshot();
    POE_CHECK(snap->slotStats[0].kind == GemKind::Spell);
    POE_CHECK(snap->slotStats[0].projectileCount == 4);
    for (const auto id : { "Spark", "gem.FrostBolt" }) {
        slot.gem.gemId = id;
        auto stale = *snap;
        stale.slotStats[0].cooldownSec = 5;
        auto denied = skills.TryBuildHandCastSpell(0, stale, input.slots, 100, 100, pack, config, 100, 100);
        POE_CHECK(!denied && skills.RemainingCooldown(17) == 0.0);
        stats.SetLayer1(input);
        POE_CHECK(stats.RebuildIfDirty(pack, config));
        POE_CHECK(stats.GetSnapshot()->slotStats[0].kind == GemKind::Unknown);
    }
    slot.gem.gemId = "gem.Spark";
    auto available = skills.TryBuildHandCastSpell(0, *snap, input.slots, 100, 100, pack, config, 100, 100);
    POE_CHECK(available && available->skyrimMagickaCost > 0.0);
}

POE_TEST("GKEY and CORE preserve the recursive gem records in each generation")
{
    for (const auto& generation : { kGen0, kGen1 }) {
        auto batch = MakeSave(generation);
        const bool legacy = generation.contentGeneration == 0;
        const auto& gkey = Record(batch, kRecordGemKeys);
        const auto& core = Record(batch, kRecordCore);
        POE_CHECK(gkey.version == (legacy ? 1u : 2u) && core.version == (legacy ? 2u : 3u));
        POE_CHECK(DecideSave(batch, false, generation).disposition == SaveDisposition::Normal);
        std::vector<SkyrimPoE::Runtime::TrackedGemEntry> entries;
        POE_CHECK(ReadGemKeysRecord(gkey, entries) && entries.size() == 1);
        POE_CHECK(entries[0].isWorld && entries[0].key.ownerFormId == 0xFF001002 && entries[0].key.uniqueId == 1234);
        Snapshot restored;
        POE_CHECK(ReadCoreRecord(core, restored.state));
        restored.tracked = std::move(entries);
        for (const auto* root : { &restored.state.slots[0].gem, &restored.tracked[0].gem }) {
            POE_CHECK(root->gemId == (legacy ? "SupportCastOnCriticalStrike" : "gem.CastOnCriticalStrike"));
            POE_CHECK(root->instanceId == 42 && root->level == 6 && root->quality == 7 && root->xp == 456 && root->socketCount == 2);
            const auto& child = root->socketedGems.at(0);
            POE_CHECK(child.gemId == (legacy ? "Spark" : "gem.Spark"));
            POE_CHECK(child.instanceId == 43 && child.level == 5 && child.quality == 4 && child.xp == 234 && child.socketCount == 1);
            const auto& support = child.socketedGems.at(0);
            POE_CHECK(support.gemId == (legacy ? "SupportChain" : "sup.Chain1"));
            POE_CHECK(support.instanceId == 44 && support.level == 3 && support.quality == 2 && support.xp == 12);
        }
        POE_CHECK(WriteSnapshot(restored, generation, [&](RawSaveRecord&& record) {
            if (record.type == kRecordGemKeys) POE_CHECK(record == gkey);
            if (record.type == kRecordCore) POE_CHECK(record == core);
            return true;
        }));
    }

    // 위치 바이트(개수 u32 + 소유자 u32 + 고유 번호 u16 뒤)는 v2에서 0·1만 허용한다. v1은 예전처럼 0이 아니면 바닥으로 읽는다.
    constexpr std::size_t kWorldByteOffset = 4 + 4 + 2;
    for (const auto& generation : { kGen0, kGen1 }) {
        auto batch = MakeSave(generation);
        auto& gkey = Record(batch, kRecordGemKeys);
        gkey.bytes.at(kWorldByteOffset) = 2;
        std::vector<SkyrimPoE::Runtime::TrackedGemEntry> entries;
        const bool legacy = generation.contentGeneration == 0;
        POE_CHECK(ReadGemKeysRecord(gkey, entries) == legacy);
        POE_CHECK((DecideSave(batch, false, generation).disposition == SaveDisposition::Preserve) == !legacy);
    }
}

POE_TEST("Generation-one GKEY omits legacy entries before applying the save cap")
{
    Snapshot snapshot;
    snapshot.state.tree.generation = 1;
    snapshot.state.slots[0] = { .occupied = true, .gem = { .instanceId = 1, .gemId = "gem.Spark" } };
    for (std::uint32_t i = 0; i < kMaxListLen; ++i) {
        snapshot.tracked.push_back({ { 0xFF001002, static_cast<std::uint16_t>(i + 1) }, true,
            { .instanceId = 2 + i, .gemId = "gem.Spark", .level = 4 } });
    }
    snapshot.tracked.push_back({ { 0x14, 1 }, false, { .gemId = "SupportChain" } });
    snapshot.tracked.push_back({ { 0x14, 2 }, false, { .gemId = "gem.CastOnCriticalStrike",
        .socketedGems = { { .gemId = "gem.Spark", .socketedGems = { { .gemId = "SupportChain" } } } } } });

    SaveRecordBatch batch;
    std::vector<std::string> failures;
    std::vector<std::string> omissions;
    batch.complete = WriteSnapshot(snapshot, kGen1, [&](RawSaveRecord&& record) {
        batch.records.push_back(std::move(record));
        return true;
    }, [&](std::string_view warning) { failures.emplace_back(warning); },
        [&](std::string_view warning) { omissions.emplace_back(warning); });
    POE_CHECK(batch.complete && failures.empty());
    POE_CHECK(omissions.size() == 1 && omissions[0].contains("omitted 2"));
    POE_CHECK(DecideSave(batch, false, kGen1).disposition == SaveDisposition::Normal);

    std::vector<SkyrimPoE::Runtime::TrackedGemEntry> restored;
    POE_CHECK(ReadGemKeysRecord(Record(batch, kRecordGemKeys), restored) && restored.size() == kMaxListLen);
    for (std::size_t i = 0; i < restored.size(); ++i) {
        POE_CHECK(restored[i].key == snapshot.tracked[i].key && restored[i].isWorld);
        POE_CHECK(restored[i].gem.gemId == "gem.Spark" && restored[i].gem.level == 4);
    }
    POE_CHECK(snapshot.tracked.size() == kMaxListLen + 2);
    POE_CHECK(snapshot.tracked[kMaxListLen].gem.gemId == "SupportChain");
}

POE_TEST("Generation-one saves refuse to omit legacy gems owned by CORE slots")
{
    Snapshot snapshot;
    snapshot.state.tree.generation = 1;
    snapshot.state.slots[0] = { .occupied = true, .gem = { .instanceId = 1, .gemId = "Spark" } };

    std::vector<RawSaveRecord> records;
    std::vector<std::string> failures;
    bool omitted = false;
    POE_CHECK(!WriteSnapshot(snapshot, kGen1, [&](RawSaveRecord&& record) {
        records.push_back(std::move(record));
        return true;
    }, [&](std::string_view warning) { failures.emplace_back(warning); },
        [&](std::string_view) { omitted = true; }));
    POE_CHECK(!omitted && failures.size() == 1);
    POE_CHECK(std::ranges::find(records, kRecordCore, &RawSaveRecord::type) == records.end());
    POE_CHECK(std::ranges::find(records, kRecordGeneration, &RawSaveRecord::type) == records.end());
}

POE_TEST("Gem keys with the wrong generation preserve the original CORE and GKEY bytes")
{
    auto legacy = MakeSave(kGen0);
    auto current = MakeSave(kGen1);
    const auto preserve = [](const SaveRecordBatch& batch, const SaveGeneration& installed) {
        POE_CHECK(DecideSave(batch, false, installed).disposition == SaveDisposition::Preserve);
        std::vector<RawSaveRecord> written;
        POE_CHECK(ReplaySaveRecords(batch, [&](const RawSaveRecord& record) {
            written.push_back(record);
            return true;
        }));
        POE_CHECK(written == batch.records);
    };
    for (const auto recordType : { kRecordCore, kRecordGemKeys }) {
        auto oldMixed = legacy;
        ReplaceKey(Record(oldMixed, recordType), "SupportChain", "sup.Chain1");
        preserve(oldMixed, kGen0);
        preserve(oldMixed, kGen1);  // Before legacy cleanup.
        auto newMixed = current;
        ReplaceKey(Record(newMixed, recordType), "sup.Chain1", "SupportChain");
        preserve(newMixed, kGen1);
        ReplaceKey(Record(newMixed, recordType), "SupportChain", "sup.Unknown");
        preserve(newMixed, kGen1);
    }
    for (auto invalid : { legacy, current }) {
        const auto& installed = Record(invalid, kRecordCore).version == 2 ? kGen0 : kGen1;
        Record(invalid, kRecordGemKeys).version = 99;
        preserve(invalid, installed);
    }
    auto wrongVersion = current;
    Record(wrongVersion, kRecordGemKeys).version = 1;
    preserve(wrongVersion, kGen1);
    std::erase_if(legacy.records, [](const auto& record) { return record.type == kRecordGeneration; });
    std::erase_if(current.records, [](const auto& record) { return record.type == kRecordGeneration; });
    POE_CHECK(DecideSave(legacy, false, kGen0).disposition == SaveDisposition::Normal);
    POE_CHECK(DecideSave(legacy, false, kGen1).disposition == SaveDisposition::CleanupFromLegacy);
    preserve(current, kGen0);
    preserve(current, kGen1);
}

POE_TEST("Generation-one gem experience leaves active and socketed instances unchanged")
{
    std::array<SkillSlot, kSkillSlotCount> slots{};
    slots[0] = { .occupied = true, .gem = { .instanceId = 1, .gemId = "gem.Spark", .xp = 99 } };
    slots[0].gem.socketedGems.push_back({ .instanceId = 2, .gemId = "sup.Chain1", .xp = 99 });
    const auto noGrowth = KillTracker::ApplyGemXpToSlots(slots, 1, 1);
    POE_CHECK(noGrowth.gemsGainedXp == 0 && noGrowth.gemLevelsGained == 0);
    POE_CHECK(slots[0].gem.xp == 99 && slots[0].gem.socketedGems[0].xp == 99);
    slots[0].gem.gemId = "Spark";
    slots[0].gem.socketedGems[0].gemId = "SupportChain";
    const auto legacy = KillTracker::ApplyGemXpToSlots(slots, 1, 0);
    POE_CHECK(legacy.gemsGainedXp == 2 && legacy.gemLevelsGained == 2);
}

POE_TEST("ESP verification observes all 62 physical gems including staged and retiring records")
{
    std::vector<EspFormObservation> forms;
    ObservePhysicalForms([&](const GemForm& gem) {
        const bool missing = gem.contentId == "gem.FrostBolt";
        const bool wrongType = gem.legacyId == "ShockNova";
        forms.push_back({ gem.legacyId.empty() ? gem.contentId : gem.legacyId,
            gem.localFormId, !missing, !wrongType, true, false });
    });
    const auto result = CheckEspForms(forms);
    POE_CHECK(forms.size() == 62 && result.found == 61);
    POE_CHECK(result.missing == 1 && result.wrongType == 1 && result.failedForms == 2);
    POE_CHECK(result.reason.find("gem.FrostBolt") != std::string::npos);
    POE_CHECK(result.reason.find("ShockNova") != std::string::npos);
}
