#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace SkyrimPoE::Core
{
    struct EspFormObservation
    {
        std::string_view name;
        std::uint32_t localId;
        bool found;
        bool typeMatches;
        bool ownerMatches;
        bool deleted;
    };

    struct EspFormCheckResult
    {
        std::size_t found = 0;
        std::size_t missing = 0;
        std::size_t wrongType = 0;
        std::size_t wrongOwner = 0;
        std::size_t deleted = 0;
        std::size_t failedForms = 0;
        std::string reason;
    };

    [[nodiscard]] EspFormCheckResult CheckEspForms(std::span<const EspFormObservation> forms);
}
