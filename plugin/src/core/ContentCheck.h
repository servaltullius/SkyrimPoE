#pragma once

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SkyrimPoE::Core
{
    struct ContentIdentity
    {
        std::string game;
        std::uint32_t contentGeneration;
        std::string gameVersion;
        std::string buildId;
    };

    struct ContentFile
    {
        std::string_view path;  // Relative to the package root, with '/' separators.
        std::optional<std::string> sha256;  // Absent when the caller could not read or hash the file.
    };

    struct ContentCheckResult
    {
        std::optional<ContentIdentity> identity;  // Present only when every check passed.
        std::vector<std::string> reasons;
    };

    // Pack and file order is core, tree, items, rules, followed by the ESP in files.
    // A generation-0 ESP must omit the generation GLOB.
    [[nodiscard]] ContentCheckResult CheckContent(
        const std::array<nlohmann::json, 4>& packMeta,
        const nlohmann::json& manifest,
        const std::array<ContentFile, 5>& files,
        std::optional<std::uint32_t> espGeneration);

}
