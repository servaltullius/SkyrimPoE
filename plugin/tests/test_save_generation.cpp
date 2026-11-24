#include "core/SaveGeneration.h"
#include "test_harness.h"

#include <algorithm>
#include <array>

using namespace SkyrimPoE::Core;

namespace
{
    const SaveGeneration kGeneration{ "poe2", 2, "1.0", "1234567890abcdef", "0.1.0" };

    SaveGenerationInput WithPack(std::uint32_t generation)
    {
        SaveGenerationInput input;
        input.packGame = generation == 0 ? "poe1" : "poe2";
        input.packGeneration = generation;
        return input;
    }

    void SetRecord(SaveGenerationInput& input, std::uint32_t generation, std::string_view game)
    {
        input.generation = { GenerationRecordStatus::Valid,
            { std::string(game), generation, "version", "other-build", "older-dll" } };
    }

    void ExpectDecision(const SaveGenerationInput& input, SaveDisposition disposition, std::string_view reason)
    {
        const auto result = DecideSaveGeneration(input);
        POE_CHECK(result.disposition == disposition);
        POE_CHECK(result.reason.find(reason) != std::string_view::npos);
    }
}

POE_TEST("GENR v1 round trips with little-endian CRC-32")
{
    const auto bytes = WriteGenerationRecord(kGeneration);
    const auto parsed = ReadGenerationRecord(kGenerationRecordVersion, bytes);
    POE_CHECK(parsed.status == GenerationRecordStatus::Valid);
    POE_CHECK(parsed.value == kGeneration);
    constexpr std::array<std::uint8_t, 4> expectedCrc{ 0x0F, 0xD9, 0x5A, 0x75 };
    POE_CHECK(std::ranges::equal(std::span(bytes).last(expectedCrc.size()), expectedCrc));
}

POE_TEST("GENR v1 rejects each truncated prefix")
{
    const auto bytes = WriteGenerationRecord(kGeneration);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const auto parsed = ReadGenerationRecord(kGenerationRecordVersion, std::span(bytes).first(size));
        POE_CHECK(parsed.status == GenerationRecordStatus::Damaged);
    }
}

POE_TEST("GENR v1 rejects a CRC mismatch")
{
    auto bytes = WriteGenerationRecord(kGeneration);
    bytes[5] ^= 0x80;
    POE_CHECK(ReadGenerationRecord(kGenerationRecordVersion, bytes).status == GenerationRecordStatus::Damaged);
}

POE_TEST("GENR v1 rejects an unsupported record version")
{
    const auto bytes = WriteGenerationRecord(kGeneration);
    for (const auto version : { 0u, kGenerationRecordVersion + 1 }) {
        POE_CHECK(ReadGenerationRecord(version, bytes).status == GenerationRecordStatus::Damaged);
    }
}

POE_TEST("GENR v1 rejects empty gameVersion, buildId and dllVersion with a valid CRC")
{
    constexpr std::array fields{ &SaveGeneration::gameVersion, &SaveGeneration::buildId, &SaveGeneration::dllVersion };
    for (const auto field : fields) {
        auto generation = kGeneration;
        (generation.*field).clear();
        const auto bytes = WriteGenerationRecord(generation);
        POE_CHECK(ReadGenerationRecord(kGenerationRecordVersion, bytes).status == GenerationRecordStatus::Damaged);
    }
}

POE_TEST("Save generation gives the new game message priority")
{
    for (const auto packGeneration : { 0u, 1u }) {
        auto input = WithPack(packGeneration);
        input.isNewGame = true;
        input.migration.status = MigrationRecordStatus::Damaged;
        input.hasOtherRecords = true;
        input.hasIntactCoreV2 = true;
        input.generation.status = GenerationRecordStatus::Damaged;
        ExpectDecision(input, SaveDisposition::Normal, "새 게임");
        ExpectDecision(WithPack(packGeneration), SaveDisposition::FirstInstall, ".skse 분실·읽기 실패 포함");
    }
}

POE_TEST("Save generation accepts a matching GENR even if its build ID differs")
{
    for (const auto packGeneration : { 0u, 1u }) {
        auto input = WithPack(packGeneration);
        SetRecord(input, packGeneration, input.packGame);
        input.hasIntactCoreV2 = packGeneration == 0;
        ExpectDecision(input, SaveDisposition::Normal, "세대가 같습니다");
    }
}

POE_TEST("Save generation preserves a damaged migration")
{
    for (const auto packGeneration : { 0u, 1u }) {
        auto input = WithPack(packGeneration);
        SetRecord(input, packGeneration, input.packGame);
        input.migration.status = MigrationRecordStatus::Damaged;
        ExpectDecision(input, SaveDisposition::Preserve, "MIGR");
    }
}

POE_TEST("Save generation zero keeps unstamped PoE1 saves on the existing load path")
{
    auto input = WithPack(0);
    input.hasOtherRecords = true;
    for (const auto intactCore : { true, false }) {
        input.hasIntactCoreV2 = intactCore;
        ExpectDecision(input, SaveDisposition::Normal, "기존 세대 0");
    }
}

POE_TEST("Save generation one distinguishes legacy cleanup from an undecidable old save")
{
    auto input = WithPack(1);
    input.hasOtherRecords = true;
    input.hasIntactCoreV2 = true;
    ExpectDecision(input, SaveDisposition::CleanupFromLegacy, "정리");
    input.hasIntactCoreV2 = false;
    ExpectDecision(input, SaveDisposition::Preserve, "기준 게임을 식별할 수 없습니다");
}

POE_TEST("A PoE1 generation-0 GENR also requests legacy cleanup with PoE2 packs")
{
    for (const auto packGeneration : { 1u, 2u }) {
        auto input = WithPack(packGeneration);
        SetRecord(input, 0, "poe1");
        ExpectDecision(input, SaveDisposition::CleanupFromLegacy, "PoE1 세이브를 정리");
        input.hasOtherRecords = true;
        input.hasIntactCoreV2 = true;
        ExpectDecision(input, SaveDisposition::CleanupFromLegacy, "PoE1 세이브를 정리");
    }
}

POE_TEST("PoE2 generation 2 requests migration for a generation-1 GENR")
{
    auto input = WithPack(2);
    SetRecord(input, 1, "poe2");
    input.hasOtherRecords = true;
    input.hasIntactCoreV2 = true;
    ExpectDecision(input, SaveDisposition::MigrateGeneration, "이전");
}

POE_TEST("Save generation preserves damaged GENR, unknown games and newer generations")
{
    for (const auto packGeneration : { 0u, 1u }) {
        auto input = WithPack(packGeneration);
        input.generation.status = GenerationRecordStatus::Damaged;
        ExpectDecision(input, SaveDisposition::Preserve, "손상");
        SetRecord(input, packGeneration, "unknown");
        ExpectDecision(input, SaveDisposition::Preserve, "기준 게임(game)을 지원하지 않습니다");
        SetRecord(input, packGeneration + 1, "poe2");
        ExpectDecision(input, SaveDisposition::Preserve, "세대가 설치된 팩보다 높습니다");
    }
}

POE_TEST("PoE2 GENR and CORE v2 can coexist in a matching generation")
{
    auto input = WithPack(1);
    SetRecord(input, 1, input.packGame);
    input.hasOtherRecords = true;
    input.hasIntactCoreV2 = true;
    ExpectDecision(input, SaveDisposition::Normal, "세대가 같습니다");
}
