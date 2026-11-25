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
    inline constexpr std::uint32_t kRecordPlayerRates = 'PRAT';
    inline constexpr std::uint32_t kRecordAtlas = 'ATLS';
    inline constexpr std::uint32_t kRecordMaps = 'MAPS';
    inline constexpr std::uint32_t kCoreVersion = 3;
    inline constexpr std::uint32_t kGemKeysVersion = 1;
    inline constexpr std::uint32_t kLegacyTreeVersion = 1;
    inline constexpr std::uint32_t kTreeVersion = 2;
    inline constexpr std::uint32_t kItemsVersion = 2;
    inline constexpr std::uint32_t kWorldDropsVersion = 1;
    inline constexpr std::uint32_t kSpiritVersion = 1;
    inline constexpr std::uint32_t kLegacyGearVersion = 1;
    inline constexpr std::uint32_t kGearVersion = 2;
    inline constexpr std::uint32_t kGearItemVersion = 2;
    inline constexpr std::uint32_t kSpeedModsVersion = 1;
    inline constexpr std::uint32_t kPlayerRatesVersion = 1;
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

    struct PlayerRateDeltas
    {
        double speedMult = 0.0;
        double magickaRateMult = 0.0;

        bool operator==(const PlayerRateDeltas&) const = default;
    };

    struct PlayerRateReconciliation
    {
        float changeSpeedMult = 0.0f;
        float changeMagickaRateMult = 0.0f;
        PlayerRateDeltas applied;
    };

    struct GearHeader
    {
        struct Charm
        {
            std::uint8_t occupied = 0;
            std::uint8_t kind = 0;
            double charges = 0.0;
        };

        InventoryItemKey ring2Key{};
        std::uint32_t ring2BaseFormId = 0;
        InventoryItemKey beltKey{};
        std::uint32_t beltBaseFormId = 0;
        std::array<Charm, kMaxCharmSlots> charms{};
        std::uint32_t charmCount = 0;
        std::uint32_t jewelCount = 0;
        std::uint32_t jewelGeneration = 0;
    };

    bool ReadGemInstance(std::span<const std::uint8_t>& bytes, GemInstance& out, std::uint32_t depth = 0);
    bool ReadLegacyCoreV2Front(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& out);
    bool ReadLegacyCoreV2Slots(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& out);
    bool ReadCoreV2(std::span<const std::uint8_t>& bytes, Runtime::PersistentState& out);
    bool ReadCoreRecord(const RawSaveRecord& record, Runtime::PersistentState& out);
    [[nodiscard]] bool AreValidPlayerRateDeltas(PlayerRateDeltas deltas) noexcept;
    [[nodiscard]] std::optional<PlayerRateDeltas> ReadPlayerRateRecord(const RawSaveRecord& record) noexcept;
    [[nodiscard]] std::optional<PlayerRateDeltas> ReadPlayerRates(const SaveRecordBatch& batch) noexcept;
    [[nodiscard]] float ComputePlayerRateDelta(double observed, double applied, double multiplier) noexcept;
    [[nodiscard]] PlayerRateReconciliation ReconcilePlayerRates(double speedMult, double magickaRateMult,
        PlayerRateDeltas applied, double speedFactor, double magickaRateFactor) noexcept;
    bool ReadTreeRecord(const RawSaveRecord& record, PassiveTreeState& out);
    // Leaves the cursor at the first jewel. Every socket in the record shares jewelGeneration.
    bool ReadGearHeader(std::uint32_t version, std::span<const std::uint8_t>& bytes, GearHeader& out);
    SaveDecision DecideSave(const SaveRecordBatch& batch, bool isNewGame, const SaveGeneration& pack,
        std::optional<std::uint32_t> activeTreeGeneration = std::nullopt);
    bool WriteSnapshot(const Snapshot& snapshot, const SaveGeneration& generation, const RecordSink& sink,
        const Report& reportWarning = {});
}
