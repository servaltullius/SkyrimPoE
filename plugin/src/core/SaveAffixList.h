#pragma once

#include "core/ItemTypes.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace SkyrimPoE::Core
{
    inline constexpr std::uint32_t kMaxSaveStringBytes = 256;
    inline constexpr std::uint32_t kMaxSavedAffixes = 16;
    inline constexpr std::uint32_t kMaxSavedAffixValues = 16;

    template <class Write>
    bool WriteSaveString(Write&& write, std::string_view str)
    {
        const auto length = static_cast<std::uint32_t>(std::min<std::size_t>(str.size(), kMaxSaveStringBytes));
        return write(&length, sizeof(length)) && (length == 0 || write(str.data(), length));
    }

    template <class Read>
    bool ReadSaveString(Read&& read, std::string& out)
    {
        std::uint32_t length = 0;
        if (!read(&length, sizeof(length)) || length > kMaxSaveStringBytes) {
            return false;
        }
        out.resize(length);
        if (length != 0 && !read(out.data(), length)) {
            out.clear();
            return false;
        }
        return true;
    }

    template <class Write>
    bool WriteSaveAffixList(Write&& write, const std::vector<RolledAffix>& list)
    {
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(list.size(), kMaxSavedAffixes));
        if (!write(&count, sizeof(count))) {
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& affix = list[i];
            const auto values = static_cast<std::uint32_t>(std::min<std::size_t>(affix.values.size(), kMaxSavedAffixValues));
            if (!WriteSaveString(write, affix.affixId) || !write(&values, sizeof(values))) {
                return false;
            }
            for (std::uint32_t j = 0; j < values; ++j) {
                if (!write(&affix.values[j], sizeof(double))) {
                    return false;
                }
            }
        }
        return true;
    }

    template <class Read>
    bool ReadSaveAffixList(Read&& read, std::vector<RolledAffix>& out, bool strictValues = false)
    {
        std::uint32_t count = 0;
        if (!read(&count, sizeof(count)) || count > kMaxSavedAffixes) {
            return false;
        }
        out.clear();
        out.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            RolledAffix affix;
            std::uint32_t values = 0;
            if (!ReadSaveString(read, affix.affixId) || !read(&values, sizeof(values)) || values > kMaxSavedAffixValues) {
                return false;
            }
            affix.values.resize(values);
            for (double& value : affix.values) {
                if (!read(&value, sizeof(value))) {
                    return false;
                }
                if (!std::isfinite(value)) {
                    if (strictValues) {
                        return false;
                    }
                    value = 0.0;
                }
            }
            out.push_back(std::move(affix));
        }
        return true;
    }
}
