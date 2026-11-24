#pragma once

#include "core/MapSave.h"
#include "core/SaveGeneration.h"
#include "core/SaveRecords.h"
#include "core/SaveRecordState.h"

#include <functional>
#include <span>
#include <string_view>

namespace SkyrimPoE::Core::SaveRecord
{
    inline constexpr std::uint32_t kRecordCore = 'CORE';
    inline constexpr std::uint32_t kRecordGeneration = 'GENR';
    inline constexpr std::uint32_t kRecordGemKeys = 'GKEY';
    inline constexpr std::uint32_t kRecordTree = 'TREE';
    inline constexpr std::uint32_t kRecordItems = 'ITEM';
    inline constexpr std::uint32_t kRecordWorldDrops = 'WDRP';
    inline constexpr std::uint32_t kRecordSpirit = 'SPRT';
    inline constexpr std::uint32_t kRecordGear = 'GEAR';
    inline constexpr std::uint32_t kRecordSpeedMods = 'SPDM';
    inline constexpr std::uint32_t kRecordAtlas = 'ATLS';
    inline constexpr std::uint32_t kRecordMaps = 'MAPS';
    inline constexpr std::uint32_t kCoreVersion = 2;
    inline constexpr std::uint32_t kGemKeysVersion = 1;
    inline constexpr std::uint32_t kTreeVersion = 1;
    inline constexpr std::uint32_t kItemsVersion = 2;
    inline constexpr std::uint32_t kWorldDropsVersion = 1;
    inline constexpr std::uint32_t kSpiritVersion = 1;
    inline constexpr std::uint32_t kGearVersion = 1;
    inline constexpr std::uint32_t kSpeedModsVersion = 1;
    inline constexpr std::uint32_t kAtlasVersion = kAtlasSaveVersion;
    inline constexpr std::uint32_t kMapsVersion = kMapSaveVersion;

    inline constexpr std::uint32_t kMaxListLen = 1024;
    inline constexpr std::uint32_t kMaxTreeNodes = 4096;
    inline constexpr std::uint32_t kMaxItemRecords = 8192;
    inline constexpr std::uint32_t kMaxWorldDropRecords = 64;
    inline constexpr std::uint32_t kMaxGemDepth = 4;
    inline constexpr std::uint32_t kMaxSocketedJewels = 64;
    inline constexpr std::uint32_t kPlayerRefFormId = 0x00000014;

    struct Snapshot
    {
        Runtime::PersistentState state;
        std::vector<Runtime::TrackedGemEntry> tracked;
        std::vector<Runtime::TrackedItemEntry> trackedItems;
        std::vector<Runtime::SavedWorldDropEntry> savedWorldDrops;
        ActiveMapState savedMapState;
        std::vector<std::pair<std::uint32_t, float>> savedSpeedMods;
    };

    using RecordSink = std::function<bool(RawSaveRecord&&)>;
    using Report = std::function<void(std::string_view)>;

    bool ReadGemInstance(std::span<const std::uint8_t>& bytes, GemInstance& out, std::uint32_t depth = 0);
    bool ReadLegacyCoreV2Front(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& out);
    bool ReadLegacyCoreV2Slots(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& out);
    bool ReadCoreV2(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& out);
    SaveDecision DecideSave(const SaveRecordBatch& batch, bool isNewGame, const SaveGeneration& pack);
    bool WriteSnapshot(const Snapshot& snapshot, const SaveGeneration& generation, const RecordSink& sink,
        const Report& reportWarning = {});
}
