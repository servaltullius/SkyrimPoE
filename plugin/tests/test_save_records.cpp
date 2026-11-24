#include "core/SaveRecordFormat.h"
#include "test_harness.h"

#include <algorithm>
#include <cstring>
#include <optional>

using namespace SkyrimPoE::Core;
using namespace SkyrimPoE::Core::SaveRecord;

namespace
{
    const SaveGeneration kPack{ "poe1", 0, "3.30", "1234567890abcdef", "0.1.0" };

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

    SaveRecordBatch EncodeToMemory(const Snapshot& save)
    {
        SaveRecordBatch batch;
        batch.complete = WriteSnapshot(save, kPack, [&](RawSaveRecord&& record) {
            batch.records.push_back(std::move(record));
            return true;
        });
        return batch;
    }

    void Replay(const SaveRecordBatch& batch, FakeOutput& output)
    {
        POE_CHECK(ReplaySaveRecords(batch, [&](const RawSaveRecord& record) { return WriteSaveRecord(output, record); }));
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
