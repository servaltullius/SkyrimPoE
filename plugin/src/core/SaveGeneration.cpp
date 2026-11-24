#include "core/SaveGeneration.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace SkyrimPoE::Core
{
    namespace
    {
        std::uint32_t Crc32(std::span<const std::uint8_t> bytes)
        {
            std::uint32_t crc = 0xFFFFFFFFu;
            for (const auto byte : bytes) {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit) {
                    const std::uint32_t mask = 0u - (crc & 1u);
                    crc = (crc >> 1u) ^ (0xEDB88320u & mask);
                }
            }
            return ~crc;
        }

        void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
        {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                bytes.push_back(static_cast<std::uint8_t>(value >> shift));
            }
        }

        void AppendString(std::vector<std::uint8_t>& bytes, std::string_view value)
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw std::length_error("GENR string exceeds its u32 length field");
            }
            AppendU32(bytes, static_cast<std::uint32_t>(value.size()));
            bytes.insert(bytes.end(), value.begin(), value.end());
        }

        bool ReadU32(std::span<const std::uint8_t>& bytes, std::uint32_t& value)
        {
            if (bytes.size() < sizeof(value)) {
                return false;
            }
            value = 0;
            for (unsigned i = 0; i < sizeof(value); ++i) {
                value |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
            }
            bytes = bytes.subspan(sizeof(value));
            return true;
        }

        bool ReadString(std::span<const std::uint8_t>& bytes, std::string& value)
        {
            std::uint32_t size = 0;
            if (!ReadU32(bytes, size) || size > bytes.size()) {
                return false;
            }
            value.assign(reinterpret_cast<const char*>(bytes.data()), size);
            bytes = bytes.subspan(size);
            return true;
        }
    }

    std::vector<std::uint8_t> WriteGenerationRecord(const SaveGeneration& generation)
    {
        std::vector<std::uint8_t> bytes;
        AppendString(bytes, generation.game);
        AppendU32(bytes, generation.contentGeneration);
        AppendString(bytes, generation.gameVersion);
        AppendString(bytes, generation.buildId);
        AppendString(bytes, generation.dllVersion);
        AppendU32(bytes, Crc32(bytes));
        return bytes;
    }

    GenerationRecord ReadGenerationRecord(std::uint32_t version, std::span<const std::uint8_t> bytes)
    {
        constexpr auto crcSize = sizeof(std::uint32_t);
        const GenerationRecord damaged{ GenerationRecordStatus::Damaged, {} };
        if (version != kGenerationRecordVersion || bytes.size() < crcSize) {
            return damaged;
        }

        auto payload = bytes.first(bytes.size() - crcSize);
        auto checksum = bytes.last(crcSize);
        std::uint32_t expected = 0;
        if (!ReadU32(checksum, expected) || Crc32(payload) != expected) {
            return damaged;
        }

        SaveGeneration generation;
        if (!ReadString(payload, generation.game) || !ReadU32(payload, generation.contentGeneration) ||
            !ReadString(payload, generation.gameVersion) || !ReadString(payload, generation.buildId) ||
            !ReadString(payload, generation.dllVersion) || !payload.empty() || generation.gameVersion.empty() ||
            generation.buildId.empty() || generation.dllVersion.empty()) {
            return damaged;
        }
        return { GenerationRecordStatus::Valid, std::move(generation) };
    }

    SaveDecision DecideSaveGeneration(const SaveGenerationInput& input)
    {
        using enum SaveDisposition;
        if (input.isNewGame) {
            return { Normal, "새 게임입니다." };
        }
        if (input.hasMigration) {
            return { Preserve, "진행 중인 정리·이전 기록(MIGR)을 이 DLL에서 재개할 수 없습니다." };
        }
        if (input.generation.status == GenerationRecordStatus::Damaged) {
            return { Preserve, "세이브 세대 기록(GENR)이 손상되었거나 지원하지 않는 판입니다." };
        }
        if (input.generation.status == GenerationRecordStatus::Valid) {
            const auto& save = input.generation.value;
            if (save.contentGeneration > input.packGeneration) {
                return { Preserve, "세이브의 콘텐츠 세대가 설치된 팩보다 높습니다." };
            }
            if (save.game == "poe1" && save.contentGeneration == 0 &&
                input.packGame == "poe2" && input.packGeneration > 0) {
                return { CleanupFromLegacy, "PoE1 세이브를 정리한 뒤 초기화해야 합니다." };
            }
            if (save.game != input.packGame) {
                return { Preserve, "세이브의 기준 게임(game)을 지원하지 않습니다." };
            }
            if (save.contentGeneration < input.packGeneration) {
                return { MigrateGeneration, "세이브의 콘텐츠 세대를 이전해야 합니다." };
            }
            return { Normal, "세이브와 팩의 콘텐츠 세대가 같습니다." };
        }
        if (!input.hasOtherRecords && !input.hasIntactCoreV2) {
            return { FirstInstall, "이 세이브에 모드 기록이 없거나 읽을 수 없어 처음 설치로 봅니다(.skse 분실·읽기 실패 포함)." };
        }
        if (input.packGeneration == 0) {
            return { Normal, "GENR이 없는 기존 세대 0 세이브입니다." };
        }
        if (input.hasIntactCoreV2) {
            return { CleanupFromLegacy, "PoE1 세이브를 정리한 뒤 초기화해야 합니다." };
        }
        return { Preserve, "세이브에 모드 기록이 있으나 GENR과 온전한 CORE v2가 없어 기준 게임을 식별할 수 없습니다." };
    }
}
