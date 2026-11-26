#include "core/CleanupVerification.h"
#include "core/GemForms.h"
#include "core/Migration.h"
#include "core/SaveRecordFormat.h"
#include "core/TestSessionGate.h"
#include "runtime/AutoTest.h"
#include "test_harness.h"

#include <sstream>

using namespace SkyrimPoE::Core;
using namespace SkyrimPoE::Core::SaveRecord;
using SkyrimPoE::Runtime::AutoTestMarker::ParseMarker;

POE_TEST("Gem hand retries send at most five requests two seconds apart")
{
    using SkyrimPoE::Runtime::AutoTestMarker::GemsRetry;
    using Action = GemsRetry::Action;
    GemsRetry retry(5);
    for (unsigned attempt = 0; attempt < 5; ++attempt) {
        const double now = 1.0 + attempt * 2.0;
        POE_CHECK(retry.Poll(now) == Action::Request);
        POE_CHECK(retry.Poll(now + 1.99) == Action::Wait);
        POE_CHECK(retry.Attempts() == attempt + 1);
    }
    POE_CHECK(retry.Poll(11.0) == Action::Exhausted);
    POE_CHECK(retry.Poll(90.0) == Action::Exhausted);
    POE_CHECK(retry.Attempts() == 5);
    POE_CHECK(retry.ExhaustionMessage("Right-hand proxy equip") ==
        "Right-hand proxy equip did not complete after 5 requests.");
}

POE_TEST("Gem load and save retries have independent budgets and no catch-up burst")
{
    using SkyrimPoE::Runtime::AutoTestMarker::GemsRetry;
    using Action = GemsRetry::Action;
    GemsRetry load(3);
    GemsRetry save(3);
    POE_CHECK(load.Poll(0, 4) == Action::Request);
    POE_CHECK(load.Poll(3, 4) == Action::Wait);
    POE_CHECK(load.Poll(20, 4) == Action::Request);
    POE_CHECK(load.Poll(20.1, 4) == Action::Wait);
    POE_CHECK(save.Poll(20.1) == Action::Request);
    POE_CHECK(load.Poll(24, 4) == Action::Request);
    POE_CHECK(load.Poll(28, 4) == Action::Exhausted);
    POE_CHECK(save.Attempts() == 1);
}

POE_TEST("Gem right-hand readiness accepts selection only when the process hand is empty")
{
    using SkyrimPoE::Runtime::AutoTestMarker::IsGemsProxyInHand;
    POE_CHECK(IsGemsProxyInHand(true, true, false));
    POE_CHECK(IsGemsProxyInHand(false, false, true));
    POE_CHECK(!IsGemsProxyInHand(true, false, true));
    POE_CHECK(!IsGemsProxyInHand(false, false, false));
}

POE_TEST("Test session autosave settings restore and verify by re-reading each collection")
{
    using SkyrimPoE::Runtime::AutoTestMarker::AutoSaveCollection;
    using SkyrimPoE::Runtime::AutoTestMarker::AutoSaveSettings;

    AutoSaveSettings settings;
    bool saveOnRest = true;
    bool saveOnWait = false;
    bool disableAutosave = false;
    // Two engine collections may share the same underlying bool pointer.
    settings.Set(AutoSaveCollection::IniPref, "bSaveOnRest:Main", saveOnRest, false);
    settings.Set(AutoSaveCollection::IniSetting, "bSaveOnRest:Main", saveOnRest, false);
    settings.Set(AutoSaveCollection::IniPref, "bSaveOnWait:Main", saveOnWait, false);
    settings.Set(AutoSaveCollection::GameSetting, "bDisableAutoSave", disableAutosave, true);
    POE_CHECK(!saveOnRest && !saveOnWait && disableAutosave);
    POE_CHECK(settings.CapturedCount() == 4);

    auto readActual = [&](AutoSaveCollection col, std::string_view name) -> std::optional<bool> {
        if (name == "bSaveOnRest:Main" && (col == AutoSaveCollection::IniPref || col == AutoSaveCollection::IniSetting))
            return saveOnRest;
        if (name == "bSaveOnWait:Main" && col == AutoSaveCollection::IniPref)
            return saveOnWait;
        if (name == "bDisableAutoSave" && col == AutoSaveCollection::GameSetting)
            return disableAutosave;
        return std::nullopt;
    };
    std::size_t restored = 0;
    std::string detail;
    POE_CHECK(settings.RestoreAndVerify(readActual, restored, &detail) && restored == 4 && detail.empty());
    POE_CHECK(saveOnRest && !saveOnWait && !disableAutosave);
    POE_CHECK(settings.CapturedCount() == 0);

    // 실패 감지 1: 컬렉션에서 다시 읽은 값이 원래 값과 다르면 실패해야 한다.
    bool divergent = true;
    settings.Set(AutoSaveCollection::IniSetting, "bSaveOnTravel:Main", divergent, false);
    detail.clear();
    const bool mismatchDetected = !settings.RestoreAndVerify(
        [](AutoSaveCollection, std::string_view) -> std::optional<bool> { return false; },
        restored, &detail);
    POE_CHECK(mismatchDetected && restored == 1 && !detail.empty());

    // 실패 감지 2: 컬렉션에서 항목을 찾지 못해도 실패해야 한다.
    bool missing = false;
    settings.Set(AutoSaveCollection::GameSetting, "bDisableAutoSave", missing, true);
    detail.clear();
    const bool missingDetected = !settings.RestoreAndVerify(
        [](AutoSaveCollection, std::string_view) -> std::optional<bool> { return std::nullopt; },
        restored, &detail);
    POE_CHECK(missingDetected && restored == 1 && detail.find("missing") != std::string::npos);

    // F5: 같은 (컬렉션, 이름)이라도 포인터가 다르면 두 포인터의 원래 값을 모두 기록해 복구해야 한다.
    bool firstPtr = true;
    bool secondPtr = false;
    settings.Set(AutoSaveCollection::IniSetting, "bSaveOnTravel:Main", firstPtr, false);
    settings.Set(AutoSaveCollection::IniSetting, "bSaveOnTravel:Main", secondPtr, true);
    POE_CHECK(!firstPtr && secondPtr && settings.CapturedCount() == 2);
    std::size_t readCalls = 0;
    detail.clear();
    const bool bothOk = settings.RestoreAndVerify(
        [&](AutoSaveCollection, std::string_view) -> std::optional<bool> {
            return (readCalls++ == 0) ? firstPtr : secondPtr;
        },
        restored, &detail);
    POE_CHECK(bothOk && restored == 2 && detail.empty());
    POE_CHECK(firstPtr && !secondPtr && settings.CapturedCount() == 0);

    // A6: RestoreAndVerify를 두 번 부르면 첫 호출 뒤 목록이 비어 두 번째 호출은 0개·true를 돌려준다.
    bool secondCallVal = true;
    settings.Set(AutoSaveCollection::IniSetting, "bSaveOnTravel:Main", secondCallVal, false);
    std::size_t firstRestored = 0;
    std::string firstMismatch;
    const bool firstFail = !settings.RestoreAndVerify(
        [](AutoSaveCollection, std::string_view) -> std::optional<bool> { return false; },
        firstRestored, &firstMismatch);
    POE_CHECK(firstFail && firstRestored == 1 && !firstMismatch.empty() && settings.CapturedCount() == 0);
    std::size_t secondRestored = 99;
    std::string secondMismatch;
    const bool secondVacuousOk = settings.RestoreAndVerify(
        [](AutoSaveCollection, std::string_view) -> std::optional<bool> { return false; },
        secondRestored, &secondMismatch);
    POE_CHECK(secondVacuousOk && secondRestored == 0 && secondMismatch.empty());
}

POE_TEST("Gems and firstinstall markers reject overwritten save names, missing baselines and quit false")
{
    std::istringstream valid("only=gems\nsave=Baseline.ess\ntemp_save=Probe\nquit=true\n");
    const auto parsed = ParseMarker(valid);
    POE_CHECK(parsed && parsed->gemsOnly && !parsed->firstInstall && !parsed->cleanupOnly);
    POE_CHECK(parsed->EffectiveOutSave() == "Probe_final" && parsed->UsesTestSession());
    for (const auto* stem : { "", "probe", "PROBE_FINAL.ess", "Probe_h_restore", "PROBE_H_RESTORE_2026_2.ess" }) {
        std::istringstream conflict(std::string("only=gems\ntemp_save=Probe\nsave=") + stem + "\n");
        POE_CHECK(!ParseMarker(conflict));
    }
    std::istringstream stay("only=gems\nsave=Baseline\nquit=false\n");
    POE_CHECK(!ParseMarker(stay));

    // 처음 설치 시험은 실제 세대 1 패키지에서 독립 모드로 실행되며 같은 슬롯 보호를 받는다.
    std::istringstream first("only=firstinstall\nsave=Baseline.ess\ntemp_save=Probe\nquit=true\n");
    const auto firstParsed = ParseMarker(first);
    POE_CHECK(firstParsed && !firstParsed->gemsOnly && firstParsed->firstInstall && !firstParsed->cleanupOnly);
    POE_CHECK(firstParsed->EffectiveOutSave() == "Probe_final" && firstParsed->UsesTestSession());
    std::istringstream firstFinal("only=firstinstall\nsave=Probe_final\ntemp_save=Probe\nquit=true\n");
    POE_CHECK(!ParseMarker(firstFinal));
    std::istringstream firstStay("only=firstinstall\nsave=Baseline\nquit=false\n");
    POE_CHECK(!ParseMarker(firstStay));
    std::istringstream mixed("only=gems\nonly=firstinstall\nsave=Baseline\ntemp_save=Probe\nquit=true\n");
    POE_CHECK(!ParseMarker(mixed));

    // 타격 반응 단축 실행(결정 236~238)은 기준 세이브만 쓰고 젬·정리 모드와 함께 켤 수 없다.
    std::istringstream react("only=react\nsave=Baseline\n");
    const auto reactParsed = ParseMarker(react);
    POE_CHECK(reactParsed && reactParsed->reactOnly && !reactParsed->gemsOnly && !reactParsed->enemyEsOnly &&
        reactParsed->UsesTestSession());
    std::istringstream reactGems("only=react\nonly=gems\nsave=Baseline\ntemp_save=Probe\nquit=true\n");
    POE_CHECK(!ParseMarker(reactGems));
    std::istringstream reactEs("only=react\nonly=es\nsave=Baseline\n");
    POE_CHECK(!ParseMarker(reactEs));
    std::istringstream esReact("only=es\nonly=react\nsave=Baseline\n");
    POE_CHECK(!ParseMarker(esReact));
    std::istringstream reactStay("only=react\nsave=Baseline\nquit=false\n");
    POE_CHECK(!ParseMarker(reactStay));

    // W8: out_save 지정 및 충돌 검사.
    std::istringstream withOut("only=cleanup\nsave=Legacy.ess\ntemp_save=Probe\nout_save=g1_cleanup_genr.ess\nquit=true\n");
    const auto withOutParsed = ParseMarker(withOut);
    POE_CHECK(withOutParsed && withOutParsed->outSave == "g1_cleanup_genr" &&
        withOutParsed->EffectiveOutSave() == "g1_cleanup_genr");
    std::istringstream saveWithOut("only=save\nsave=g1_cleanup_genr\ntemp_save=Probe\nout_save=g1_save\n");
    const auto saveWithOutParsed = ParseMarker(saveWithOut);
    POE_CHECK(saveWithOutParsed && saveWithOutParsed->EffectiveOutSave() == "g1_save" && saveWithOutParsed->UsesTestSession());
    std::istringstream outConflict("only=save\nsave=g1_save\ntemp_save=Probe\nout_save=G1_SAVE.ess\n");
    POE_CHECK(!ParseMarker(outConflict));
    std::istringstream outSlash("only=cleanup\nsave=Legacy\nout_save=sub/g1_out\nquit=true\n");
    POE_CHECK(!ParseMarker(outSlash));
    std::istringstream outUnicodeConflict("only=cleanup\nsave=\xC3\x89toile.ess\nout_save=\xC3\xA9toile\nquit=true\n");
    POE_CHECK(!ParseMarker(outUnicodeConflict));
    std::istringstream allMode("save=Baseline\n");
    const auto allParsed = ParseMarker(allMode);
    POE_CHECK(allParsed && !allParsed->UsesTestSession());
}

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
    for (const auto* mode : { "cleanup", "gems", "firstinstall", "save", "es", "react" }) {
        std::istringstream stayInSessionMode(std::string("save=Hero.ess\nquit=false\nonly=") + mode);
        POE_CHECK(!ParseMarker(stayInSessionMode));
    }
    for (const auto* extra : { "only=h27\n", "" }) {
        std::istringstream stayInOrdinaryMode(std::string("save=Hero.ess\nquit=false\n") + extra);
        const auto ordinaryMode = ParseMarker(stayInOrdinaryMode);
        POE_CHECK(ordinaryMode && !ordinaryMode->UsesTestSession() && !ordinaryMode->quitWhenDone);
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

POE_TEST("Ready starter content and pending diff select only ungranted ready generation-one gems")
{
    using namespace SkyrimPoE::Core::GemForms;

    POE_CHECK(ReadyStarterContent(0).empty());
    POE_CHECK(ReadyStarterContent(2).empty());
    const auto ready = ReadyStarterContent(1);
    POE_CHECK(ready.size() == 1 && ready[0].contentId == "gem.Spark" && ready[0].localFormId == 0x000880u);
    POE_CHECK(StarterForms(0).size() == 28 && StarterForms(1).empty());

    const std::array<StarterContentItem, 4> futureReady{{
        { "gem.Spark", 0x880 },
        { "gem.Fireball", 0x87D },
        { "sup.Fork", 0x883 },
        { "gem.Fireball", 0x87D }, // 중복 ready 입력은 한 번만 고른다.
    }};

    // 아직 아무것도 안 받은 경우 순서대로 전부 선택
    const std::vector<std::string> emptyGranted;
    const auto allPending = PendingStarterContent(futureReady, emptyGranted);
    POE_CHECK(allPending.size() == 3);
    POE_CHECK(allPending[0].contentId == "gem.Spark" && allPending[1].contentId == "gem.Fireball" &&
        allPending[2].contentId == "sup.Fork");

    // 기존 지급분 + 모르는 유효 ID가 섞여 있으면 늘어난 몫만 선택
    std::vector<std::string> granted{ "gem.Spark", "future.Unknown_Reward" };
    const auto diff = PendingStarterContent(futureReady, granted);
    POE_CHECK(diff.size() == 2 && diff[0].contentId == "gem.Fireball" && diff[1].contentId == "sup.Fork");

    // GiveStarterGems=false이면 콜백을 부르지 않고 기록도 바꾸지 않으며 모두 DisabledByConfig
    int grantCalls = 0;
    const auto disabledReport = ApplyStarterGrant(1, futureReady, false, granted, [&](const StarterContentItem&) {
        ++grantCalls;
        return std::optional<std::int32_t>{ 1 };
    });
    POE_CHECK(grantCalls == 0);
    POE_CHECK(!disabledReport.configEnabled && disabledReport.DisabledCount() == 2 &&
        disabledReport.GrantedCount() == 0 && disabledReport.FailedCount() == 0);
    POE_CHECK((granted == std::vector<std::string>{ "gem.Spark", "future.Unknown_Reward" }));

    // 부분 실패: 1 이상 증가(+1, +2)는 기록하고, 0·nullopt·예외는 기록하지 않는다.
    const std::array<StarterContentItem, 5> edgeReady{{
        { "gem.Spark", 0x880 },
        { "gem.Fireball", 0x87D },
        { "gem.Arc", 0x87F },
        { "sup.Fork", 0x883 },
        { "gem.IceNova", 0x816 },
    }};
    std::vector<std::string> partialGranted{ "future.Preserved" };
    const auto partial = ApplyStarterGrant(1, edgeReady, true, partialGranted,
        [](const StarterContentItem& item) -> std::optional<std::int32_t> {
            if (item.contentId == "gem.Spark") return 1;
            if (item.contentId == "gem.Fireball") return 0;
            if (item.contentId == "gem.Arc") return 2;
            if (item.contentId == "sup.Fork") return std::nullopt;
            throw std::runtime_error("simulated add failure");
        });
    POE_CHECK(partial.GrantedCount() == 2 && partial.FailedCount() == 3 && partial.DisabledCount() == 0);
    POE_CHECK(partial.entries[0].status == StarterGrantStatus::Granted && partial.entries[0].countDelta == 1);
    POE_CHECK(partial.entries[1].status == StarterGrantStatus::Failed && partial.entries[1].countDelta == 0);
    POE_CHECK(partial.entries[2].status == StarterGrantStatus::Granted && partial.entries[2].countDelta == 2);
    POE_CHECK(partial.entries[3].status == StarterGrantStatus::Failed && partial.entries[3].countDelta == 0);
    POE_CHECK(partial.entries[4].status == StarterGrantStatus::Failed && partial.entries[4].countDelta == 0);
    POE_CHECK((partialGranted == std::vector<std::string>{ "future.Preserved", "gem.Spark", "gem.Arc" }));

    // 다음 로드 보충: 실패했던 3개만 다시 시도하고 이미 받은 gem.Spark·gem.Arc는 건드리지 않는다.
    std::vector<std::string> retriedIds;
    const auto retryReport = ApplyStarterGrant(1, edgeReady, true, partialGranted,
        [&](const StarterContentItem& item) -> std::optional<std::int32_t> {
            retriedIds.emplace_back(item.contentId);
            return 1;
        });
    POE_CHECK((retriedIds == std::vector<std::string>{ "gem.Fireball", "sup.Fork", "gem.IceNova" }));
    POE_CHECK(retryReport.GrantedCount() == 3 && retryReport.FailedCount() == 0);
    POE_CHECK((partialGranted == std::vector<std::string>{
        "future.Preserved", "gem.Spark", "gem.Arc", "gem.Fireball", "sup.Fork", "gem.IceNova" }));

    // 모두 받은 뒤 재호출하면 아무것도 하지 않음
    int extraCalls = 0;
    const auto noopReport = ApplyStarterGrant(1, edgeReady, true, partialGranted,
        [&](const StarterContentItem&) -> std::optional<std::int32_t> {
            ++extraCalls;
            return 1;
        });
    POE_CHECK(extraCalls == 0 && noopReport.entries.empty());
}

POE_TEST("Cleanup step order runs GrantStarter once between ClearCosave and Finish and records same-tick diagnostics")
{
    using namespace SkyrimPoE::Core::GemForms;

    struct DiagnosticCleanupOps final : CleanupOps
    {
        std::vector<CleanupStep> called;
        std::vector<std::string> stateStarterIds;
        std::vector<StarterContentItem> readyItems;
        bool configEnabled = true;
        std::function<std::optional<std::int32_t>(const StarterContentItem&)> grantOne;
        bool isNormalMode = false;
        std::uint64_t currentFrame = 100;
        StarterGrantDiagnostics diag{};

        StepOutcome CloseMapRun(MigrationState& state) override { state.mapRunClosed = true; return Record(state); }
        StepOutcome ReturnSlottedGems(MigrationState& state) override { return Record(state); }
        StepOutcome DetachBagSupports(MigrationState& state) override { return Record(state); }
        StepOutcome ReturnJewelsAndCharms(MigrationState& state) override { return Record(state); }
        StepOutcome RevertEngineState(MigrationState& state) override { state.deltasReverted = true; return Record(state); }
        StepOutcome SpeedModsAndWorldDrops(MigrationState& state) override { return Record(state); }

        StepOutcome ClearCosave(MigrationState& state) override
        {
            stateStarterIds.clear();
            return Record(state);
        }

        StepOutcome GrantStarter(MigrationState& state) override
        {
            POE_CHECK(!isNormalMode);
            POE_CHECK(!called.empty() && called.back() == CleanupStep::ClearCosave);
            diag.report = ApplyStarterGrant(state.targetGeneration, readyItems, configEnabled, stateStarterIds, grantOne);
            diag.recordedIds = stateStarterIds;
            diag.grantTickId = diag.cleanupTickId;
            diag.grantFrame = currentFrame;
            diag.recordedBeforeNormal = !isNormalMode;
            state.starterGranted = true;
            return Record(state);
        }

        StepOutcome Finish(MigrationState& state) override
        {
            POE_CHECK(!called.empty() && called.back() == CleanupStep::GrantStarter);
            diag.committedIds = stateStarterIds;  // 실제 TickMigration은 커밋 뒤 g_state에서 다시 읽는다.
            POE_CHECK(diag.RanInCleanupTick());
            return Record(state);
        }

    private:
        StepOutcome Record(const MigrationState& state)
        {
            called.push_back(state.step);
            return StepOutcome::Done;
        }
    };

    // 1. 정상 지급(같은 틱, ClearCosave 뒤·Finish 앞 정확히 1회, Normal 전환 전 기록)
    {
        MigrationState state;
        state.targetGeneration = 1;
        DiagnosticCleanupOps ops;
        ops.stateStarterIds = { "stale.BeforeClear" };
        ops.readyItems.assign(ReadyStarterContent(1).begin(), ReadyStarterContent(1).end());
        ops.diag.cleanupTickId = 7;
        ops.diag.cleanupFrame = ops.currentFrame;
        ops.grantOne = [](const StarterContentItem&) { return std::optional<std::int32_t>{ 1 }; };

        POE_CHECK(CompleteCleanup(state, ops) == StepOutcome::Done && state.step == CleanupStep::Complete);
        ops.isNormalMode = true;

        POE_CHECK(std::ranges::count(ops.called, CleanupStep::GrantStarter) == 1);
        const auto clearPos = std::ranges::find(ops.called, CleanupStep::ClearCosave);
        const auto grantPos = std::ranges::find(ops.called, CleanupStep::GrantStarter);
        const auto finishPos = std::ranges::find(ops.called, CleanupStep::Finish);
        POE_CHECK(clearPos != ops.called.end() && grantPos != ops.called.end() && finishPos != ops.called.end());
        POE_CHECK(clearPos < grantPos && grantPos < finishPos);
        POE_CHECK(state.starterGranted && ops.diag.RanInCleanupTick());
        POE_CHECK(ops.diag.report.GrantedCount() == 1 && ops.diag.report.FailedCount() == 0);
        POE_CHECK((ops.diag.recordedIds == std::vector<std::string>{ "gem.Spark" }));
        POE_CHECK(ops.stateStarterIds == ops.diag.recordedIds);
        // 지급 결과가 상태에 들어가지 않았거나 다른 프레임에 기록되면 증거가 성립하지 않는다.
        auto notCommitted = ops.diag;
        notCommitted.committedIds.clear();
        POE_CHECK(!notCommitted.RanInCleanupTick());
        auto otherFrame = ops.diag;
        ++otherFrame.grantFrame;
        POE_CHECK(!otherFrame.RanInCleanupTick());
    }

    // 2. GiveStarterGems=false: 설정 제외로 진단에 남고 STRT는 비어 있음
    {
        MigrationState state;
        state.targetGeneration = 1;
        DiagnosticCleanupOps ops;
        ops.readyItems.assign(ReadyStarterContent(1).begin(), ReadyStarterContent(1).end());
        ops.configEnabled = false;
        ops.diag.cleanupTickId = 8;
        ops.diag.cleanupFrame = ops.currentFrame;
        ops.grantOne = [](const StarterContentItem&) { return std::optional<std::int32_t>{ 1 }; };

        POE_CHECK(CompleteCleanup(state, ops) == StepOutcome::Done && state.starterGranted);
        POE_CHECK(ops.diag.RanInCleanupTick());
        POE_CHECK(ops.diag.report.DisabledCount() == 1 && ops.diag.report.GrantedCount() == 0);
        POE_CHECK(ops.diag.recordedIds.empty() && ops.stateStarterIds.empty());
    }

    // 3. 부분 실패: 성공한 ID만 STRT에 남기고 실패 ID는 진단에 남긴다(정리는 완료).
    {
        MigrationState state;
        state.targetGeneration = 1;
        DiagnosticCleanupOps ops;
        ops.readyItems = { { "gem.Spark", 0x880 }, { "gem.Fireball", 0x87D } };
        ops.diag.cleanupTickId = 9;
        ops.diag.cleanupFrame = ops.currentFrame;
        ops.grantOne = [](const StarterContentItem& item) {
            return std::optional<std::int32_t>{ item.contentId == "gem.Spark" ? 1 : 0 };
        };

        POE_CHECK(CompleteCleanup(state, ops) == StepOutcome::Done && state.starterGranted);
        POE_CHECK(ops.diag.RanInCleanupTick());
        POE_CHECK(ops.diag.report.GrantedCount() == 1 && ops.diag.report.FailedCount() == 1);
        POE_CHECK((ops.diag.recordedIds == std::vector<std::string>{ "gem.Spark" }));
    }
}

POE_TEST("TestSessionGate enforces one-shot save allowance and sticky abort")
{
    using SkyrimPoE::Runtime::AutoTestMarker::SameSaveStem;
    TestSessionGate gate;

    // 세션 비활성 상태에서는 코세이브를 거부하지 않는다.
    POE_CHECK(!gate.IsActive() && !gate.IsAborted() && !gate.ShouldRefuseCoSave());

    gate.Begin("g1_out");
    POE_CHECK(gate.IsActive() && !gate.IsAborted() && gate.AllowedSlot() == "g1_out");
    // 명시 요청 없이는 거부.
    POE_CHECK(gate.ShouldRefuseCoSave());

    // 다른 슬롯·미요청·로딩 중·비정상 모드 모두 거부.
    POE_CHECK(!gate.AuthorizeSave("other_slot", true, false, true, SameSaveStem));
    POE_CHECK(gate.ShouldRefuseCoSave());
    POE_CHECK(!gate.AuthorizeSave("g1_out", false, false, true, SameSaveStem));
    POE_CHECK(gate.ShouldRefuseCoSave());
    POE_CHECK(!gate.AuthorizeSave("g1_out", true, true, true, SameSaveStem));
    POE_CHECK(gate.ShouldRefuseCoSave());
    POE_CHECK(!gate.AuthorizeSave("g1_out", true, false, false, SameSaveStem));
    POE_CHECK(gate.ShouldRefuseCoSave());

    // 허용 슬롯 요청은 정확히 1회만 통과하고 바로 소비된다.
    POE_CHECK(gate.AuthorizeSave("G1_OUT", true, false, true, SameSaveStem));
    POE_CHECK(!gate.ShouldRefuseCoSave());
    POE_CHECK(gate.ShouldRefuseCoSave());

    // 로드 시작 시 대기 중인 허용 토큰이 초기화된다.
    POE_CHECK(gate.AuthorizeSave("g1_out", true, false, true, SameSaveStem));
    gate.OnLoadStarted();
    POE_CHECK(gate.ShouldRefuseCoSave());

    // 정상 종료 대기 잠금은 슬롯을 비워 추가 저장을 막되 aborted는 세우지 않는다.
    // A7: LockForQuit(false) 뒤에도 SetAllowedSlot은 거부해야 한다.
    gate.LockForQuit(false);
    POE_CHECK(gate.IsActive() && !gate.IsAborted() && gate.AllowedSlot().empty());
    POE_CHECK(!gate.SetAllowedSlot("after_normal_lock"));
    POE_CHECK(gate.AllowedSlot().empty());
    POE_CHECK(!gate.AuthorizeSave("g1_out", true, false, true, SameSaveStem));
    POE_CHECK(gate.ShouldRefuseCoSave());

    // A7: allowPreserveSave=false인 세션에서는 Preserve 모드 저장 요청(allowsGameplay=false, isPreserveMode=true)을 거부한다.
    {
        TestSessionGate normalOnlyGate;
        normalOnlyGate.Begin("g1_out", /*allowPreserveSave=*/false);
        POE_CHECK(!normalOnlyGate.AuthorizeSave("g1_out", true, false, false, SameSaveStem, /*isPreserveMode=*/true));
        POE_CHECK(normalOnlyGate.ShouldRefuseCoSave());
    }

    // F7: 한 번 중단되면 sticky로 유지되며 Begin으로도, SetAllowedSlot으로도 풀리지 않는다.
    gate.LockForQuit(true);
    POE_CHECK(gate.IsActive() && gate.IsAborted() && gate.AllowedSlot().empty());
    gate.Begin("g1_retry");
    POE_CHECK(gate.IsAborted() && gate.AllowedSlot().empty());
    POE_CHECK(!gate.SetAllowedSlot("g1_retry_slot"));
    POE_CHECK(gate.AllowedSlot().empty());
    POE_CHECK(!gate.AuthorizeSave("g1_retry", true, false, true, SameSaveStem));
    POE_CHECK(gate.ShouldRefuseCoSave());

    // W8d(preserve): SetAllowedSlot은 활성·미중단 세션에서만 슬롯을 바꾸고, allowPreserveSave=true일 때만 보존 모드 저장을 허용한다.
    TestSessionGate preserveGate;
    POE_CHECK(!preserveGate.SetAllowedSlot("before_begin"));
    preserveGate.Begin("", /*allowPreserveSave=*/true);
    POE_CHECK(preserveGate.IsActive() && !preserveGate.IsAborted() && preserveGate.AllowedSlot().empty());
    POE_CHECK(!preserveGate.AuthorizeSave("a_out", true, false, false, SameSaveStem, true));
    POE_CHECK(preserveGate.SetAllowedSlot("a_out"));
    POE_CHECK(preserveGate.AllowedSlot() == "a_out");
    // Preserve 모드 허용(normal=false, preserve=true)은 1회 통과 후 소비.
    POE_CHECK(preserveGate.AuthorizeSave("A_OUT", true, false, false, SameSaveStem, true));
    POE_CHECK(!preserveGate.ShouldRefuseCoSave());
    POE_CHECK(preserveGate.ShouldRefuseCoSave());
    // 둘 다 false(예: CleanupPending 등)이면 여전히 거부.
    POE_CHECK(!preserveGate.AuthorizeSave("a_out", true, false, false, SameSaveStem, false));
    // 다음 입력 슬롯으로 교체 가능.
    POE_CHECK(preserveGate.SetAllowedSlot("b_out"));
    POE_CHECK(!preserveGate.AuthorizeSave("a_out", true, false, false, SameSaveStem, true));
    POE_CHECK(preserveGate.AuthorizeSave("b_out", true, false, false, SameSaveStem, true));
    POE_CHECK(!preserveGate.ShouldRefuseCoSave());
    // 중단 뒤에는 SetAllowedSlot도, preserve 저장도 거부한다.
    preserveGate.LockForQuit(true);
    POE_CHECK(!preserveGate.SetAllowedSlot("c_out"));
    POE_CHECK(preserveGate.AllowedSlot().empty());
    POE_CHECK(!preserveGate.AuthorizeSave("c_out", true, false, false, SameSaveStem, true));
    POE_CHECK(preserveGate.ShouldRefuseCoSave());
}

POE_TEST("Cleanup C3 independent expectation verifies Spark, Chain form, losses, and GiveStarterGems")
{
    using namespace SkyrimPoE::Core::GemForms;
    auto makeReport = [](bool enabled, std::int32_t sparkDelta) {
        StarterGrantReport r;
        r.generation = 1;
        r.configEnabled = enabled;
        r.entries.push_back({
            .contentId = "gem.Spark",
            .localFormId = 0x880,
            .status = enabled ? StarterGrantStatus::Granted : StarterGrantStatus::DisabledByConfig,
            .countDelta = sparkDelta,
        });
        return r;
    };

    // 정상: 소켓 Spark 1, 소켓/보조 Chain 1, GiveStarterGems=true -> Spark +2, oldChain +1, newChain +0, 손실 0.
    CleanupItemExpectationInput base{};
    base.plannedSlottedSpark = 1;
    base.plannedOldChain = 1;
    base.giveStarterGems = true;
    base.sparkPre = 3;
    base.sparkActual = 5;
    base.oldChainPre = 0;
    base.oldChainActual = 1;
    base.newChainPre = 0;
    base.newChainActual = 0;
    base.starterReport = makeReport(true, 1);
    const auto okRes = EvaluateCleanupItemExpectation(base);
    POE_CHECK(okRes.ok && okRes.expectedSparkDelta == 2 && okRes.expectedOldChainDelta == 1 && okRes.expectedNewChainDelta == 0);
    POE_CHECK((ExpectedGen1StarterIdsForConfig(true) == std::vector<std::string>{ "gem.Spark" }));
    POE_CHECK(ExpectedGen1StarterIdsForConfig(false).empty());

    // 소켓 Spark 수 N은 입력 요건이 아니고 기대 증분(N + 시작 1)에 들어간다.
    for (const std::size_t sparks : { std::size_t{ 0 }, std::size_t{ 2 } }) {
        auto other = base;
        other.plannedSlottedSpark = sparks;
        other.sparkActual = other.sparkPre + static_cast<std::int32_t>(sparks) + 1;
        const auto res = EvaluateCleanupItemExpectation(other);
        POE_CHECK(res.ok && res.inputRequirementMet && res.expectedSparkDelta == static_cast<std::int32_t>(sparks) + 1);
        auto wrong = other;
        wrong.sparkActual = wrong.sparkPre + 2 + static_cast<std::int32_t>(sparks);  // 하나 더 많음
        POE_CHECK(!EvaluateCleanupItemExpectation(wrong).ok);
    }

    // F6(a): 구판 Chain이 소켓/보조에 없으면(가방만 있으면 plannedOldChain=0) 입력 요건 실패.
    {
        auto noChainPlan = base;
        noChainPlan.plannedOldChain = 0;
        noChainPlan.oldChainActual = noChainPlan.oldChainPre;
        const auto res = EvaluateCleanupItemExpectation(noChainPlan);
        POE_CHECK(!res.ok && !res.inputRequirementMet);
    }

    // 구판 Chain이 신판 폼(0x8D2)으로 돌아오면 실패.
    {
        auto wrongForm = base;
        wrongForm.oldChainActual = wrongForm.oldChainPre;
        wrongForm.newChainActual = wrongForm.newChainPre + 1;
        const auto res = EvaluateCleanupItemExpectation(wrongForm);
        POE_CHECK(!res.ok && !res.oldChainCountOk && !res.newChainCountOk);
    }

    // F6(b): 구판 Spark 반환 손실 1과 시작 지급 +2가 겹쳐 합계가 +2로 보여도 실패해야 한다.
    {
        auto masked = base;
        masked.sparkLost = 1;
        masked.sparkActual = masked.sparkPre + 2;
        masked.starterReport = makeReport(true, 2);
        const auto res = EvaluateCleanupItemExpectation(masked);
        POE_CHECK(!res.ok && !res.zeroLossOk && !res.starterGrantOk);
    }

    // F6(c): GiveStarterGems=false면 Spark +1, 시작 지급 0(DisabledByConfig).
    {
        auto off = base;
        off.giveStarterGems = false;
        off.sparkActual = off.sparkPre + 1;
        off.starterReport = makeReport(false, 0);
        const auto res = EvaluateCleanupItemExpectation(off);
        POE_CHECK(res.ok && res.expectedSparkDelta == 1 && res.expectedStarterSparkDelta == 0);

        auto offTooMany = off;
        offTooMany.sparkActual = offTooMany.sparkPre + 2;
        POE_CHECK(!EvaluateCleanupItemExpectation(offTooMany).ok);
    }
}

POE_TEST("Decision 258: GiveStarterGems off then on grants only missing ready IDs without duplicates")
{
    using namespace SkyrimPoE::Core::GemForms;
    const std::array<StarterContentItem, 2> readyTwo{{
        { "gem.Spark", 0x880 },
        { "gem.Fireball", 0x87D },
    }};
    std::vector<std::string> grantedIds;
    std::vector<std::string> delivered;
    auto deliver = [&](const StarterContentItem& item) -> std::optional<std::int32_t> {
        delivered.emplace_back(item.contentId);
        return 1;
    };

    // 1. GiveStarterGems=false: 무지급·빈 STRT
    const auto offReport = ApplyStarterGrant(1, readyTwo, false, grantedIds, deliver);
    POE_CHECK(delivered.empty() && grantedIds.empty());
    POE_CHECK(!offReport.configEnabled && offReport.DisabledCount() == 2 && offReport.GrantedCount() == 0);

    // 2. GiveStarterGems=true로 전환: 아직 받지 않은 ready ID만 1회 지급
    const auto onReport = ApplyStarterGrant(1, readyTwo, true, grantedIds, deliver);
    POE_CHECK(onReport.configEnabled && onReport.GrantedCount() == 2 && onReport.DisabledCount() == 0);
    POE_CHECK((delivered == std::vector<std::string>{ "gem.Spark", "gem.Fireball" }));
    POE_CHECK((grantedIds == std::vector<std::string>{ "gem.Spark", "gem.Fireball" }));

    // 3. 같은 목록으로 재호출: 추가 지급 0
    delivered.clear();
    const auto repeatSame = ApplyStarterGrant(1, readyTwo, true, grantedIds, deliver);
    POE_CHECK(delivered.empty() && repeatSame.entries.empty() && repeatSame.GrantedCount() == 0);

    // 4. 이미 지급된 ID가 있는 상태에서 off -> on 전환: 기존 ID는 다시 주지 않고 누락된 ready ID만 지급
    std::vector<std::string> partialGranted{ "gem.Spark" };
    delivered.clear();
    const auto partialOff = ApplyStarterGrant(1, readyTwo, false, partialGranted, deliver);
    POE_CHECK(delivered.empty() && !partialOff.configEnabled && partialOff.DisabledCount() == 1 &&
        partialOff.GrantedCount() == 0);
    POE_CHECK((partialGranted == std::vector<std::string>{ "gem.Spark" }));

    const auto partialOn = ApplyStarterGrant(1, readyTwo, true, partialGranted, deliver);
    POE_CHECK(partialOn.configEnabled && partialOn.GrantedCount() == 1 && partialOn.DisabledCount() == 0);
    POE_CHECK((delivered == std::vector<std::string>{ "gem.Fireball" }));
    POE_CHECK((partialGranted == std::vector<std::string>{ "gem.Spark", "gem.Fireball" }));

    // 5. 같은 목록 + 새 ready ID 추가 후 다시 켬: 이미 받은 것은 다시 주지 않고 새 항목만 지급
    delivered.clear();
    const std::array<StarterContentItem, 3> readyThree{{
        { "gem.Spark", 0x880 },
        { "gem.Fireball", 0x87D },
        { "sup.Fork", 0x883 },
    }};
    const auto nextOn = ApplyStarterGrant(1, readyThree, true, grantedIds, deliver);
    POE_CHECK(nextOn.GrantedCount() == 1 && nextOn.FailedCount() == 0 && nextOn.DisabledCount() == 0);
    POE_CHECK((delivered == std::vector<std::string>{ "sup.Fork" }));
    POE_CHECK((grantedIds == std::vector<std::string>{ "gem.Spark", "gem.Fireball", "sup.Fork" }));
}

POE_TEST("ParseMarker validates newgame and preserve modes and still rejects gen1gate")
{
    // newgame: save 없이 out_save 필수, quit=true 필수, preserve_saves 거부.
    {
        std::istringstream ok("only=newgame\nout_save=g1_newgame.ess\nquit=true\n");
        const auto parsed = ParseMarker(ok);
        POE_CHECK(parsed && parsed->newGameOnly && !parsed->preserveOnly && parsed->UsesTestSession());
        POE_CHECK(parsed->baselineSave.empty() && parsed->EffectiveOutSave() == "g1_newgame" && parsed->quitWhenDone);
    }
    for (const auto* bad : {
        "only=newgame\nquit=true\n",                                        // out_save 누락
        "only=newgame\nsave=Hero\nout_save=g1_newgame\nquit=true\n",        // save 지정 금지
        "only=newgame\nout_save=g1_newgame\nquit=false\n",                  // quit=false 거부
        "only=newgame\nout_save=g1_newgame\npreserve_saves=a\nquit=true\n", // preserve_saves 금지
        "only=newgame\nout_save=sub/g1_newgame\nquit=true\n",               // 경로 구분자 거부
        "only=newgame\nonly=save\nout_save=g1_newgame\nquit=true\n",        // 모드 중복 거부
    }) {
        std::istringstream in(bad);
        POE_CHECK(!ParseMarker(in));
    }

    // preserve: preserve_saves 1..16개 필수, save/out_save 금지, quit=true 필수, 중복·경로·_out 충돌 거부.
    {
        std::istringstream ok("only=preserve\npreserve_saves=nogenr.ess, genr_corrupt , strt_dup.ESS\nquit=true\n");
        const auto parsed = ParseMarker(ok);
        POE_CHECK(parsed && parsed->preserveOnly && !parsed->newGameOnly && parsed->UsesTestSession());
        POE_CHECK(parsed->baselineSave.empty() && parsed->outSave.empty() && parsed->quitWhenDone);
        POE_CHECK((parsed->preserveSaves == std::vector<std::string>{ "nogenr", "genr_corrupt", "strt_dup" }));
    }
    {
        // 정확히 16개는 허용, 17개는 거부.
        std::string sixteen = "only=preserve\npreserve_saves=";
        for (int i = 1; i <= 16; ++i) {
            if (i > 1) sixteen += ',';
            sixteen += "s" + std::to_string(i);
        }
        sixteen += "\nquit=true\n";
        std::istringstream ok16(sixteen);
        const auto parsed16 = ParseMarker(ok16);
        POE_CHECK(parsed16 && parsed16->preserveSaves.size() == 16);

        std::istringstream bad17("only=preserve\npreserve_saves=s1,s2,s3,s4,s5,s6,s7,s8,s9,s10,s11,s12,s13,s14,s15,s16,s17\nquit=true\n");
        POE_CHECK(!ParseMarker(bad17));
    }
    for (const auto* bad : {
        "only=preserve\nquit=true\n",                                      // preserve_saves 누락
        "only=preserve\npreserve_saves=\nquit=true\n",                     // 빈 목록
        "only=preserve\npreserve_saves=a,,b\nquit=true\n",                 // 빈 원소
        "only=preserve\npreserve_saves=a,A.ess\nquit=true\n",              // 대소문자 무시 중복
        "only=preserve\npreserve_saves=a,a_out\nquit=true\n",              // 출력 <stem>_out과 다른 입력 충돌
        "only=preserve\npreserve_saves=sub/a\nquit=true\n",                // 경로 구분자 거부
        "only=preserve\npreserve_saves=a\nsave=Hero\nquit=true\n",         // save 지정 금지
        "only=preserve\npreserve_saves=a\nout_save=out\nquit=true\n",      // out_save 지정 금지
        "only=preserve\npreserve_saves=a\nquit=false\n",                   // quit=false 거부
        "only=save\nsave=Hero\npreserve_saves=a\nquit=true\n",             // 다른 모드에서 preserve_saves 금지
    }) {
        std::istringstream in(bad);
        POE_CHECK(!ParseMarker(in));
    }

    // A4: cleanup·save·es·react·gen1gate는 save= 필수(입력 없음 허용은 newgame에서만).
    for (const auto* mode : { "cleanup", "save", "es", "react", "gen1gate" }) {
        std::istringstream missingSave(std::string("only=") + mode + "\nquit=true\n");
        POE_CHECK(!ParseMarker(missingSave));
    }

    // B1: gen1gate는 save=·quit=true 필수, out_save= 선택(패키지 세대별 검사는 DLL), preserve_saves 거부.
    {
        std::istringstream g1("only=gen1gate\nsave=g1_cleanup_genr\nout_save=g1_gate\nquit=true\n");
        const auto parsed = ParseMarker(g1);
        POE_CHECK(parsed && parsed->gen1GateOnly && parsed->UsesTestSession() &&
            parsed->baselineSave == "g1_cleanup_genr" && parsed->outSave == "g1_gate");

        std::istringstream g0("only=gen1gate\nsave=poe1_base\nquit=true\n");
        const auto parsed0 = ParseMarker(g0);
        POE_CHECK(parsed0 && parsed0->gen1GateOnly && parsed0->UsesTestSession() &&
            parsed0->baselineSave == "poe1_base" && parsed0->outSave.empty());
    }
    for (const auto* bad : {
        "only=gen1gate\nsave=Hero\nquit=false\n",
        "only=gen1gate\nsave=Hero\nout_save=hero.ess\nquit=true\n",
        "only=gen1gate\nsave=Hero\npreserve_saves=a\nquit=true\n",
        "only=gen1gate\nonly=save\nsave=Hero\nquit=true\n",
    }) {
        std::istringstream in(bad);
        POE_CHECK(!ParseMarker(in));
    }

    // A11: 콜론(:) 및 Windows 금지 문자(<>"|?*)는 save·temp_save·out_save·preserve_saves 모두에서 거부.
    for (const auto* bad : {
        "only=save\nsave=C:Hero\nquit=true\n",
        "only=save\nsave=Hero:ads\nquit=true\n",
        "only=save\nsave=Hero\ntemp_save=bad*name\nquit=true\n",
        "only=newgame\nout_save=bad?out\nquit=true\n",
        "only=newgame\nout_save=bad<out>\nquit=true\n",
        "only=preserve\npreserve_saves=ok,bad:stem\nquit=true\n",
        "only=preserve\npreserve_saves=bad|stem\nquit=true\n",
        "only=preserve\npreserve_saves=bad\"stem\nquit=true\n",
    }) {
        std::istringstream in(bad);
        POE_CHECK(!ParseMarker(in));
    }
}
