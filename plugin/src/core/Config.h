#pragma once

#include "core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace SkyrimPoE::Core
{
    struct MapTravelSettings
    {
        double timeoutSec = 20.0;
        double arrivalRadius = 512.0;
        std::uint32_t maxAttempts = 2;
    };

    struct ConfigLoadResult
    {
        Config config;
        MapTravelSettings mapTravel;
        bool fileFound = false;
        std::vector<std::string> warnings;
    };

    [[nodiscard]] ConfigLoadResult LoadConfigFromFile(const std::filesystem::path& path);
    [[nodiscard]] ConfigLoadResult LoadConfigFromString(std::string_view content);
}
