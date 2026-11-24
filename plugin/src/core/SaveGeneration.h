#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SkyrimPoE::Core
{
    inline constexpr std::uint32_t kGenerationRecordVersion = 1;

    struct SaveGeneration
    {
        std::string game;
        std::uint32_t contentGeneration = 0;
        std::string gameVersion;
        std::string buildId;
        std::string dllVersion;

        bool operator==(const SaveGeneration&) const = default;
    };

    enum class GenerationRecordStatus { Missing, Valid, Damaged };

    struct GenerationRecord
    {
        GenerationRecordStatus status = GenerationRecordStatus::Missing;
        SaveGeneration value;
    };

    // v1 uses little-endian u32 values and u32 byte lengths for strings.
    // The SKSE record header supplies the total length. The payload ends in CRC-32/ISO-HDLC.
    [[nodiscard]] std::vector<std::uint8_t> WriteGenerationRecord(const SaveGeneration& generation);
    [[nodiscard]] GenerationRecord ReadGenerationRecord(std::uint32_t version, std::span<const std::uint8_t> bytes);

    [[nodiscard]] std::uint32_t SaveRecordCrc32(std::span<const std::uint8_t> bytes);

    enum class SaveDisposition { Normal, FirstInstall, CleanupFromLegacy, MigrateGeneration, Preserve, ResumeMigration };

    struct SaveDecision
    {
        SaveDisposition disposition;
        std::string reason;
    };

    enum class MigrationRecordStatus { Missing, Valid, Damaged };

    struct MigrationGeneration
    {
        MigrationRecordStatus status = MigrationRecordStatus::Missing;
        std::string sourceGame;
        std::uint32_t sourceGeneration = 0;
        std::string targetGame;
        std::uint32_t targetGeneration = 0;
    };

    struct SaveGenerationInput
    {
        bool isNewGame = false;
        GenerationRecord generation;
        MigrationGeneration migration;
        bool hasOtherRecords = false;
        bool hasIntactCoreV2 = false;
        bool hasIntactCoreV2Front = false;
        std::string_view packGame;
        std::uint32_t packGeneration = 0;
    };

    [[nodiscard]] SaveDecision DecideSaveGeneration(const SaveGenerationInput& input);
}
