#include "core/Migration.h"
#include "core/SaveRecordFormat.h"
#include "test_harness.h"

#include <array>
#include <bit>
#include <cstring>
#include <limits>

using namespace SkyrimPoE::Core;
using namespace SkyrimPoE::Core::SaveRecord;

namespace
{
    const SaveGeneration kPoE1Pack{ "poe1", 0, "3.30", "test-build", "0.1.0" };
    const SaveGeneration kPoE2Pack{ "poe2", 1, "0.5", "test-build", "0.1.0" };
    constexpr std::uint32_t kChest = 0x01000050;
    constexpr std::uint32_t kJewel = 0x0100089F;
    constexpr std::uint32_t kActor = 0x01000123;

    GemInstance Gem(std::uint64_t id, std::string name, std::vector<GemInstance> children = {})
    {
        return { .instanceId = id, .gemId = std::move(name), .socketedGems = std::move(children) };
    }

    Snapshot LegacySnapshot()
    {
        Snapshot save;
        save.state.exileLevel = 27;
        save.state.appliedHealthDelta = 21.75;
        save.state.appliedMagickaDelta = -6.25;
        save.state.slots[0] = { 0, true, Gem(1, "OldAttack", { Gem(2, "OldSupport") }) };
        save.state.slots[1] = { 1, true, Gem(3, "OldTrigger", { Gem(4, "OldSpell", { Gem(5, "SpellSupport") }) }) };
        save.tracked.push_back({ { kChest, 7 }, false, Gem(10, "BagRoot",
            { Gem(11, "BagSpell", { Gem(12, "BagSupport") }) }) });
        save.tracked.push_back({ { kPlayerRefFormId, 8 }, false, Gem(13, "StandaloneGem") });
        save.state.activeAuraMask = 2;
        save.state.starterStage5GemsGiven = true;
        save.state.charmSlots[1] = { true, CharmKind::Topaz, 30.0 };
        save.state.virtualRing2Key = { kPlayerRefFormId, 90 };
        save.state.virtualRing2BaseFormId = 0x0100089B;
        save.state.virtualBeltKey = { kPlayerRefFormId, 91 };
        save.state.virtualBeltBaseFormId = 0x0100089C;
        auto& jewel = save.state.socketedJewels[42];
        jewel.baseFormId = kJewel;
        jewel.jewelColor = 2;
        jewel.affixes = { { "OldJewelAffix", { 100.0 } } };
        jewel.implicitAffixes = { { "OldImplicit", { 1.0, 2.0 } } };
        save.savedSpeedMods.push_back({ kActor, -5.25f });
        save.savedWorldDrops.push_back({ 0xFF000001, 0xFF000002, 0xFF000003, 4 });
        auto& map = save.savedMapState;
        map.active = true;
        map.dungeonKey = "old-pack-dungeon";
        map.returnCellFormId = 0x10001;
        map.mods = { { "RetiredMapAffix", { 99.0 } } };
        return save;
    }

    SaveRecordBatch EncodeLegacy(const Snapshot& snapshot)
    {
        SaveRecordBatch batch;
        batch.complete = WriteSnapshot(snapshot, kPoE1Pack, [&](RawSaveRecord&& record) {
            batch.records.push_back(std::move(record));
            return true;
        });
        POE_CHECK(batch.complete);
        return batch;
    }

    RawSaveRecord& FindRecord(SaveRecordBatch& batch, std::uint32_t type)
    {
        const auto record = std::ranges::find(batch.records, type, &RawSaveRecord::type);
        POE_CHECK(record != batch.records.end());
        return *record;
    }

    LegacyMigrationResult Build(const SaveRecordBatch& batch, const FormIdResolver& resolver = {})
    {
        return BuildLegacyMigration(batch, kPoE2Pack.game, kPoE2Pack.contentGeneration, resolver);
    }

    MigrationState FullMigration()
    {
        auto built = Build(EncodeLegacy(LegacySnapshot()));
        POE_CHECK(built.report.Succeeded());
        auto& state = built.state;
        state.step = CleanupStep::GrantStarter;
        state.sourceGeneration = 7;
        state.targetGeneration = 8;
        state.deltasReverted = true;
        state.mapRunClosed = true;
        state.refunds = { { RefundKind::Tribute, 0x010008B0, 3, true }, { RefundKind::Map, 0x010008B1, 1, false } };
        state.returns[0].status = ReturnStatus::Done;
        state.returns[1].status = ReturnStatus::Lost;
        state.returns[1].attempts = kMaxReturnAttempts;
        state.returns[2].attempts = 3;
        state.virtualRefs.front().released = true;
        state.worldDrops.front().status = WorldDropStatus::Retry;
        state.starterGranted = true;
        state.lostCount = 9;
        return std::move(state);
    }

    void RefreshCrc(std::vector<std::uint8_t>& bytes)
    {
        const auto payloadSize = bytes.size() - sizeof(std::uint32_t);
        const auto crc = SaveRecordCrc32(std::span(bytes).first(payloadSize));
        std::memcpy(bytes.data() + payloadSize, &crc, sizeof(crc));
    }

    void CheckDamaged(std::span<const std::uint8_t> bytes, std::uint32_t version = kMigrationVersion)
    {
        POE_CHECK(ReadMigrationRecord(version, bytes).status == MigrationRecordStatus::Damaged);
    }
}

POE_TEST("MIGR v1 round trips every field and preserves the original MAPS bytes")
{
    const auto state = FullMigration();
    const auto bytes = WriteMigrationRecord(state);
    const auto parsed = ReadMigrationRecord(kMigrationVersion, bytes);
    POE_CHECK(parsed.status == MigrationRecordStatus::Valid);
    POE_CHECK(parsed.state == state);
    POE_CHECK(parsed.state.returns.front().instanceId == 5);
    POE_CHECK(parsed.state.returns[5].instanceId == 12);
    POE_CHECK(parsed.state.returns.back().instanceId == 0);
    POE_CHECK(parsed.unresolvedCount == 0);
    POE_CHECK(WriteMigrationRecord(parsed.state) == bytes);
    constexpr std::array<std::uint8_t, 12> prefix{ 4, 0, 0, 0, 'p', 'o', 'e', '1', 7, 0, 0, 0 };
    POE_CHECK(std::ranges::equal(std::span(bytes).first(prefix.size()), prefix));
}

POE_TEST("MIGR v1 rejects every truncated prefix, bad CRC, extra bytes and unsupported versions")
{
    const auto bytes = WriteMigrationRecord(FullMigration());
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        CheckDamaged(std::span(bytes).first(size));
    }
    CheckDamaged(bytes, 0);
    CheckDamaged(bytes, kMigrationVersion + 1);
    auto corrupted = bytes;
    corrupted.back() ^= 1;
    CheckDamaged(corrupted);
    corrupted = bytes;
    corrupted.insert(corrupted.end() - sizeof(std::uint32_t), 0);
    RefreshCrc(corrupted);
    CheckDamaged(corrupted);
}

POE_TEST("MIGR v1 enforces sizes and enum ranges even with a valid CRC")
{
    auto state = FullMigration();
    auto bytes = WriteMigrationRecord(state);
    bytes[1] = 2; // Source game string length = 516.
    RefreshCrc(bytes);
    CheckDamaged(bytes);

    bytes = WriteMigrationRecord(state);
    const auto refundCountOffset = 2 * (sizeof(std::uint32_t) + 4 + sizeof(std::uint32_t)) +
        1 + 2 * sizeof(double) + 1 + 1 + sizeof(std::uint32_t) + sizeof(std::uint32_t) + state.mapRun.bytes.size() + 1;
    const std::uint32_t oversizedCount = kMaxListLen + 1;
    std::memcpy(bytes.data() + refundCountOffset, &oversizedCount, sizeof(oversizedCount));
    RefreshCrc(bytes);
    CheckDamaged(bytes);
    CheckDamaged(std::vector<std::uint8_t>(kMaxSaveRecordBytes + 1));

    state.step = static_cast<CleanupStep>(255);
    CheckDamaged(WriteMigrationRecord(state));
    state = FullMigration();
    state.returns.front().kind = static_cast<ReturnKind>(255);
    CheckDamaged(WriteMigrationRecord(state));
    state = FullMigration();
    state.returns.front().status = static_cast<ReturnStatus>(255);
    CheckDamaged(WriteMigrationRecord(state));
    state = FullMigration();
    state.returns.back().jewelColor = 255;
    CheckDamaged(WriteMigrationRecord(state));
    state = FullMigration();
    state.refunds.front().kind = static_cast<RefundKind>(255);
    CheckDamaged(WriteMigrationRecord(state));
    state = FullMigration();
    state.virtualRefs.front().slot = static_cast<VirtualSlot>(255);
    CheckDamaged(WriteMigrationRecord(state));
    state = FullMigration();
    state.worldDrops.front().status = static_cast<WorldDropStatus>(255);
    CheckDamaged(WriteMigrationRecord(state));
}

POE_TEST("MIGR v1 remaps FormIDs, isolates losses and keeps the jewel color fallback")
{
    const auto state = FullMigration();
    const auto result = ReadMigrationRecord(kMigrationVersion, WriteMigrationRecord(state),
        [](std::uint32_t raw, std::uint32_t& resolved) {
            if (raw == kChest || raw == kJewel || raw == kActor || raw == 0x010008B1 ||
                raw == 0xFF000002 || raw == 0xFF000003) {
                return false;
            }
            resolved = raw == kPlayerRefFormId ? raw : raw + 0x100;
            return true;
        });
    POE_CHECK(result.status == MigrationRecordStatus::Valid);
    POE_CHECK(result.unresolvedCount == 6);
    POE_CHECK(result.state.lostCount == state.lostCount + 5);
    POE_CHECK(result.state.returns[5].status == ReturnStatus::Lost);
    POE_CHECK(result.state.returns[6].status == ReturnStatus::Lost);
    POE_CHECK(result.state.returns[7].status == ReturnStatus::Pending);
    POE_CHECK(result.state.returns[8].baseFormId == 0);
    POE_CHECK(result.state.returns[8].jewelColor == 2);
    POE_CHECK(result.state.returns[8].status == ReturnStatus::Pending);
    POE_CHECK(result.state.refunds.front().baseFormId == state.refunds.front().baseFormId + 0x100);
    POE_CHECK(result.state.refunds.back().baseFormId == 0);
    POE_CHECK(result.state.virtualRefs.front().baseFormId == state.virtualRefs.front().baseFormId + 0x100);
    POE_CHECK(result.state.speedMods.empty());
    POE_CHECK(result.state.worldDrops.front().status == WorldDropStatus::CorpseGone);
    POE_CHECK(result.state.worldDrops.front().dropRefFormId == state.worldDrops.front().dropRefFormId + 0x100);
    POE_CHECK(result.state.mapRun == state.mapRun);
    const auto reloaded = ReadMigrationRecord(kMigrationVersion, WriteMigrationRecord(result.state));
    POE_CHECK(reloaded.state == result.state);
    POE_CHECK(reloaded.unresolvedCount == 0);
}

POE_TEST("Legacy migration collects deepest gems first and carries GKEY GEAR SPRT MAPS SPDM WDRP")
{
    auto batch = EncodeLegacy(LegacySnapshot());
    const auto result = Build(batch, [](std::uint32_t raw, std::uint32_t& resolved) {
        resolved = raw == kPlayerRefFormId ? raw : raw + 1;
        return true;
    });
    const auto& [state, report] = result;
    POE_CHECK(report.Succeeded() && !report.slotsDamaged);
    for (const auto status : { report.gemKeys, report.gear, report.spirit, report.maps, report.speedMods, report.worldDrops }) {
        POE_CHECK(status == LegacyRecordStatus::Valid);
    }
    POE_CHECK(state.sourceGame == kPoE1Pack.game && state.sourceGeneration == 0);
    POE_CHECK(state.targetGame == kPoE2Pack.game && state.targetGeneration == 1);
    POE_CHECK(state.step == CleanupStep::CloseMapRun);
    POE_CHECK(state.healthDelta == 21.75 && state.magickaDelta == -6.25);
    POE_CHECK(!state.deltasReverted && !state.mapRunClosed && !state.starterGranted);
    POE_CHECK(state.returns.size() == 9);
    const std::array<std::string_view, 7> gems{ "SpellSupport", "OldSupport", "OldSpell",
        "OldAttack", "OldTrigger", "BagSupport", "BagSpell" };
    const std::array<std::uint8_t, 7> depths{ 2, 1, 1, 0, 0, 2, 1 };
    for (std::size_t i = 0; i < gems.size(); ++i) {
        const bool slotted = i < 5;
        const auto& entry = state.returns[i];
        POE_CHECK(entry.gemId == gems[i]);
        POE_CHECK(entry.depth == depths[i]);
        POE_CHECK(entry.kind == (slotted ? ReturnKind::SlottedGem : ReturnKind::BagSupport));
        POE_CHECK(entry.recipientFormId == (slotted ? kPlayerRefFormId : kChest + 1));
    }
    POE_CHECK(state.returns[7].kind == ReturnKind::Charm && state.returns[7].charmKind == CharmKind::Topaz);
    POE_CHECK(state.returns[7].baseFormId == 0);
    POE_CHECK(state.returns[8].kind == ReturnKind::Jewel && state.returns[8].baseFormId == kJewel + 1);
    POE_CHECK(state.returns[8].jewelColor == 2);
    const std::vector<MigrationVirtualRef> expectedRefs{ { VirtualSlot::Ring2, { kPlayerRefFormId, 90 }, 0x0100089C },
        { VirtualSlot::Belt, { kPlayerRefFormId, 91 }, 0x0100089D } };
    POE_CHECK(state.virtualRefs == expectedRefs);
    const std::vector<MigrationSpeedMod> expectedSpeedMods{ { kActor + 1, -5.25 } };
    POE_CHECK(state.speedMods == expectedSpeedMods);
    const std::vector<MigrationWorldDrop> expectedDrops{ { 0xFF000002, 0xFF000003, 0xFF000004, 4 } };
    POE_CHECK(state.worldDrops == expectedDrops);
    auto& map = FindRecord(batch, kRecordMaps);
    POE_CHECK(state.mapRun.active && state.mapRun.version == map.version && state.mapRun.bytes == map.bytes);
}

POE_TEST("Legacy CORE keeps raw deltas when slots are broken and fails on damage to its front")
{
    auto snapshot = LegacySnapshot();
    snapshot.state.appliedHealthDelta = std::numeric_limits<double>::quiet_NaN();
    auto batch = EncodeLegacy(snapshot);
    auto& core = FindRecord(batch, kRecordCore);
    std::span<const std::uint8_t> slots = core.bytes;
    SkyrimPoE::Runtime::PersistentState decoded;
    POE_CHECK(ReadLegacyCoreV2Front(slots, decoded));
    const auto frontSize = core.bytes.size() - slots.size();
    core.bytes.resize(frontSize);
    const auto brokenSlots = Build(batch);
    POE_CHECK(brokenSlots.report.Succeeded() && brokenSlots.report.slotsDamaged);
    POE_CHECK(std::isnan(brokenSlots.state.healthDelta) && brokenSlots.state.magickaDelta == -6.25);
    POE_CHECK(JudgeLegacyDelta(brokenSlots.state.healthDelta, 25.0) == DeltaTrust::Untrusted);
    POE_CHECK(std::ranges::none_of(brokenSlots.state.returns, [](const auto& entry) {
        return entry.kind == ReturnKind::SlottedGem;
    }));
    const auto parsed = ReadMigrationRecord(kMigrationVersion, WriteMigrationRecord(brokenSlots.state));
    POE_CHECK(parsed.status == MigrationRecordStatus::Valid);
    POE_CHECK(std::bit_cast<std::uint64_t>(parsed.state.healthDelta) ==
        std::bit_cast<std::uint64_t>(snapshot.state.appliedHealthDelta));

    core.bytes.pop_back();
    const auto brokenFront = Build(batch);
    POE_CHECK(!brokenFront.report.Succeeded());
    POE_CHECK(brokenFront.report.coreFront == LegacyRecordStatus::Damaged);
    POE_CHECK(!brokenFront.report.failureReason.empty());
    POE_CHECK(brokenFront.state.step == CleanupStep::Begin);
}

POE_TEST("Legacy migration isolates damaged optional records and only reads the MAPS active header")
{
    auto batch = EncodeLegacy(LegacySnapshot());
    FindRecord(batch, kRecordGemKeys).bytes.pop_back();
    FindRecord(batch, kRecordGear).bytes.pop_back();
    FindRecord(batch, kRecordSpirit).bytes.pop_back();
    FindRecord(batch, kRecordSpeedMods).bytes.pop_back();
    FindRecord(batch, kRecordWorldDrops).bytes.pop_back();
    auto& map = FindRecord(batch, kRecordMaps);
    map.version = kLegacyKeyMapSaveVersion;
    map.bytes = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF, 0xFF };
    const auto [state, report] = Build(batch);
    POE_CHECK(report.Succeeded() && !report.slotsDamaged);
    for (const auto status : { report.gemKeys, report.gear, report.spirit, report.speedMods, report.worldDrops }) {
        POE_CHECK(status == LegacyRecordStatus::Damaged);
    }
    POE_CHECK(state.returns.size() == 5);
    POE_CHECK(state.virtualRefs.empty() && state.speedMods.empty() && state.worldDrops.empty());
    POE_CHECK(report.maps == LegacyRecordStatus::Valid);
    POE_CHECK(state.mapRun.active && state.mapRun.version == map.version && state.mapRun.bytes == map.bytes);
    map.bytes.resize(8);
    POE_CHECK(Build(batch).report.maps == LegacyRecordStatus::Damaged);

    std::erase_if(batch.records, [](const auto& record) { return record.type != kRecordCore; });
    const auto sparse = Build(batch);
    POE_CHECK(sparse.report.Succeeded());
    const auto& missing = sparse.report;
    for (const auto status : { missing.gemKeys, missing.gear, missing.spirit, missing.maps, missing.speedMods, missing.worldDrops }) {
        POE_CHECK(status == LegacyRecordStatus::Missing);
    }
    POE_CHECK(!Build({}).report.Succeeded());
}

POE_TEST("Legacy delta trust accepts zero or a finite same-direction correction within the margin")
{
    using enum DeltaTrust;
    POE_CHECK(JudgeLegacyDelta(0.0, 0.0) == Trusted);
    POE_CHECK(JudgeLegacyDelta(-0.0, std::numeric_limits<double>::quiet_NaN()) == Trusted);
    POE_CHECK(JudgeLegacyDelta(1e-7, -10.0) == Untrusted);
    POE_CHECK(JudgeLegacyDelta(-1e-7, 10.0) == Untrusted);
    POE_CHECK(JudgeLegacyDelta(1e-6, 0.0) == Untrusted);
    POE_CHECK(JudgeLegacyDelta(1e-7, 1e-7) == Trusted);
    for (const auto sign : { -1.0, 1.0 }) {
        POE_CHECK(JudgeLegacyDelta(sign * 10.01, sign * 10.0) == Trusted);
        POE_CHECK(JudgeLegacyDelta(sign * std::nextafter(10.01, 11.0), sign * 10.0) == Untrusted);
        POE_CHECK(JudgeLegacyDelta(sign * 10.5, sign * 10.0) == Untrusted);
        POE_CHECK(JudgeLegacyDelta(sign * 10.0, -sign * 10.0) == Untrusted);
        POE_CHECK(JudgeLegacyDelta(sign * std::numeric_limits<double>::infinity(), sign * 10.0) == Untrusted);
        POE_CHECK(JudgeLegacyDelta(sign * 10.0, sign * std::numeric_limits<double>::infinity()) == Untrusted);
    }
    POE_CHECK(JudgeLegacyDelta(std::numeric_limits<double>::quiet_NaN(), 10.0) == Untrusted);
    POE_CHECK(JudgeLegacyDelta(10.0, std::numeric_limits<double>::quiet_NaN()) == Untrusted);
}

POE_TEST("Legacy delta trust accepts the margin boundary in both directions")
{
    using enum DeltaTrust;
    for (const auto sign : { -1.0, 1.0 }) {
        POE_CHECK(JudgeLegacyDelta(sign * 0.012, sign * 0.002) == Trusted);
        POE_CHECK(JudgeLegacyDelta(sign * 0.0121, sign * 0.002) == Untrusted);
    }
}

POE_TEST("Legacy gem returns deduplicate nonzero instances shared by CORE and GKEY")
{
    auto snapshot = LegacySnapshot();
    snapshot.state.slots[2] = { 2, true, Gem(0, "ZeroSlot") };
    snapshot.state.slots[3] = { 3, true, Gem(2, "OldSupport") };
    snapshot.tracked.front().gem.socketedGems.push_back(Gem(0, "ZeroBag"));
    snapshot.tracked.front().gem.socketedGems.push_back(Gem(2, "OldSupport"));
    snapshot.tracked.front().gem.socketedGems.push_back(Gem(9, "OldSupport"));
    const auto result = Build(EncodeLegacy(snapshot));
    POE_CHECK(result.report.Succeeded());
    POE_CHECK(result.report.duplicateReturns == 2);
    POE_CHECK(std::ranges::count(result.state.returns, 2, &ReturnEntry::instanceId) == 1);
    POE_CHECK(std::ranges::count(result.state.returns, 9, &ReturnEntry::instanceId) == 1);
    POE_CHECK(std::ranges::count_if(result.state.returns, [](const auto& entry) {
        return (entry.kind == ReturnKind::SlottedGem || entry.kind == ReturnKind::BagSupport) && entry.instanceId == 0;
    }) == 2);
}

POE_TEST("Legacy SPDM accounts for invalid and unresolved input before engine reversion")
{
    auto snapshot = LegacySnapshot();
    snapshot.savedSpeedMods = { { 0, 3.0f }, { kActor + 1, std::numeric_limits<float>::quiet_NaN() },
        { kActor + 2, 0.009f }, { kActor + 3, 4.0f }, { kActor, -5.25f } };
    const auto result = BuildLegacyMigration(EncodeLegacy(snapshot), kPoE2Pack.game, kPoE2Pack.contentGeneration,
        [](auto raw, auto& resolved) {
            resolved = raw;
            return raw != kActor + 3;
        }, LegacyReturnDestination::Player);
    POE_CHECK(result.report.speedMods == LegacyRecordStatus::Valid);
    POE_CHECK(result.report.speedModCount == 5);
    POE_CHECK(result.report.speedModsFiltered == 4);
    POE_CHECK(result.report.speedModsUnresolved == 1);
    POE_CHECK(result.state.lostCount == 1);
    POE_CHECK(result.state.speedMods.size() + result.report.speedModsFiltered == result.report.speedModCount);
    POE_CHECK(result.state.speedMods.front().actorFormId == kActor);
}

POE_TEST("Truncated or duplicated MAPS with an active legacy header retains the active signal")
{
    auto batch = EncodeLegacy(LegacySnapshot());
    auto& map = FindRecord(batch, kRecordMaps);
    map.version = kLegacyIndexMapSaveVersion;
    map.bytes = { 0, 0, 0, 0, 0, 0, 0, 0, 2 }; // An active v1 run with only the atlas and flag.
    const auto resolve = [](auto raw, auto& resolved) { resolved = raw; return true; };
    const auto truncated = Build(batch);
    POE_CHECK(truncated.state.mapRun.active);
    POE_CHECK(!ReadMapSave(map.version, map.bytes, resolve).run);

    auto active = map;
    map.bytes.back() = 0;
    batch.records.push_back(std::move(active));
    const auto duplicated = Build(batch);
    POE_CHECK(duplicated.report.maps == LegacyRecordStatus::Damaged);
    POE_CHECK(duplicated.state.mapRun.active);
}

POE_TEST("Legacy CORE identification rejects a later record version with the same bytes")
{
    auto batch = EncodeLegacy(LegacySnapshot());
    std::erase_if(batch.records, [](const auto& record) { return record.type == kRecordGeneration; });
    auto& core = FindRecord(batch, kRecordCore);
    core.version = kLegacyCoreVersion;
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == SaveDisposition::CleanupFromLegacy);
    POE_CHECK(Build(batch).report.Succeeded());
    core.version = kLegacyCoreVersion + 1;
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == SaveDisposition::Preserve);
    POE_CHECK(!Build(batch).report.Succeeded());
}

POE_TEST("Save decisions resume only an intact matching MIGR, including with a legacy GENR")
{
    auto batch = EncodeLegacy(LegacySnapshot());
    auto migration = Build(batch).state;
    batch.records.push_back({ kRecordMigration, kMigrationVersion, WriteMigrationRecord(migration) });
    using enum SaveDisposition;
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == ResumeMigration);
    const auto mismatch = DecideSave(batch, false, kPoE1Pack);
    POE_CHECK(mismatch.disposition == Preserve);
    POE_CHECK(mismatch.reason.find("출발 poe1/0, 목표 poe2/1, 팩 poe1/0") != std::string::npos);
    batch.records.back().bytes.back() ^= 1;
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == Preserve);
    POE_CHECK(DecideSave(batch, true, kPoE2Pack).disposition == Normal);
    batch.records.back().bytes = WriteMigrationRecord(migration);
    batch.complete = false;
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == Preserve);
    batch.complete = true;
    std::erase_if(batch.records, [](const auto& record) { return record.type == kRecordGeneration; });
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == ResumeMigration);
    batch.records.push_back(batch.records.back());
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == Preserve);
}

POE_TEST("Unstamped saves need only the CORE front for cleanup and keep unchanged generation-zero decisions")
{
    auto batch = EncodeLegacy(LegacySnapshot());
    std::erase_if(batch.records, [](const auto& record) { return record.type == kRecordGeneration; });
    auto& core = FindRecord(batch, kRecordCore);
    std::span<const std::uint8_t> slots = core.bytes;
    SkyrimPoE::Runtime::PersistentState decoded;
    POE_CHECK(ReadLegacyCoreV2Front(slots, decoded));
    core.bytes.resize(core.bytes.size() - slots.size());
    using enum SaveDisposition;
    POE_CHECK(DecideSave(batch, false, kPoE1Pack).disposition == Normal);
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == CleanupFromLegacy);
    core.bytes.pop_back();
    POE_CHECK(DecideSave(batch, false, kPoE1Pack).disposition == Normal);
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == Preserve);
    std::erase_if(batch.records, [](const auto& record) { return record.type == kRecordCore; });
    POE_CHECK(DecideSave(batch, false, kPoE1Pack).disposition == Normal);
    POE_CHECK(DecideSave(batch, false, kPoE2Pack).disposition == Preserve);
    POE_CHECK(DecideSave({}, false, kPoE1Pack).disposition == FirstInstall);
}

namespace
{
    using enum CleanupStep;
    using enum StepOutcome;

    struct FakeOps final : CleanupOps
    {
        std::vector<CleanupStep> calls;

        StepOutcome CloseMapRun(MigrationState& state) override
        {
            calls.push_back(CleanupStep::CloseMapRun);
            state.mapRunClosed = true;
            for (auto& refund : state.refunds) {
                refund.done = true;
            }
            return Done;
        }

        StepOutcome ReturnSlottedGems(MigrationState& state) override
        {
            calls.push_back(CleanupStep::ReturnSlottedGems);
            if (std::ranges::count(calls, CleanupStep::ReturnSlottedGems) == 1) {
                return Waiting;
            }
            MarkReturned(state, ReturnKind::SlottedGem);
            return Done;
        }

        StepOutcome DetachBagSupports(MigrationState& state) override
        {
            calls.push_back(CleanupStep::DetachBagSupports);
            auto& successful = state.returns[5];
            POE_CHECK(successful.kind == ReturnKind::BagSupport);
            successful.status = ReturnStatus::Done;
            return Retry;
        }

        StepOutcome ReturnJewelsAndCharms(MigrationState& state) override
        {
            calls.push_back(CleanupStep::ReturnJewelsAndCharms);
            MarkReturned(state, ReturnKind::Jewel);
            MarkReturned(state, ReturnKind::Charm);
            for (auto& ref : state.virtualRefs) {
                ref.released = true;
            }
            return Done;
        }

        StepOutcome RevertEngineState(MigrationState& state) override
        {
            calls.push_back(CleanupStep::RevertEngineState);
            state.healthDelta = 0.0;
            state.magickaDelta = 0.0;
            state.deltasReverted = true;
            return Done;
        }

        StepOutcome SpeedModsAndWorldDrops(MigrationState& state) override
        {
            calls.push_back(CleanupStep::SpeedModsAndWorldDrops);
            if (std::ranges::count(calls, CleanupStep::SpeedModsAndWorldDrops) == 1) {
                state.worldDrops.front().status = WorldDropStatus::Retry;
                return Retry;
            }
            state.speedMods.clear();
            state.worldDrops.front().status = WorldDropStatus::Done;
            return Done;
        }

        StepOutcome ClearCosave(MigrationState&) override
        {
            calls.push_back(CleanupStep::ClearCosave);
            return Done;
        }

        StepOutcome GrantStarter(MigrationState& state) override
        {
            calls.push_back(CleanupStep::GrantStarter);
            state.starterGranted = true;
            return Done;
        }

        StepOutcome Finish(MigrationState&) override
        {
            calls.push_back(CleanupStep::Finish);
            return Done;
        }

        static void MarkReturned(MigrationState& state, ReturnKind kind)
        {
            for (auto& entry : state.returns) {
                if (entry.kind == kind && entry.status == ReturnStatus::Pending) {
                    entry.status = ReturnStatus::Done;
                }
            }
        }
    };

    auto RunCleanup(MigrationState state, bool reloadBetweenCalls)
    {
        FakeOps ops;
        for (int advances = 0; state.step != Complete; ++advances) {
            POE_CHECK(advances < 20);
            const auto formerStep = state.step;
            const auto before = state.returns;
            const auto outcome = AdvanceCleanup(state, ops);
            if (outcome == Waiting) {
                POE_CHECK(formerStep == state.step && before == state.returns);
            } else if (outcome == Retry) {
                POE_CHECK(formerStep == state.step);
            } else {
                POE_CHECK(static_cast<int>(state.step) == static_cast<int>(formerStep) + 1);
            }
            if (reloadBetweenCalls) {
                auto parsed = ReadMigrationRecord(kMigrationVersion, WriteMigrationRecord(state));
                POE_CHECK(parsed.status == MigrationRecordStatus::Valid);
                state = std::move(parsed.state);
            }
        }
        POE_CHECK(AdvanceCleanup(state, ops) == Done);
        return std::pair{ std::move(state), std::move(ops.calls) };
    }
}

POE_TEST("Cleanup completes in order with Waiting, five return retries and a retry in step six across saves")
{
    auto state = Build(EncodeLegacy(LegacySnapshot())).state;
    state.step = CleanupStep::Begin;
    state.refunds.push_back({ RefundKind::Map, 0x010008B1, 1, false });
    const auto uninterrupted = RunCleanup(state, false);
    const auto resumed = RunCleanup(state, true);
    POE_CHECK(uninterrupted == resumed);
    const auto& [finished, calls] = resumed;
    POE_CHECK(finished.step == CleanupStep::Complete && finished.deltasReverted && finished.starterGranted);
    POE_CHECK(finished.mapRunClosed && finished.refunds.front().done);
    POE_CHECK(finished.lostCount == 1);
    POE_CHECK(finished.returns[0].attempts == 0);
    POE_CHECK(finished.returns[5].status == ReturnStatus::Done && finished.returns[5].attempts == 0);
    POE_CHECK(finished.returns[6].status == ReturnStatus::Lost && finished.returns[6].attempts == kMaxReturnAttempts);
    POE_CHECK(finished.returns[7].attempts == 0 && finished.returns[8].attempts == 0);
    const std::vector expectedCalls{ CleanupStep::CloseMapRun, CleanupStep::ReturnSlottedGems, CleanupStep::ReturnSlottedGems,
        CleanupStep::DetachBagSupports, CleanupStep::DetachBagSupports, CleanupStep::DetachBagSupports,
        CleanupStep::DetachBagSupports, CleanupStep::DetachBagSupports, CleanupStep::ReturnJewelsAndCharms,
        CleanupStep::RevertEngineState, CleanupStep::SpeedModsAndWorldDrops, CleanupStep::SpeedModsAndWorldDrops,
        CleanupStep::ClearCosave, CleanupStep::GrantStarter, CleanupStep::Finish };
    POE_CHECK(calls == expectedCalls);
}

POE_TEST("Cleanup skips empty lists and still calls the four mandatory steps")
{
    MigrationState state;
    state.sourceGame = "poe1";
    state.targetGame = "poe2";
    const auto [finished, calls] = RunCleanup(state, true);
    const std::vector expectedCalls{ CleanupStep::RevertEngineState, CleanupStep::ClearCosave,
        CleanupStep::GrantStarter, CleanupStep::Finish };
    POE_CHECK(finished.step == CleanupStep::Complete);
    POE_CHECK(calls == expectedCalls);
}
