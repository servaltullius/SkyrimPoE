#include "core/Migration.h"
#include "core/SaveRecordFormat.h"
#include "test_harness.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>

using namespace SkyrimPoE::Core;
using namespace SkyrimPoE::Core::SaveRecord;

namespace
{
    const SaveGeneration kPack{ "poe1", 0, "3.30", "1234567890abcdef", "0.1.0" };
    const SaveGeneration kGen1Pack{ "poe2", 1, "0.5", "1234567890abcdef", "0.1.0" };

    struct FakeOutput
    {
        std::vector<RawSaveRecord> records;
        std::vector<std::uint32_t> attempted;
        std::optional<std::uint32_t> failOpen;
        std::optional<std::uint32_t> failWrite;

        bool OpenRecord(std::uint32_t type, std::uint32_t version)
        {
            attempted.push_back(type);
            if (failOpen == type) {
                return false;
            }
            records.push_back({ type, version, {} });
            return true;
        }

        bool WriteRecordData(const void* data, std::uint32_t length)
        {
            auto& record = records.back();
            if (failWrite == record.type) {
                return false;
            }
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            record.bytes.insert(record.bytes.end(), bytes, bytes + length);
            return true;
        }
    };

    struct FakeInput
    {
        const std::vector<RawSaveRecord>& records;
        std::optional<std::size_t> shortRecord = std::nullopt;
        std::optional<std::size_t> throwRecord = std::nullopt;

        SaveRecordBatch Collect() const
        {
            std::size_t nextIndex = 0;
            std::size_t offset = 0;
            const auto next = [&](std::uint32_t& type, std::uint32_t& version, std::uint32_t& length) {
                if (nextIndex == records.size()) {
                    return false;
                }
                const auto& record = records[nextIndex++];
                type = record.type;
                version = record.version;
                length = static_cast<std::uint32_t>(record.bytes.size());
                offset = 0;
                return true;
            };
            const auto read = [&](void* data, std::uint32_t size) {
                const auto index = nextIndex - 1;
                if (throwRecord == index) {
                    throw std::runtime_error("simulated read exception");
                }
                const auto readSize = shortRecord == index && offset + size == records[index].bytes.size() ?
                    size - 1 : size;
                std::memcpy(data, records[index].bytes.data() + offset, readSize);
                offset += readSize;
                return readSize;
            };
            return CollectSaveRecords(next, read);
        }
    };

    Snapshot MakeSnapshot(std::uint32_t exileLevel, std::uint32_t generation = 0)
    {
        Snapshot save;
        save.state.exileLevel = exileLevel;
        save.state.exileXp = 12345;
        save.state.starterGemsGiven = true;
        save.state.appliedHealthDelta = 2.5;
        auto& slot = save.state.slots[1];
        slot.occupied = true;
        slot.gem = GemInstance{ .instanceId = 11, .gemId = generation == 0 ? "IceNova" : "gem.IceNova" };
        slot.gem.socketedGems.push_back(GemInstance{
            .instanceId = 12, .gemId = generation == 0 ? "SupportAddedCold" : "sup.ConcentratedArea"
        });
        save.state.tree.generation = generation;
        save.state.tree.allocatedNodes = { 100, 351 };
        save.state.tree.masterySelections = { { 41, 12 }, { 3, 2 } };
        save.state.atlas.completedTierMask = 0x32;
        save.state.virtualRing2BaseFormId = 0x01001234;
        save.state.activeAuraMask = 2;
        save.tracked.push_back({ { 0x14, 9 }, false, slot.gem });
        save.trackedItems.push_back({ { 0x14, 7 }, false, PoEItemRecord{} });
        save.savedWorldDrops.push_back({ 0xFF001001, 0xFF001000, 0xFF001002, 3 });
        save.savedSpeedMods.push_back({ 0x123456, -5.0f });
        return save;
    }

    SaveRecordBatch EncodeToMemory(const Snapshot& save, const SaveGeneration& pack = kPack)
    {
        SaveRecordBatch batch;
        batch.complete = WriteSnapshot(save, pack, [&](RawSaveRecord&& record) {
            batch.records.push_back(std::move(record));
            return true;
        });
        return batch;
    }

    void Replay(const SaveRecordBatch& batch, FakeOutput& output)
    {
        POE_CHECK(ReplaySaveRecords(batch, [&](const RawSaveRecord& record) { return WriteSaveRecord(output, record); }));
    }

    RawSaveRecord& FindRecord(SaveRecordBatch& batch, std::uint32_t type)
    {
        const auto record = std::ranges::find(batch.records, type, &RawSaveRecord::type);
        POE_CHECK(record != batch.records.end());
        return *record;
    }
}

POE_TEST("Generation-zero legacy records collect, restore CORE and write the same bytes")
{
    const auto original = MakeSnapshot(27);
    FakeOutput direct;
    POE_CHECK(WriteSnapshot(original, kPack, [&](const RawSaveRecord& record) { return WriteSaveRecord(direct, record); }));
    const auto snapshot = EncodeToMemory(original);
    POE_CHECK(snapshot.complete);
    POE_CHECK(snapshot.records == direct.records);
    POE_CHECK(direct.records.back().type == kRecordGeneration);
    const auto generation = direct.records.back();
    direct.records.pop_back();

    const auto collected = FakeInput{ direct.records }.Collect();
    POE_CHECK(collected.complete);
    POE_CHECK(DecideSave(collected, false, kPack).disposition == SaveDisposition::Normal);
    POE_CHECK(collected.records == direct.records);

    POE_CHECK(collected.records.front().type == kRecordCore);
    SkyrimPoE::Runtime::PersistentState restoredCore;
    std::span<const std::uint8_t> core = collected.records.front().bytes;
    POE_CHECK(ReadCoreV2(core, restoredCore));
    POE_CHECK(core.empty());
    POE_CHECK(restoredCore.exileLevel == 27);
    POE_CHECK(restoredCore.slots[1].gem.socketedGems.front().instanceId == 12);
    auto restored = original;
    restored.state.exileLevel = restoredCore.exileLevel;
    restored.state.exileXp = restoredCore.exileXp;
    restored.state.unallocatedPassivePoints = restoredCore.unallocatedPassivePoints;
    restored.state.className = std::move(restoredCore.className);
    restored.state.starterGemsGiven = restoredCore.starterGemsGiven;
    restored.state.nextGemInstanceId = restoredCore.nextGemInstanceId;
    restored.state.appliedHealthDelta = restoredCore.appliedHealthDelta;
    restored.state.appliedMagickaDelta = restoredCore.appliedMagickaDelta;
    restored.state.slots = std::move(restoredCore.slots);
    const auto reencoded = EncodeToMemory(restored);
    POE_CHECK(reencoded.records == snapshot.records);
    POE_CHECK(reencoded.records.back() == generation);

    FakeOutput replayed;
    Replay(collected, replayed);
    POE_CHECK(replayed.records == direct.records);
}

POE_TEST("PRAT round trips player rate deltas in both generations without changing the CORE payload")
{
    const auto legacy = EncodeToMemory(MakeSnapshot(27));
    for (const auto& pack : { kPack, kGen1Pack }) {
        auto expected = MakeSnapshot(27, pack.contentGeneration);
        const auto beforeRates = EncodeToMemory(expected, pack);
        expected.state.appliedSpeedMultDelta = 37.5;
        expected.state.appliedMagickaRateMultDelta = -58.75;
        SaveRecordBatch written;
        written.complete = WriteSnapshot(expected, pack, [&](RawSaveRecord&& record) {
            written.records.push_back(std::move(record));
            return true;
        });
        POE_CHECK(written.complete);
        const auto& core = written.records.front();
        POE_CHECK(core.type == kRecordCore && core.version == (pack.contentGeneration == 0 ? 2u : 3u));
        POE_CHECK(core.bytes == beforeRates.records.front().bytes);
        POE_CHECK(DecideSave(written, false, pack).disposition == SaveDisposition::Normal);

        SkyrimPoE::Runtime::PersistentState restored;
        POE_CHECK(ReadCoreRecord(core, restored));
        POE_CHECK(restored.slots[1].gem.socketedGems.front().instanceId == 12);
        const auto deltas = ReadPlayerRates(written);
        POE_CHECK(deltas.has_value());
        POE_CHECK_NEAR(deltas->speedMult, 37.5, 1e-9);
        POE_CHECK_NEAR(deltas->magickaRateMult, -58.75, 1e-9);
        const auto sync = ReconcilePlayerRates(137.5, 58.75, *deltas, 1.375, 0.5);
        POE_CHECK(sync.changeSpeedMult == 0.0f && sync.changeMagickaRateMult == 0.0f);
        POE_CHECK(sync.applied == *deltas);

        // A raw pre-load snapshot is replayed byte-for-byte even after reverting the live state.
        SaveRecordTransition transition;
        transition.BeginLoad(written);
        transition.Revert();
        transition.current = legacy;
        POE_CHECK(ReadPlayerRates(transition.RecordsForSave()) == deltas);
        transition.FinishLoad(false);
        FakeOutput replayed;
        Replay(transition.RecordsForSave(), replayed);
        POE_CHECK(replayed.records == written.records);
        transition.Revert();
        POE_CHECK(ReadPlayerRates(transition.RecordsForSave()) == PlayerRateDeltas{});
    }
}

POE_TEST("Damaged PRAT is preserved in either generation and CORE v3 still requires GENR")
{
    const SaveGeneration pack{ "poe2", 1, "0.5", "1234567890abcdef", "0.1.0" };
    auto save = MakeSnapshot(27, pack.contentGeneration);
    save.state.appliedMagickaRateMultDelta = -50.0;
    SaveRecordBatch written;
    written.complete = WriteSnapshot(save, pack, [&](RawSaveRecord&& record) {
        written.records.push_back(std::move(record));
        return true;
    });
    POE_CHECK(written.complete);
    auto gen0 = EncodeToMemory(MakeSnapshot(27));
    for (const auto& original : { written, gen0 }) {
        for (int problem = 0; problem < 6; ++problem) {
            auto bad = original;
            auto record = std::ranges::find(bad.records, kRecordPlayerRates, &RawSaveRecord::type);
            POE_CHECK(record != bad.records.end());
            if (problem == 0) record->bytes.pop_back();
            if (problem == 1) record->bytes.push_back(0);
            if (problem == 2) ++record->version;
            if (problem == 3) bad.records.push_back(*record);
            if (problem == 4 || problem == 5) {
                const double value = problem == 4 ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::max();
                std::memcpy(record->bytes.data(), &value, sizeof(value));
            }
            POE_CHECK(!ReadPlayerRates(bad));
            POE_CHECK(DecideSave(bad, false, pack).disposition == SaveDisposition::Preserve);
            POE_CHECK(DecideSave(bad, false, kPack).disposition == SaveDisposition::Preserve);
            FakeOutput replayed;
            Replay(bad, replayed);
            POE_CHECK(replayed.records == bad.records);
        }
    }
    auto bad = written;
    bad.records.front().bytes.pop_back();
    POE_CHECK(DecideSave(bad, false, pack).disposition == SaveDisposition::Preserve);

    bad = written;
    std::erase_if(bad.records, [](const auto& record) { return record.type == kRecordCore; });
    POE_CHECK(DecideSave(bad, false, pack).disposition == SaveDisposition::Preserve);

    bad = written;
    std::erase_if(bad.records, [](const auto& record) { return record.type == kRecordGeneration; });
    POE_CHECK(DecideSave(bad, false, pack).disposition == SaveDisposition::Preserve);
    POE_CHECK(DecideSave(bad, false, kPack).disposition == SaveDisposition::Preserve);

    // The unpublished extended v3 must not run with an untracked delta embedded in CORE.
    bad = written;
    std::erase_if(bad.records, [](const auto& record) { return record.type == kRecordPlayerRates; });
    bad.records.front().bytes.resize(bad.records.front().bytes.size() + 2 * sizeof(double));
    POE_CHECK(DecideSave(bad, false, pack).disposition == SaveDisposition::Preserve);
}

POE_TEST("Normal generation-one loads require exactly one PRAT, CORE v3 and TREE v2")
{
    auto save = MakeSnapshot(27, kGen1Pack.contentGeneration);
    const auto valid = EncodeToMemory(save, kGen1Pack);
    POE_CHECK(valid.complete && DecideSave(valid, false, kGen1Pack).disposition == SaveDisposition::Normal);
    for (const auto type : { kRecordPlayerRates, kRecordCore, kRecordTree }) {
        auto missing = valid;
        POE_CHECK(std::erase_if(missing.records, [type](const auto& record) { return record.type == type; }) == 1);
        POE_CHECK(DecideSave(missing, false, kGen1Pack).disposition == SaveDisposition::Preserve);

        auto duplicate = valid;
        duplicate.records.push_back(FindRecord(duplicate, type));
        POE_CHECK(DecideSave(duplicate, false, kGen1Pack).disposition == SaveDisposition::Preserve);
    }

    // TREE v1 has two class bytes followed by an empty ascendancy, zero list counts and three counters.
    const RawSaveRecord legacyTree{ kRecordTree, kLegacyTreeVersion,
        std::vector<std::uint8_t>(2 + 6 * sizeof(std::uint32_t)) };
    PassiveTreeState restored;
    POE_CHECK(ReadTreeRecord(legacyTree, restored) && restored.generation == 0);
    auto wrongVersion = valid;
    FindRecord(wrongVersion, kRecordTree) = legacyTree;
    POE_CHECK(DecideSave(wrongVersion, false, kGen1Pack).disposition == SaveDisposition::Preserve);
    auto legacy = EncodeToMemory(MakeSnapshot(27));
    FindRecord(legacy, kRecordTree) = legacyTree;
    POE_CHECK(DecideSave(legacy, false, kPack).disposition == SaveDisposition::Normal);
}

POE_TEST("Generation-one GENR rejects a fully readable CORE v2")
{
    auto save = MakeSnapshot(27, kGen1Pack.contentGeneration);
    auto batch = EncodeToMemory(save, kGen1Pack);
    auto gen0 = EncodeToMemory(MakeSnapshot(27));
    FindRecord(batch, kRecordCore) = FindRecord(gen0, kRecordCore);
    SkyrimPoE::Runtime::PersistentState restored;
    POE_CHECK(ReadCoreRecord(FindRecord(batch, kRecordCore), restored));
    POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Preserve);
}

POE_TEST("CORE v3 rejects nonfinite health and magicka deltas while v2 still reads them as zero")
{
    for (const auto invalid : { std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity() }) {
        for (const auto delta : { &SkyrimPoE::Runtime::PersistentState::appliedHealthDelta,
                 &SkyrimPoE::Runtime::PersistentState::appliedMagickaDelta }) {
            auto save = MakeSnapshot(27, kGen1Pack.contentGeneration);
            save.state.*delta = invalid;
            auto batch = EncodeToMemory(save, kGen1Pack);
            auto& core = FindRecord(batch, kRecordCore);
            SkyrimPoE::Runtime::PersistentState restored;
            restored.exileLevel = 99;
            POE_CHECK(!ReadCoreRecord(core, restored) && restored.exileLevel == 99);
            POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Preserve);
            core.version = kLegacyCoreVersion;
            POE_CHECK(ReadCoreRecord(core, restored) && restored.*delta == 0.0);
        }
    }
}

POE_TEST("Truncated TREE is preserved even if its generation was fully read")
{
    for (const auto& pack : { kPack, kGen1Pack }) {
        auto save = MakeSnapshot(27, pack.contentGeneration);
        auto batch = EncodeToMemory(save, pack);
        if (pack.contentGeneration == 0) {
            auto duplicate = batch;
            duplicate.records.push_back(FindRecord(duplicate, kRecordTree));
            POE_CHECK(DecideSave(duplicate, false, kPack).disposition == SaveDisposition::Preserve);
            POE_CHECK(DecideSave(duplicate, false, kGen1Pack).disposition == SaveDisposition::Preserve);
        }
        FindRecord(batch, kRecordTree).bytes.resize(sizeof(std::uint32_t));
        POE_CHECK(DecideSave(batch, false, pack).disposition == SaveDisposition::Preserve);
        if (pack.contentGeneration == 0) {
            POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Preserve);
        }
    }
}

POE_TEST("Prior TREE v2 loads but a duplicate attribute selection key preserves the save")
{
    auto save = MakeSnapshot(27, kGen1Pack.contentGeneration);
    auto batch = EncodeToMemory(save, kGen1Pack);
    auto& tree = FindRecord(batch, kRecordTree);
    const auto setTree = [&](const auto& words) {
        tree.bytes.resize(sizeof(words));
        std::memcpy(tree.bytes.data(), words.data(), sizeof(words));
    };
    // Generation, class length, ascendancy length, node count, node, choice count,
    // attribute pairs, mastery count, boss kills, dragon kills, milestones.
    setTree(std::array{ 1u, 0u, 0u, 1u, 351u, 1u, 351u, 1u, 0u, 3u, 4u, 0u });
    PassiveTreeState restored;
    POE_CHECK(ReadTreeRecord(tree, restored) && restored.attributeSelections.at(351) == 1);
    POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Normal);
    setTree(std::array{ 1u, 0u, 0u, 1u, 351u, 2u, 351u, 1u, 351u, 2u, 0u, 3u, 4u, 0u });
    POE_CHECK(!ReadTreeRecord(tree, restored) && restored.attributeSelections.at(351) == 1);
    POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Preserve);
}

POE_TEST("Saving a selected class without an ID warns and keeps TREE; loading that invalid flag preserves")
{
    auto save = MakeSnapshot(27, kGen1Pack.contentGeneration);
    save.state.tree.classSelected = true;
    save.state.tree.bossKillCount = 7;
    SaveRecordBatch batch;
    std::vector<std::string> warnings;
    POE_CHECK(WriteSnapshot(save, kGen1Pack, [&](RawSaveRecord&& record) {
        batch.records.push_back(std::move(record));
        return true;
    }, [&](std::string_view warning) { warnings.emplace_back(warning); }));
    POE_CHECK(warnings.size() == 1 && warnings.front().find("TREE") != std::string::npos);
    POE_CHECK(batch.records.back().type == kRecordGeneration);
    auto& tree = FindRecord(batch, kRecordTree);
    PassiveTreeState restored;
    POE_CHECK(ReadTreeRecord(tree, restored) && !restored.classSelected && restored.classContentId.empty());
    POE_CHECK(restored.bossKillCount == 7);
    POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Normal);

    constexpr std::size_t selectedOffset = 2 * sizeof(std::uint32_t);  // generation and class field marker
    tree.bytes.at(selectedOffset) = 1;
    POE_CHECK(!ReadTreeRecord(tree, restored));
    POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Preserve);
}

POE_TEST("A PRAT-only generation-zero save supplies both deltas and is not a first install")
{
    auto save = MakeSnapshot(27);
    save.state.appliedSpeedMultDelta = 20.0;
    save.state.appliedMagickaRateMultDelta = -5.0;
    auto batch = EncodeToMemory(save);
    const SaveRecordBatch onlyRates{ { FindRecord(batch, kRecordPlayerRates) } };
    POE_CHECK(DecideSave(onlyRates, false, kPack).disposition == SaveDisposition::Normal);
    POE_CHECK((ReadPlayerRates(onlyRates) == PlayerRateDeltas{ 20.0, -5.0 }));
    POE_CHECK(DecideSave(onlyRates, false, kGen1Pack).disposition == SaveDisposition::Preserve);
}

POE_TEST("Player rate reconciliation converges on a new game, an old save and a changed build after reload")
{
    auto old = EncodeToMemory(MakeSnapshot(27));
    std::erase_if(old.records, [](const auto& record) { return record.type == kRecordPlayerRates; });
    POE_CHECK(DecideSave(old, false, kPack).disposition == SaveDisposition::Normal);
    for (const auto& original : { SaveRecordBatch{}, old }) {
        const auto deltas = ReadPlayerRates(original);
        POE_CHECK(deltas == PlayerRateDeltas{});
        const auto first = ReconcilePlayerRates(120.0, 150.0, *deltas, 1.25, 0.5);
        POE_CHECK_NEAR(first.changeSpeedMult, 30.0, 1e-6);
        POE_CHECK_NEAR(first.changeMagickaRateMult, -75.0, 1e-6);

        // Change the factors with the first deltas still active, then save and load those actor values.
        const auto changed = ReconcilePlayerRates(150.0, 75.0, first.applied, 1.5, 0.8);
        POE_CHECK_NEAR(changed.changeSpeedMult, 30.0, 1e-6);
        POE_CHECK_NEAR(changed.changeMagickaRateMult, 45.0, 1e-6);
        auto save = MakeSnapshot(27);
        save.state.appliedSpeedMultDelta = changed.applied.speedMult;
        save.state.appliedMagickaRateMultDelta = changed.applied.magickaRateMult;
        const auto fromSave = ReadPlayerRates(EncodeToMemory(save));
        POE_CHECK(fromSave == changed.applied);
        const auto loaded = ReconcilePlayerRates(180.0, 120.0, *fromSave, 1.5, 0.8);
        POE_CHECK(loaded.changeSpeedMult == 0.0f && loaded.changeMagickaRateMult == 0.0f);
        const auto reverted = ReconcilePlayerRates(180.0, 120.0, loaded.applied, 1.0, 1.0);
        POE_CHECK_NEAR(180.0 + reverted.changeSpeedMult, 120.0, 1e-6);
        POE_CHECK_NEAR(120.0 + reverted.changeMagickaRateMult, 150.0, 1e-6);
        POE_CHECK(reverted.applied == PlayerRateDeltas{});
    }
}

POE_TEST("Player rate deltas retain changes from other effects and can be removed exactly")
{
    const auto initialSpeed = ComputePlayerRateDelta(120.0, 0.0, 1.2);
    POE_CHECK_NEAR(initialSpeed, 24.0, 1e-6);
    const auto updatedSpeed = ComputePlayerRateDelta(114.0, initialSpeed, 1.2);
    POE_CHECK_NEAR(updatedSpeed, -6.0, 1e-6);
    POE_CHECK_NEAR(ComputePlayerRateDelta(108.0, initialSpeed + updatedSpeed, 1.0), -18.0, 1e-6);

    const auto initialRate = ComputePlayerRateDelta(150.0, 0.0, 0.5);
    POE_CHECK_NEAR(initialRate, -75.0, 1e-6);
    const auto externalRateChange = ComputePlayerRateDelta(95.0, initialRate, 0.5);
    POE_CHECK_NEAR(externalRateChange, -10.0, 1e-6);
    POE_CHECK_NEAR(ComputePlayerRateDelta(85.0, initialRate + externalRateChange, 1.0), 85.0, 1e-6);
}

POE_TEST("Short reads discard the whole record and preserve complete siblings, including unknown and empty records")
{
    const std::vector<RawSaveRecord> records{
        { kRecordCore, kCoreVersion, { 1, 2, 3 } },
        { kRecordGeneration, kGenerationRecordVersion, std::vector<std::uint8_t>(70000, 4) },
        { 0x76543210, 42, {} },
        { 0x76543210, 42, { 10, 255, 0 } },
    };
    const FakeInput reader{ records, 1 };
    const auto collected = reader.Collect();
    POE_CHECK(!collected.complete);
    POE_CHECK(DecideSave(collected, false, kPack).disposition == SaveDisposition::Preserve);
    FakeOutput output;
    Replay(collected, output);
    const std::vector expected{ records[0], records[2], records[3] };
    POE_CHECK(output.records == expected);
}

POE_TEST("Collection exceptions retain only records finished before the exception")
{
    const std::vector<RawSaveRecord> records{ { 31, 7, { 5 } }, { 17, 3, { 1, 2 } } };
    const auto batch = FakeInput{ records, std::nullopt, 1 }.Collect();
    POE_CHECK(!batch.complete);
    POE_CHECK(batch.records == std::vector{ records.front() });
}

POE_TEST("Each read chunk starts zeroed even when an EOF is concealed by the interface")
{
    bool nextCalled = false;
    const auto batch = CollectSaveRecords(
        [&](auto& type, auto& version, auto& length) {
            type = 7;
            version = 1;
            length = 64 * 1024 + 1;
            return !std::exchange(nextCalled, true);
        },
        [](void* data, std::uint32_t size) {
            if (size > 1) {
                std::memset(data, 0xA5, size);
            }
            return size;
        });
    POE_CHECK(batch.complete);
    POE_CHECK(batch.records.front().bytes.front() == 0xA5);
    POE_CHECK(batch.records.front().bytes.back() == 0);
}

POE_TEST("Collection enforces the per-record and total co-save size limits before reading")
{
    constexpr std::uint32_t lengths[]{
        kMaxSaveRecordBytes + 1, kMaxSaveRecordBytes, kMaxSaveRecordBytes, kMaxSaveRecordBytes, kMaxSaveRecordBytes
    };
    std::uint32_t record = 0;
    std::uint32_t bytesRead = 0;
    const auto batch = CollectSaveRecords(
        [&](auto& type, auto& version, auto& length) {
            if (record == std::size(lengths)) {
                return false;
            }
            length = lengths[record++];
            type = record;
            version = 7;
            return true;
        },
        [&](void*, std::uint32_t size) {
            POE_CHECK(record >= 2 && record <= 4);
            bytesRead += size;
            return size;
        });
    POE_CHECK(!batch.complete);
    POE_CHECK(batch.records.size() == 3);
    POE_CHECK(bytesRead == 3 * kMaxSaveRecordBytes);
    POE_CHECK(DecideSave(batch, false, kPack).disposition == SaveDisposition::Preserve);
}

POE_TEST("Failed load restores A's pre-load snapshot after revert and collection of B")
{
    const auto saveA = EncodeToMemory(MakeSnapshot(27));
    const auto saveB = EncodeToMemory(MakeSnapshot(70));
    for (const bool complete : { true, false }) {
        SaveRecordTransition transition;
        transition.current = saveA;
        transition.current.complete = complete;
        transition.BeginLoad(transition.current);
        transition.Revert();
        transition.current = FakeInput{ saveB.records }.Collect();
        transition.receivedLoad = true;
        POE_CHECK(transition.RecordsForSave().records == saveA.records);
        transition.FinishLoad(false);
        FakeOutput output;
        Replay(transition.RecordsForSave(), output);
        POE_CHECK(output.records == saveA.records);
        POE_CHECK(transition.current.complete == complete);
    }
}

POE_TEST("Successful load discards the snapshot and a later unpaired revert clears the loaded records")
{
    SaveRecordTransition transition;
    transition.BeginLoad(EncodeToMemory(MakeSnapshot(27)));
    transition.Revert();
    const auto saveB = EncodeToMemory(MakeSnapshot(70));
    transition.current = FakeInput{ saveB.records }.Collect();
    transition.receivedLoad = true;
    transition.FinishLoad(true);
    POE_CHECK(transition.RecordsForSave().records == saveB.records);

    transition.Revert();
    POE_CHECK(!transition.IsLoading());
    POE_CHECK(!transition.receivedLoad);
    FakeOutput output;
    Replay(transition.RecordsForSave(), output);
    POE_CHECK(output.records.empty());
}

POE_TEST("Normal write omits GENR after a failed record open or data write")
{
    const auto snapshot = MakeSnapshot(27);
    for (const bool failOpen : { true, false }) {
        FakeOutput output;
        (failOpen ? output.failOpen : output.failWrite) = kRecordItems;
        POE_CHECK(!WriteSnapshot(snapshot, kPack, [&](const RawSaveRecord& record) { return WriteSaveRecord(output, record); }));
        POE_CHECK(std::ranges::find(output.attempted, kRecordMaps) != output.attempted.end());
        POE_CHECK(std::ranges::find(output.attempted, kRecordGeneration) == output.attempted.end());
    }
}

POE_TEST("Replay returns failure and still attempts each later complete record")
{
    const SaveRecordBatch batch{ { { 3, 1, { 5 } }, { 4, 42, { 6, 0 } } }, false };
    FakeOutput output;
    output.failWrite = 3;
    POE_CHECK(!ReplaySaveRecords(batch, [&](const RawSaveRecord& record) { return WriteSaveRecord(output, record); }));
    POE_CHECK(output.records.back() == batch.records.back());
}

namespace
{
    RawSaveRecord MakeRawStarterRecord(std::uint32_t version, std::uint32_t count,
        const std::vector<std::string>& ids, std::vector<std::uint8_t> trailing = {})
    {
        RawSaveRecord record{ kRecordStarter, version, {} };
        const auto append = [&](const void* data, std::size_t size) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            record.bytes.insert(record.bytes.end(), bytes, bytes + size);
        };
        append(&count, sizeof(count));
        for (const auto& id : ids) {
            const auto len = static_cast<std::uint32_t>(id.size());
            append(&len, sizeof(len));
            if (len != 0) {
                append(id.data(), len);
            }
        }
        record.bytes.insert(record.bytes.end(), trailing.begin(), trailing.end());
        return record;
    }
}

POE_TEST("STRT v1 round trips empty lists, multiple IDs in order, and unknown valid content IDs")
{
    POE_CHECK(IsValidStarterContentId("gem.Spark"));
    POE_CHECK(IsValidStarterContentId("sup.Chain1"));
    POE_CHECK(IsValidStarterContentId("future.Unknown_Reward.v2"));
    POE_CHECK(IsValidStarterContentId(std::string("gem.") + std::string(kMaxStarterContentIdBytes - 4, 'a')));
    for (const std::string invalid : {
             std::string(""), std::string("Spark"), std::string(".Spark"), std::string("gem."),
             std::string("Gem.Spark"), std::string("gem1.Spark"), std::string("gem_1.Spark"),
             std::string("gem.Spark!"), std::string("gem.Spark "), std::string("gem.\xEA\xB0\x80"),
             std::string("gem.") + std::string(kMaxStarterContentIdBytes - 3, 'a') }) {
        POE_CHECK(!IsValidStarterContentId(invalid));
    }

    // 빈 목록 왕복
    {
        auto save = MakeSnapshot(27, kGen1Pack.contentGeneration);
        save.state.starterGrantedIds.clear();
        auto batch = EncodeToMemory(save, kGen1Pack);
        POE_CHECK(batch.complete);
        POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Normal);
        const auto& strt = FindRecord(batch, kRecordStarter);
        POE_CHECK(strt.version == kStarterVersion && strt.bytes.size() == sizeof(std::uint32_t));
        std::vector<std::string> restored{ "stale.Entry" };
        POE_CHECK(ReadStarterRecord(strt, restored) && restored.empty());
    }

    // 여러 ID 및 현재 빌드가 모르는 유효 ID 보존·순서 유지
    {
        auto save = MakeSnapshot(27, kGen1Pack.contentGeneration);
        save.state.starterGrantedIds = { "gem.Spark", "future.Unknown_Gem.v2", "sup.Fork", "currency.ExaltedOrb" };
        auto batch = EncodeToMemory(save, kGen1Pack);
        POE_CHECK(batch.complete);
        POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Normal);
        const auto& strt = FindRecord(batch, kRecordStarter);
        std::vector<std::string> restored;
        POE_CHECK(ReadStarterRecord(strt, restored) && restored == save.state.starterGrantedIds);

        auto resaved = save;
        resaved.state.starterGrantedIds = restored;
        auto secondBatch = EncodeToMemory(resaved, kGen1Pack);
        POE_CHECK(FindRecord(secondBatch, kRecordStarter) == strt);
    }

    // 상한 256개 허용
    {
        auto save = MakeSnapshot(27, kGen1Pack.contentGeneration);
        save.state.starterGrantedIds.clear();
        for (std::uint32_t i = 0; i < kMaxStarterGrantedIds; ++i) {
            save.state.starterGrantedIds.push_back("gem.Item_" + std::to_string(i));
        }
        auto batch = EncodeToMemory(save, kGen1Pack);
        POE_CHECK(batch.complete);
        POE_CHECK(DecideSave(batch, false, kGen1Pack).disposition == SaveDisposition::Normal);
        std::vector<std::string> restored;
        POE_CHECK(ReadStarterRecord(FindRecord(batch, kRecordStarter), restored));
        POE_CHECK(restored == save.state.starterGrantedIds);
    }
}

POE_TEST("STRT v1 rejects count over 256, oversized IDs, syntax errors, duplicates, trailing bytes, wrong version and truncation")
{
    const std::vector<std::string> sentinel{ "gem.KeepMe" };
    const auto checkRejected = [&](const RawSaveRecord& record) {
        auto out = sentinel;
        POE_CHECK(!ReadStarterRecord(record, out));
        POE_CHECK(out == sentinel);
    };

    // 개수 257 거부
    {
        std::vector<std::string> tooMany;
        tooMany.reserve(kMaxStarterGrantedIds + 1);
        for (std::uint32_t i = 0; i <= kMaxStarterGrantedIds; ++i) {
            tooMany.push_back("gem.Item_" + std::to_string(i));
        }
        checkRejected(MakeRawStarterRecord(kStarterVersion, kMaxStarterGrantedIds + 1, tooMany));
    }

    // 64바이트 넘는 ID 거부 (65바이트, SaveString 한도 256 이내이지만 콘텐츠 ID 한도 초과)
    checkRejected(MakeRawStarterRecord(kStarterVersion, 1, { "gem." + std::string(61, 'x') }));

    // 문법 오류 ID 거부
    for (const std::string badId : {
             std::string(""), std::string("Spark"), std::string(".Spark"), std::string("gem."),
             std::string("Gem.Spark"), std::string("gem1.Spark"), std::string("gem.Bad-Id"),
             std::string("gem.Bad Id"), std::string("gem.\xEC\x8A\xA4") }) {
        checkRejected(MakeRawStarterRecord(kStarterVersion, 1, { badId }));
    }

    // 중복 ID 거부
    checkRejected(MakeRawStarterRecord(kStarterVersion, 3, { "gem.Spark", "sup.Fork", "gem.Spark" }));

    // 뒤 바이트 거부
    checkRejected(MakeRawStarterRecord(kStarterVersion, 1, { "gem.Spark" }, { 0 }));
    checkRejected(MakeRawStarterRecord(kStarterVersion, 0, {}, { 0xFF }));

    // 잘린 본문 (모든 접두) 거부
    const auto valid = MakeRawStarterRecord(kStarterVersion, 2, { "gem.Spark", "sup.Fork" });
    std::vector<std::string> parsed;
    POE_CHECK(ReadStarterRecord(valid, parsed) && parsed.size() == 2);
    for (std::size_t size = 0; size < valid.bytes.size(); ++size) {
        RawSaveRecord truncated = valid;
        truncated.bytes.resize(size);
        checkRejected(truncated);
    }

    // 잘못된 판·타입 거부
    for (const auto badVer : { 0u, kStarterVersion + 1 }) {
        RawSaveRecord wrongVer = valid;
        wrongVer.version = badVer;
        checkRejected(wrongVer);
    }
    RawSaveRecord wrongType = valid;
    wrongType.type = kRecordCore;
    checkRejected(wrongType);
}

POE_TEST("DecideSave enforces STRT presence and validity in generation one and forbids it in generation zero")
{
    auto gen1Save = MakeSnapshot(27, kGen1Pack.contentGeneration);
    gen1Save.state.starterGrantedIds = { "gem.Spark" };
    const auto validGen1 = EncodeToMemory(gen1Save, kGen1Pack);
    POE_CHECK(validGen1.complete && DecideSave(validGen1, false, kGen1Pack).disposition == SaveDisposition::Normal);

    // 세대 1 Normal에서 STRT 없음 → Preserve
    {
        auto missing = validGen1;
        POE_CHECK(std::erase_if(missing.records, [](const auto& r) { return r.type == kRecordStarter; }) == 1);
        POE_CHECK(DecideSave(missing, false, kGen1Pack).disposition == SaveDisposition::Preserve);
    }

    // 세대 1 Normal에서 STRT 둘 → Preserve
    {
        auto duplicate = validGen1;
        duplicate.records.push_back(FindRecord(duplicate, kRecordStarter));
        POE_CHECK(DecideSave(duplicate, false, kGen1Pack).disposition == SaveDisposition::Preserve);
    }

    // 세대 1 Normal에서 STRT 손상(잘림·뒤 바이트·중복 ID·문법 오류·판 오류) → Preserve
    for (const auto& badStarter : {
             MakeRawStarterRecord(kStarterVersion, 1, { "gem.Spark" }, { 0 }),
             MakeRawStarterRecord(kStarterVersion, 2, { "gem.Spark", "gem.Spark" }),
             MakeRawStarterRecord(kStarterVersion, 1, { "InvalidId" }),
             MakeRawStarterRecord(kStarterVersion + 1, 1, { "gem.Spark" }),
             RawSaveRecord{ kRecordStarter, kStarterVersion, { 1, 0 } } }) {
        auto damaged = validGen1;
        FindRecord(damaged, kRecordStarter) = badStarter;
        POE_CHECK(DecideSave(damaged, false, kGen1Pack).disposition == SaveDisposition::Preserve);
    }

    // 최초 설치(모드 기록 없음)와 새 게임은 STRT 없어도 통과
    POE_CHECK(DecideSave(SaveRecordBatch{}, false, kGen1Pack).disposition == SaveDisposition::FirstInstall);
    POE_CHECK(DecideSave(SaveRecordBatch{}, true, kGen1Pack).disposition == SaveDisposition::Normal);

    // 구판 정리 입력(GENR poe1/0, 무스탬프)은 STRT 없어도 통과하고, STRT가 섞여 있으면 Preserve
    const auto validGen0 = EncodeToMemory(MakeSnapshot(27, 0), kPack);
    auto unstampedGen0 = validGen0;
    POE_CHECK(std::erase_if(unstampedGen0.records, [](const auto& r) { return r.type == kRecordGeneration; }) == 1);
    for (const auto& legacy : { validGen0, unstampedGen0 }) {
        POE_CHECK(std::ranges::find(legacy.records, kRecordStarter, &RawSaveRecord::type) == legacy.records.end());
        POE_CHECK(DecideSave(legacy, false, kPack).disposition == SaveDisposition::Normal);
        POE_CHECK(DecideSave(legacy, false, kGen1Pack).disposition == SaveDisposition::CleanupFromLegacy);

        auto poisoned = legacy;
        poisoned.records.push_back(MakeRawStarterRecord(kStarterVersion, 0, {}));
        POE_CHECK(DecideSave(poisoned, false, kPack).disposition == SaveDisposition::Preserve);
        POE_CHECK(DecideSave(poisoned, false, kGen1Pack).disposition == SaveDisposition::Preserve);
    }

    // 세대 0 팩에 STRT만 단독으로 있어도 Preserve
    const SaveRecordBatch onlyStarter{ { MakeRawStarterRecord(kStarterVersion, 0, {}) } };
    POE_CHECK(DecideSave(onlyStarter, false, kPack).disposition == SaveDisposition::Preserve);
    POE_CHECK(DecideSave(onlyStarter, false, kGen1Pack).disposition == SaveDisposition::Preserve);
}

POE_TEST("WriteSnapshot writes one STRT in generation one, none in generation zero, and omits GENR when STRT fails")
{
    // 세대 1에는 STRT 정확히 하나, 세대 0에는 STRT 없음(필드에 값이 있어도 쓰지 않으며 바이트 불변)
    auto gen0Empty = MakeSnapshot(27, 0);
    auto gen0WithIds = gen0Empty;
    gen0WithIds.state.starterGrantedIds = { "gem.Spark" };
    const auto gen0BatchA = EncodeToMemory(gen0Empty, kPack);
    const auto gen0BatchB = EncodeToMemory(gen0WithIds, kPack);
    POE_CHECK(gen0BatchA.complete && gen0BatchB.complete);
    POE_CHECK(gen0BatchA.records == gen0BatchB.records);
    POE_CHECK(std::ranges::count(gen0BatchA.records, kRecordStarter, &RawSaveRecord::type) == 0);

    auto gen1 = MakeSnapshot(27, kGen1Pack.contentGeneration);
    gen1.state.starterGrantedIds = { "gem.Spark" };
    const auto gen1Batch = EncodeToMemory(gen1, kGen1Pack);
    POE_CHECK(gen1Batch.complete);
    POE_CHECK(std::ranges::count(gen1Batch.records, kRecordStarter, &RawSaveRecord::type) == 1);
    POE_CHECK(gen1Batch.records.back().type == kRecordGeneration);

    // STRT 열기·쓰기 실패 시 GENR 생략
    for (const bool failOpen : { true, false }) {
        FakeOutput output;
        (failOpen ? output.failOpen : output.failWrite) = kRecordStarter;
        POE_CHECK(!WriteSnapshot(gen1, kGen1Pack, [&](const RawSaveRecord& record) {
            return WriteSaveRecord(output, record);
        }));
        POE_CHECK(std::ranges::find(output.attempted, kRecordStarter) != output.attempted.end());
        POE_CHECK(std::ranges::find(output.attempted, kRecordGeneration) == output.attempted.end());
    }

    // 잘못된 STRT 내용(중복·문법 오류·상한 초과)도 실패 처리하고 GENR 생략
    for (const auto badCase : { 0, 1, 2 }) {
        auto bad = gen1;
        if (badCase == 0) {
            bad.state.starterGrantedIds = { "gem.Spark", "gem.Spark" };
        } else if (badCase == 1) {
            bad.state.starterGrantedIds = { "NotValidSyntax" };
        } else {
            bad.state.starterGrantedIds.clear();
            for (std::uint32_t i = 0; i <= kMaxStarterGrantedIds; ++i) {
                bad.state.starterGrantedIds.push_back("gem.Item_" + std::to_string(i));
            }
        }
        FakeOutput output;
        POE_CHECK(!WriteSnapshot(bad, kGen1Pack, [&](const RawSaveRecord& record) {
            return WriteSaveRecord(output, record);
        }));
        POE_CHECK(std::ranges::find(output.attempted, kRecordStarter) == output.attempted.end());
        POE_CHECK(std::ranges::find(output.attempted, kRecordGeneration) == output.attempted.end());
    }
}

POE_TEST("Every generation-one content ID passes the STRT grammar so a ready gem can always be recorded")
{
    for (const auto& key : SkyrimPoE::Core::GemForms::kContentKeys) {
        POE_CHECK(SkyrimPoE::Core::SaveRecord::IsValidStarterContentId(key.contentId));
    }
    static_assert(SkyrimPoE::Core::SaveRecord::kMaxStarterGrantedIds ==
                  SkyrimPoE::Core::GemForms::kMaxStarterGrantedRecordIds);
}
