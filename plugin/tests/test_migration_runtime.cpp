#include "core/Migration.h"
#include "core/SaveRecordFormat.h"
#include "runtime/AutoTest.h"
#include "test_harness.h"

#include <sstream>

using namespace SkyrimPoE::Core;
using namespace SkyrimPoE::Core::SaveRecord;
using SkyrimPoE::Runtime::AutoTestMarker::ParseMarker;

namespace
{
    std::vector<std::uint8_t> EncodeMapRun(std::uint32_t version, const ActiveMapState& run)
    {
        std::vector<std::uint8_t> bytes;
        const auto write = [&](const void* source, std::size_t count) {
            const auto* begin = static_cast<const std::uint8_t*>(source);
            bytes.insert(bytes.end(), begin, begin + count);
            return true;
        };
        if (version == kMapsVersion) {
            POE_CHECK(WriteMapRunSave(write, run));
            return bytes;
        }
        const auto pod = [&](const auto& value) { return write(&value, sizeof(value)); };
        const auto flag = [&](bool value) { return pod(static_cast<std::uint8_t>(value)); };
        POE_CHECK(WriteAtlasSave(write, AtlasState{}));
        flag(run.active);
        flag(run.isArena4);
        flag(run.bossKilled);
        flag(run.failed);
        if (version == kLegacyKeyMapSaveVersion) {
            flag(run.playerAuthorized);
            flag(run.endRequested);
        }
        pod(run.runId);
        pod(run.tier);
        if (version == kLegacyIndexMapSaveVersion) {
            pod(std::uint8_t{ 3 });
        } else {
            POE_CHECK(WriteMapSaveIdentity(write, run));
        }
        pod(run.portalsRemaining);
        pod(run.arenaPhase);
        POE_CHECK(WriteSaveAffixList(write, run.mods));
        pod(run.returnCellFormId);
        pod(run.returnWorldspaceFormId);
        write(run.returnPos.data(), sizeof(run.returnPos));
        write(run.returnAngle.data(), sizeof(run.returnAngle));
        pod(run.bossRefFormId);
        pod(static_cast<std::uint32_t>(run.spawnedRefFormIds.size()));
        for (const auto spawn : run.spawnedRefFormIds) {
            pod(spawn);
        }
        return bytes;
    }

    class OneTickOps final : public CleanupOps
    {
    public:
        std::vector<CleanupStep> called;

        StepOutcome CloseMapRun(MigrationState& state) override
        {
            state.mapRunClosed = true;
            return Record(state);
        }

        StepOutcome ReturnSlottedGems(MigrationState& state) override { return Return(state, ReturnKind::SlottedGem); }
        StepOutcome DetachBagSupports(MigrationState& state) override { return Return(state, ReturnKind::BagSupport); }
        StepOutcome ReturnJewelsAndCharms(MigrationState& state) override { return Return(state, ReturnKind::Charm); }
        StepOutcome RevertEngineState(MigrationState& state) override { return Record(state); }
        StepOutcome SpeedModsAndWorldDrops(MigrationState& state) override { return Record(state); }
        StepOutcome ClearCosave(MigrationState& state) override { return Record(state); }
        StepOutcome GrantStarter(MigrationState& state) override
        {
            state.starterGranted = true;
            return Record(state);
        }
        StepOutcome Finish(MigrationState& state) override { return Record(state); }

    private:
        StepOutcome Record(const MigrationState& state)
        {
            called.push_back(state.step);
            return StepOutcome::Done;
        }

        StepOutcome Return(MigrationState& state, ReturnKind kind)
        {
            for (auto& entry : state.returns) {
                if (entry.kind == kind && entry.status == ReturnStatus::Pending) {
                    const bool lost = entry.gemId == "missing";
                    entry.status = lost ? ReturnStatus::Lost : ReturnStatus::Done;
                    state.lostCount += lost;
                }
            }
            return Record(state);
        }
    };
}

POE_TEST("Cleanup AutoTest marker isolates its mode and guards the baseline save stems")
{
    std::istringstream valid("save=Hero.01.ESS\r\ntemp_save=Test.ess\r\nonly=ClEaNuP\r\nquit=true\r\n");
    const auto cleanup = ParseMarker(valid);
    POE_CHECK(cleanup && cleanup->cleanupOnly && !cleanup->saveOnly && !cleanup->routeOnly && !cleanup->enemyEsOnly);
    POE_CHECK(cleanup->baselineSave == "Hero.01" && cleanup->tempSave == "Test" && cleanup->quitWhenDone);
    std::istringstream defaultQuit("save=Hero.ess\nonly=cleanup\n");
    const auto defaultCleanup = ParseMarker(defaultQuit);
    POE_CHECK(defaultCleanup && defaultCleanup->cleanupOnly && defaultCleanup->quitWhenDone);
    std::istringstream stayInCleanup("save=Hero.ess\nonly=cleanup\nquit=false\n");
    POE_CHECK(!ParseMarker(stayInCleanup));
    for (const auto* mode : { "h27", "es", "save" }) {
        std::istringstream stayInOrdinaryMode(std::string("save=Hero.ess\nquit=false\nonly=") + mode);
        const auto ordinaryMode = ParseMarker(stayInOrdinaryMode);
        POE_CHECK(ordinaryMode && !ordinaryMode->cleanupOnly && !ordinaryMode->quitWhenDone);
    }
    for (const auto* name : { "temp.ESS", "tEmP_FiNaL.EsS" }) {
        std::istringstream conflict(std::string("save=") + name + "\ntemp_save=TEMP.ess\nonly=cleanup\n");
        POE_CHECK(!ParseMarker(conflict));
    }
    std::istringstream unicodeConflict("save=\xC3\x89toile_final.ess\ntemp_save=\xC3\xA9toile.ess\nonly=cleanup\n");
    POE_CHECK(!ParseMarker(unicodeConflict));
    for (const auto* modes : { "only=h27\nonly=cleanup", "only=cleanup\nonly=es", "only=save\nonly=cleanup" }) {
        std::istringstream conflictingModes(modes);
        POE_CHECK(!ParseMarker(conflictingModes));
    }
    std::istringstream ordinary("save=Temp_final.ess\ntemp_save=Temp\nonly=save\n");
    const auto saveOnly = ParseMarker(ordinary);
    POE_CHECK(saveOnly && saveOnly->saveOnly && !saveOnly->cleanupOnly);
}

POE_TEST("Cleanup save target survives content updates and requires valid content")
{
    std::optional<SaveGeneration> content;
    std::optional<SaveGeneration> test{ { "poe2", 1, "autotest", "cleanup", "0.1" } };
    POE_CHECK(SelectSaveTarget(content, test, true) == nullptr);
    content = SaveGeneration{ "poe1", 0, "3.30", "actual", "0.1" };
    POE_CHECK(SelectSaveTarget(content, test, true) == &*test);
    content = SaveGeneration{ "poe1", 0, "3.30", "updated", "0.2" };
    const auto* target = SelectSaveTarget(content, test, true);
    POE_CHECK(target && target->game == "poe2" && target->contentGeneration == 1);
    POE_CHECK(content->game == "poe1" && content->contentGeneration == 0 && content->buildId == "updated");
    test.reset();
    POE_CHECK(SelectSaveTarget(content, test, true) == &*content);
}

POE_TEST("Cleanup target requires the next PreLoad to match the requested save")
{
    SkyrimPoE::Runtime::AutoTestMarker::CleanupLoadRequest request;
    const std::optional<SaveGeneration> content{ { "poe1", 0, "3.30", "actual", "0.1" } };
    const std::optional<SaveGeneration> test{ { "poe2", 1, "autotest", "cleanup", "0.1" } };
    const auto select = [&](std::string_view name) { return SelectSaveTarget(content, test, request.Match(name)); };

    POE_CHECK(select("Hero") == &*content);
    request.Expect("Hero");
    POE_CHECK(select("Other.ess") == &*content);
    POE_CHECK(select("Hero") == &*content);

    request.Expect("Hero");
    POE_CHECK(select(R"(Saves\hErO.ESS)") == &*test);
    POE_CHECK(select("Hero") == &*content);

    request.Expect("poe_final");
    POE_CHECK(select("POE_FINAL.ess") == &*test);
    request.Expect("poe_final");
    request.Clear();
    POE_CHECK(select("poe_final") == &*content);
}

POE_TEST("A failed matched cleanup load replays the previous world's generation")
{
    SkyrimPoE::Runtime::AutoTestMarker::CleanupLoadRequest request;
    const std::optional<SaveGeneration> content{ { "poe1", 0, "3.30", "actual", "0.1" } };
    const std::optional<SaveGeneration> test{ { "poe2", 1, "autotest", "cleanup", "0.1" } };
    const auto world = *content;
    request.Expect("Hero");

    SaveRecordBatch snapshot;
    snapshot.complete = WriteSnapshot({}, world, [&](RawSaveRecord&& record) {
        snapshot.records.push_back(std::move(record));
        return true;
    });
    POE_CHECK(snapshot.complete);
    SaveRecordTransition transition;
    transition.BeginLoad(std::move(snapshot));
    POE_CHECK(SelectSaveTarget(content, test, request.Match("Hero.ess")) == &*test);
    transition.FinishLoad(false);
    const auto& records = transition.RecordsForSave();
    const auto record = std::ranges::find(records.records, kRecordGeneration, &RawSaveRecord::type);
    POE_CHECK(record != records.records.end());
    const auto stamp = ReadGenerationRecord(record->version, record->bytes);
    POE_CHECK(stamp.status == GenerationRecordStatus::Valid && stamp.value == world);
}

POE_TEST("Cleanup rejects an invalid UTF-8 save stem and the next marker parses normally")
{
    std::istringstream malformed("save=\xFF.ess\ntemp_save=Trial\nonly=cleanup\n");
    POE_CHECK(!ParseMarker(malformed));
    std::istringstream valid("save=Hero.ess\ntemp_save=Trial\nonly=cleanup\n");
    POE_CHECK(ParseMarker(valid).has_value());
}

POE_TEST("Cleanup decodes MAPS v1 v2 and v3 without content and remaps only resolved FormIDs")
{
    ActiveMapState oldRun;
    oldRun.active = true;
    oldRun.runId = 1;
    oldRun.dungeonKey = "retired_dungeon";
    oldRun.packVersion = 42;
    oldRun.mods = { { "retired_affix", { 2.0 } } };
    oldRun.returnCellFormId = 0x01000030;
    oldRun.returnWorldspaceFormId = 0x01000031;
    oldRun.bossRefFormId = 0xFF000031;
    oldRun.spawnedRefFormIds = { 0xFF000032, 0xFF000033 };
    for (const auto version : { kLegacyIndexMapSaveVersion, kLegacyKeyMapSaveVersion, kMapsVersion }) {
        const auto bytes = EncodeMapRun(version, oldRun);
        const auto parsed = ReadMapSave(version, bytes, [](auto raw, auto& remapped) {
            remapped = raw + 0x10;
            return raw != 0xFF000033;
        });
        POE_CHECK(parsed.run && parsed.run->active);
        POE_CHECK(parsed.run->returnCellFormId == oldRun.returnCellFormId + 0x10);
        POE_CHECK(parsed.run->returnWorldspaceFormId == oldRun.returnWorldspaceFormId + 0x10);
        POE_CHECK(parsed.run->bossRefFormId == oldRun.bossRefFormId + 0x10);
        POE_CHECK(parsed.run->spawnedRefFormIds.size() == 1 && parsed.run->spawnedRefFormIds[0] == 0xFF000042);
        POE_CHECK(parsed.run->mods.size() == 1 && parsed.run->mods[0].affixId == "retired_affix");
        if (version == kLegacyIndexMapSaveVersion) {
            POE_CHECK(parsed.legacyDungeonIndex == 3 && parsed.run->dungeonKey.empty());
        } else {
            POE_CHECK(parsed.run->dungeonKey == oldRun.dungeonKey && parsed.run->packVersion == oldRun.packVersion);
        }
    }
}

POE_TEST("Legacy cleanup targets the player for bag supports and rejects duplicate CORE")
{
    Snapshot snapshot;
    constexpr std::uint32_t missingOwner = 0xFF000123;
    GemInstance parent;
    parent.gemId = "Parent";
    parent.socketedGems.push_back({ .gemId = "Support" });
    snapshot.tracked.push_back({ .key = { missingOwner, 2 }, .isWorld = true, .gem = parent });
    SaveRecordBatch batch;
    const SaveGeneration content{ "poe1", 0, "3.30", "build", "0.1" };
    POE_CHECK(WriteSnapshot(snapshot, content, [&](RawSaveRecord&& record) {
        batch.records.push_back(std::move(record));
        return true;
    }));
    const auto cleanup = BuildLegacyMigration(batch, "poe2", 1, [missingOwner](auto raw, auto& resolved) {
        resolved = raw;
        return raw != missingOwner;
    }, LegacyReturnDestination::Player);
    POE_CHECK(cleanup.report.Succeeded() && cleanup.state.returns.size() == 1 && cleanup.state.lostCount == 0);
    const auto& support = cleanup.state.returns.front();
    POE_CHECK(support.kind == ReturnKind::BagSupport && support.recipientFormId == kPlayerRefFormId);
    const auto core = std::ranges::find(batch.records, kRecordCore, &RawSaveRecord::type);
    POE_CHECK(core != batch.records.end());
    const auto duplicate = *core;
    batch.records.push_back(duplicate);
    const auto rejected = BuildLegacyMigration(batch, "poe2", 1);
    POE_CHECK(!rejected.report.Succeeded() && rejected.report.coreFront == LegacyRecordStatus::Damaged);
}

POE_TEST("One-tick cleanup completes nine steps and counts a lost return exactly once")
{
    MigrationState state;
    state.mapRun.active = true;
    state.returns = { { .kind = ReturnKind::SlottedGem, .gemId = "missing" },
        { .kind = ReturnKind::BagSupport, .gemId = "found" }, { .kind = ReturnKind::Charm } };
    state.speedMods.push_back({ 0x01000030, 2.0 });
    OneTickOps ops;
    POE_CHECK(CompleteCleanup(state, ops) == StepOutcome::Done && state.step == CleanupStep::Complete);
    POE_CHECK(state.lostCount == 1 && state.returns[0].status == ReturnStatus::Lost);
    POE_CHECK(state.returns[1].status == ReturnStatus::Done && state.returns[2].status == ReturnStatus::Done);
    POE_CHECK(ops.called.size() == 9 && state.starterGranted);
    for (std::size_t i = 0; i < ops.called.size(); ++i) {
        POE_CHECK(static_cast<std::size_t>(ops.called[i]) == i + 1);
    }
    POE_CHECK(CompleteCleanup(state, ops) == StepOutcome::Done && ops.called.size() == 9 && state.lostCount == 1);
}

POE_TEST("Legacy cleanup reports discarded TREE ITEM and ATLS records with damaged payloads")
{
    SaveRecordBatch batch;
    const SaveGeneration content{ "poe1", 0, "3.30", "build", "0.1" };
    POE_CHECK(WriteSnapshot(Snapshot{}, content, [&](RawSaveRecord&& record) {
        batch.records.push_back(std::move(record));
        return true;
    }));
    const auto clean = BuildLegacyMigration(batch, "poe2", 1);
    POE_CHECK(clean.report.Succeeded() && clean.report.tree == LegacyRecordStatus::Valid &&
        clean.report.items == LegacyRecordStatus::Valid && clean.report.atlas == LegacyRecordStatus::Valid);
    POE_CHECK(clean.report.treeNodeCount == 0 && clean.report.itemCount == 0 && clean.report.atlasClears == 0);
    for (const auto type : { kRecordTree, kRecordItems, kRecordAtlas }) {
        const auto record = std::ranges::find(batch.records, type, &RawSaveRecord::type);
        POE_CHECK(record != batch.records.end() && !record->bytes.empty());
        record->bytes.pop_back();
    }
    const auto damaged = BuildLegacyMigration(batch, "poe2", 1);
    POE_CHECK(damaged.report.Succeeded() && damaged.report.tree == LegacyRecordStatus::Damaged &&
        damaged.report.items == LegacyRecordStatus::Damaged && damaged.report.atlas == LegacyRecordStatus::Damaged);
}

POE_TEST("Legacy MAPS v1 drop membership requires a matching corpse in shared cells")
{
    POE_CHECK(!IsLegacyRunDrop(false, true, false));
    POE_CHECK(IsLegacyRunDrop(true, true, false));
    POE_CHECK(IsLegacyRunDrop(true, false, false));
    POE_CHECK(IsLegacyRunDrop(false, true, true));
    POE_CHECK(!IsLegacyRunDrop(false, false, true));
}

POE_TEST("Legacy WDRP subtracts exactly the observed partial return from the original stack")
{
    POE_CHECK(RemainingLegacyDropCount(5, 10, 15) == 0);
    POE_CHECK(RemainingLegacyDropCount(5, 10, 12) == 3);
    POE_CHECK(RemainingLegacyDropCount(2, 10, 11) == 1);
    POE_CHECK(RemainingLegacyDropCount(5, 10, 10) == 5);
    POE_CHECK(RemainingLegacyDropCount(5, 10, 9) == 5);
    POE_CHECK(RemainingLegacyDropCount(5, 10, 16) == 5);
}
