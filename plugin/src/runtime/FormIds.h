#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace SkyrimPoE::Runtime::FormIds
{
    inline constexpr std::string_view kPluginName = "SkyrimPoE.esp";

    // houseCARL 생성 고정 FormID (esp/records.json, 0x800..0x81E)
    inline constexpr RE::FormID kProxySpellEffect = 0x000800;
    inline constexpr RE::FormID kProxyPowerEffect = 0x000801;

    inline constexpr std::array<RE::FormID, 9> kProxySpells = {
        0x000802, 0x000803, 0x000804,
        0x000805, 0x000806, 0x000807,
        0x000808, 0x000809, 0x00080A
    };

    inline constexpr std::array<RE::FormID, 9> kProxyPowers = {
        0x00080B, 0x00080C, 0x00080D,
        0x00080E, 0x00080F, 0x000810,
        0x000811, 0x000812, 0x000813
    };

    struct GemFormEntry
    {
        std::string_view gemId;
        RE::FormID localFormId;
    };

    inline constexpr std::array<GemFormEntry, 9> kStage2GemForms = {{
        { "Cyclone",                          0x000814 },
        { "SupportCastOnCriticalStrike",      0x000815 },
        { "IceNova",                          0x000816 },
        { "ShockNova",                        0x000817 },
        { "SupportIncreasedCriticalStrikes",  0x000818 },
        { "SupportMeleePhysicalDamage",       0x000819 },
        { "SupportBrutality",                 0x00081A },
        { "SupportIncreasedAreaOfEffect",     0x00081B },
        { "SupportConcentratedEffect",        0x00081C },
    }};

    inline constexpr RE::FormID kVfxIceNova = 0x00081D;
    inline constexpr RE::FormID kVfxShockNova = 0x00081E;

    template <class T>
    [[nodiscard]] T* Lookup(RE::FormID localId)
    {
        auto* handler = RE::TESDataHandler::GetSingleton();
        return handler ? handler->LookupForm<T>(localId, kPluginName) : nullptr;
    }

    [[nodiscard]] inline RE::FormID FindLocalFormIdForGem(std::string_view gemId) noexcept
    {
        for (const auto& entry : kStage2GemForms) {
            if (entry.gemId == gemId) {
                return entry.localFormId;
            }
        }
        return 0;
    }

    [[nodiscard]] inline std::string_view FindGemIdForLocalFormId(RE::FormID localId) noexcept
    {
        for (const auto& entry : kStage2GemForms) {
            if (entry.localFormId == localId) {
                return entry.gemId;
            }
        }
        return {};
    }
}
