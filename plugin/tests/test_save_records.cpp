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

    Snapshot MakeSnapshot(std::uint32_t exileLevel)
    {
        Snapshot save;
        save.state.exileLevel = exileLevel;
        save.state.exileXp = 12345;
        save.state.starterGemsGiven = true;
        save.state.appliedHealthDelta = 2.5;
        auto& slot = save.state.slots[1];
        slot.occupied = true;
        slot.gem = GemInstance{ .instanceId = 11, .gemId = "IceNova" };
        slot.gem.socketedGems.push_back(GemInstance{ .instanceId = 12, .gemId = "SupportAddedCold" });
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
    auto expected = MakeSnapshot(27);
    expected.state.appliedSpeedMultDelta = 37.5;
    expected.state.appliedMagickaRateMultDelta = -58.75;
    const auto legacy = EncodeToMemory(MakeSnapshot(27));
    for (const auto& pack : { kPack, SaveGeneration{ "poe2", 1, "0.5", "1234567890abcdef", "0.1.0" } }) {
        expected.state.tree.generation = pack.contentGeneration;
        SaveRecordBatch written;
        written.complete = WriteSnapshot(expected, pack, [&](RawSaveRecord&& record) {
            written.records.push_back(std::move(record));
            return true;
        });
        POE_CHECK(written.complete);
        const auto& core = written.records.front();
        POE_CHECK(core.type == kRecordCore && core.version == (pack.contentGeneration == 0 ? 2u : 3u));
        POE_CHECK(core.bytes == legacy.records.front().bytes);
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
    auto save = MakeSnapshot(27);
    save.state.tree.generation = pack.contentGeneration;
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
    auto save = MakeSnapshot(27);
    save.state.tree.generation = kGen1Pack.contentGeneration;
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
    auto save = MakeSnapshot(27);
    save.state.tree.generation = kGen1Pack.contentGeneration;
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
            auto save = MakeSnapshot(27);
            save.state.*delta = invalid;
            save.state.tree.generation = kGen1Pack.contentGeneration;
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
        auto save = MakeSnapshot(27);
        save.state.tree.generation = pack.contentGeneration;
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
    auto save = MakeSnapshot(27);
    save.state.tree.generation = kGen1Pack.contentGeneration;
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
    auto save = MakeSnapshot(27);
    save.state.tree.generation = kGen1Pack.contentGeneration;
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
