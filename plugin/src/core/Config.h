#pragma once

#include "core/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace SkyrimPoE::Core
{
    struct ConfigLoadResult
    {
        Config config;
        bool fileFound = false;
        std::vector<std::string> warnings;
    };

    [[nodiscard]] ConfigLoadResult LoadConfigFromFile(const std::filesystem::path& path);
    [[nodiscard]] ConfigLoadResult LoadConfigFromString(std::string_view content);
}
