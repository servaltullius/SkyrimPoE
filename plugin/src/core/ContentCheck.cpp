#include "core/ContentCheck.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <span>

namespace SkyrimPoE::Core
{
    namespace
    {
        using nlohmann::json;

        struct PackSchema
        {
            std::string_view name;
            std::uint32_t version;
        };

        struct SupportedContent
        {
            std::string_view game;
            std::uint32_t generation;
            std::array<PackSchema, 3> packs;
        };

        constexpr SupportedContent kSupportedContents[] = {
            { "poe1", 0, {{ { "skyrimpoe.core", 1 }, { "skyrimpoe.tree", 1 }, { "skyrimpoe.items", 2 } }} },
        };

        const json* FindField(const json& object, std::string_view key)
        {
            const auto value = object.find(key);
            return value == object.end() ? nullptr : &*value;
        }

        const std::string* ReadText(const json& object, std::string_view key)
        {
            const auto* field = FindField(object, key);
            const auto* text = field ? field->get_ptr<const std::string*>() : nullptr;
            return text && text->find_first_not_of(" \t\r\n") != std::string::npos ? text : nullptr;
        }

        std::optional<std::uint32_t> ReadU32(const json& object, std::string_view key)
        {
            const auto* field = FindField(object, key);
            if (!field || !field->is_number_integer()) {
                return std::nullopt;
            }
            const auto number = field->get<std::uint64_t>();
            if (number > std::numeric_limits<std::uint32_t>::max()) {
                return std::nullopt;
            }
            return static_cast<std::uint32_t>(number);
        }

        bool IsLowercaseHex(std::string_view text, std::size_t length)
        {
            return text.size() == length && std::ranges::all_of(text, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        }

        struct PackageFields
        {
            const std::string* game = nullptr;
            std::optional<std::uint32_t> generation;
            const std::string* buildId = nullptr;
        };

        PackageFields ReadPackageFields(
            const json& object, std::string_view label, std::vector<std::string>& reasons)
        {
            PackageFields fields{ ReadText(object, "game"), ReadU32(object, "contentGeneration"),
                ReadText(object, "buildId") };
            if (!fields.game) {
                reasons.push_back(std::format("{}: missing or invalid game.", label));
            }
            if (!fields.generation) {
                reasons.push_back(std::format("{}: missing or invalid contentGeneration.", label));
            }
            if (!fields.buildId || !IsLowercaseHex(*fields.buildId, 16)) {
                reasons.push_back(std::format("{}: buildId must be 16 lowercase hexadecimal digits.", label));
            }
            return fields;
        }

        void ComparePackageFields(const PackageFields& expected, const PackageFields& actual,
            std::string_view label, std::vector<std::string>& reasons)
        {
            if (expected.game && actual.game && *expected.game != *actual.game) {
                reasons.push_back(std::format("{}: game mismatch (expected {}, found {}).",
                    label, *expected.game, *actual.game));
            }
            if (expected.generation && actual.generation && expected.generation != actual.generation) {
                reasons.push_back(std::format("{}: contentGeneration mismatch (expected {}, found {}).",
                    label, *expected.generation, *actual.generation));
            }
            if (expected.buildId && actual.buildId && *expected.buildId != *actual.buildId) {
                reasons.push_back(std::format("{}: buildId mismatch (expected {}, found {}).",
                    label, *expected.buildId, *actual.buildId));
            }
        }

        void CheckPackSchema(const json& meta, const PackageFields& fields, std::size_t pack,
            std::string_view path, std::vector<std::string>& reasons)
        {
            const auto matchesGame = [&](const SupportedContent& supported) {
                return fields.game && supported.game == *fields.game;
            };
            const auto compatible = std::ranges::find_if(kSupportedContents, [&](const SupportedContent& supported) {
                return matchesGame(supported) && fields.generation == supported.generation;
            });
            const auto end = std::ranges::end(kSupportedContents);
            if (fields.game && !std::ranges::any_of(kSupportedContents, matchesGame)) {
                reasons.push_back(std::format("{}: unsupported game '{}'.", path, *fields.game));
            } else if (fields.game && fields.generation && compatible == end) {
                reasons.push_back(std::format("{}: unsupported contentGeneration {} for {}.",
                    path, *fields.generation, *fields.game));
            }

            // Continue diagnosing the schema fields even if the game and generation are unusable.
            std::span<const SupportedContent> candidates = kSupportedContents;
            if (compatible != end) {
                candidates = { compatible, 1 };
            }
            const auto* schema = ReadText(meta, "schema");
            if (!schema) {
                reasons.push_back(std::format("{}: missing or invalid schema.", path));
            } else if (!std::ranges::any_of(candidates, [&](const SupportedContent& supported) {
                           return supported.packs[pack].name == *schema;
                       })) {
                reasons.push_back(std::format("{}: unsupported schema '{}'.", path, *schema));
            }
            const auto version = ReadU32(meta, "schemaVersion");
            if (!version) {
                reasons.push_back(std::format("{}: missing or invalid schemaVersion.", path));
            } else if (!std::ranges::any_of(candidates, [&](const SupportedContent& supported) {
                           return supported.packs[pack].version == *version;
                       })) {
                reasons.push_back(std::format("{}: unsupported schemaVersion {}.", path, *version));
            }
        }

        void CheckFileHash(const ContentFile& file, const json* entries, std::vector<std::string>& reasons)
        {
            if (!file.sha256) {
                reasons.push_back(std::format("{}: file missing or could not read SHA-256.", file.path));
            }
            if (!entries) {
                return;
            }

            const json* listed = nullptr;
            for (const auto& entry : *entries) {
                const auto* path = ReadText(entry, "path");
                if (!path || *path != file.path) {
                    continue;
                }
                if (listed) {
                    reasons.push_back(std::format("{}: duplicate entries in manifest.json.", file.path));
                    break;
                }
                listed = &entry;
            }
            if (!listed) {
                reasons.push_back(std::format("{}: file missing from manifest.json.", file.path));
                return;
            }
            const auto* expected = ReadText(*listed, "sha256");
            if (!expected || !IsLowercaseHex(*expected, 64)) {
                reasons.push_back(std::format("{}: missing or invalid sha256 in manifest.json.", file.path));
            } else if (file.sha256 && *file.sha256 != *expected) {
                reasons.push_back(std::format("{}: sha256 mismatch (manifest {}, actual {}).",
                    file.path, *expected, *file.sha256));
            }
        }
    }

    ContentCheckResult CheckContent(const std::array<json, 3>& packMeta, const json& manifest,
        const std::array<ContentFile, 4>& files, std::optional<std::uint32_t> espGeneration)
    {
        ContentCheckResult result;
        auto& reasons = result.reasons;
        PackageFields common;
        const std::string* gameVersion = nullptr;
        for (std::size_t pack = 0; pack < packMeta.size(); ++pack) {
            const auto& meta = packMeta[pack];
            const auto path = files[pack].path;
            if (!meta.is_object()) {
                reasons.push_back(std::format("{}: missing or invalid meta object.", path));
                continue;
            }
            const auto fields = ReadPackageFields(meta, path, reasons);
            CheckPackSchema(meta, fields, pack, path, reasons);

            const auto* version = ReadText(meta, "gameVersion");
            if (!version) {
                reasons.push_back(std::format("{}: missing or invalid gameVersion.", path));
            } else if (!gameVersion) {
                gameVersion = version;
            } else if (*gameVersion != *version) {
                reasons.push_back(std::format("{}: gameVersion mismatch (expected {}, found {}).",
                    path, *gameVersion, *version));
            }

            ComparePackageFields(common, fields, path, reasons);
            if (!common.game) {
                common.game = fields.game;
            }
            if (!common.generation) {
                common.generation = fields.generation;
            }
            if (!common.buildId) {
                common.buildId = fields.buildId;
            }
        }

        const json* entries = nullptr;
        if (!manifest.is_object()) {
            reasons.emplace_back("manifest.json: missing or invalid manifest object.");
        } else {
            if (ReadU32(manifest, "manifestVersion") != 1u) {
                reasons.emplace_back("manifest.json: missing or unsupported manifestVersion (expected 1).");
            }
            ComparePackageFields(common, ReadPackageFields(manifest, "manifest.json", reasons), "manifest.json", reasons);
            entries = FindField(manifest, "files");
            if (!entries || !entries->is_array()) {
                reasons.emplace_back("manifest.json: missing or invalid files array.");
                entries = nullptr;
            }
        }
        for (const auto& file : files) {
            CheckFileHash(file, entries, reasons);
        }

        if (common.generation == 0u && espGeneration) {
            reasons.push_back(std::format("{}: generation 0 must omit the SPOE_ContentGeneration GLOB.",
                files.back().path));
        }
        const auto generation = espGeneration.value_or(0);
        if (common.generation && generation != *common.generation) {
            reasons.push_back(std::format("{}: ESP contentGeneration mismatch (packs {}, ESP {}).",
                files.back().path, *common.generation, generation));
        }
        if (reasons.empty()) {
            result.identity = ContentIdentity{ *common.game, *common.generation, *gameVersion, *common.buildId };
        }
        return result;
    }
}
