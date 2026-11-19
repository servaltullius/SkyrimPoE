#pragma once

#include "core/ItemTypes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace SkyrimPoE::Runtime::FormIds
{
    inline constexpr std::string_view kPluginName = "SkyrimPoE.esp";

    // houseCARL 생성 고정 FormID (esp/records.json, 0x800..0x87C, 총 125폼 ESL)
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

    // 4단계(결정 158): 핵심 화폐 오브 12종 (0x00081F..0x00082A, CurrencyKind 순서와 1:1 일치)
    inline constexpr RE::FormID kCurrencyOrbBase = 0x00081F;
    inline constexpr std::array<RE::FormID, Core::kCurrencyKindCount> kCurrencyOrbForms = {
        0x00081F,  // ScrollOfWisdom
        0x000820,  // Transmutation
        0x000821,  // Augmentation
        0x000822,  // Alteration
        0x000823,  // Regal
        0x000824,  // Alchemy
        0x000825,  // Chaos
        0x000826,  // Exalted
        0x000827,  // Scouring
        0x000828,  // Divine
        0x000829,  // Regret
        0x00082A,  // Jeweller
    };

    // 4단계(결정 157): 5티어 × 2희귀도(Magic/Rare) × 8부위군 = 80종 무상태 봉인 토큰 (0x00082B..0x00087A)
    inline constexpr RE::FormID kSealedTokenBase = 0x00082B;
    inline constexpr std::size_t kSealedTokenCount = 80;

    // 4단계(결정 159): 임시 빛기둥 마커 Static 2종 (0x00087B..0x00087C)
    inline constexpr RE::FormID kLootBeamRare = 0x00087B;
    inline constexpr RE::FormID kLootBeamCurrency = 0x00087C;

    // houseCARL 대조 완료 Skyrim.esm 드랍 효과음 SoundDescriptor
    inline constexpr RE::FormID kSfxLootRare = 0x000C8C71;      // UIEnchantingItemCreate
    inline constexpr RE::FormID kSfxLootHighOrb = 0x000ECF93;   // UISpellLearned

    struct DecodedTokenInfo
    {
        std::uint8_t tier = 1;  // 1..5
        Core::ItemRarity rarity = Core::ItemRarity::Magic;
        Core::TokenSlotGroup slotGroup = Core::TokenSlotGroup::OneHandWeapon;
    };

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

    [[nodiscard]] inline RE::FormID CurrencyLocalFormId(Core::CurrencyKind kind) noexcept
    {
        const auto idx = static_cast<std::size_t>(kind);
        if (idx >= kCurrencyOrbForms.size()) {
            return 0;
        }
        return kCurrencyOrbForms[idx];
    }

    [[nodiscard]] inline std::optional<Core::CurrencyKind> CurrencyKindFromLocalFormId(RE::FormID localId) noexcept
    {
        if (localId < kCurrencyOrbBase || localId >= kCurrencyOrbBase + kCurrencyOrbForms.size()) {
            return std::nullopt;
        }
        return static_cast<Core::CurrencyKind>(localId - kCurrencyOrbBase);
    }

    [[nodiscard]] inline RE::FormID SealedTokenLocalFormId(
        std::uint8_t tier,
        Core::ItemRarity rarity,
        Core::TokenSlotGroup slotGroup) noexcept
    {
        const std::uint8_t clampedTier = (tier < 1) ? 1 : (tier > Core::kItemTierCount ? Core::kItemTierCount : tier);
        const std::uint32_t rarityOffset = (rarity == Core::ItemRarity::Rare) ? 8u : 0u;
        const std::uint32_t slotOffset = static_cast<std::uint32_t>(slotGroup) % static_cast<std::uint32_t>(Core::kTokenSlotGroupCount);
        return kSealedTokenBase + static_cast<RE::FormID>((clampedTier - 1u) * 16u + rarityOffset + slotOffset);
    }

    [[nodiscard]] inline std::optional<DecodedTokenInfo> DecodeSealedTokenLocalFormId(RE::FormID localId) noexcept
    {
        if (localId < kSealedTokenBase || localId >= kSealedTokenBase + kSealedTokenCount) {
            return std::nullopt;
        }
        const std::uint32_t offset = static_cast<std::uint32_t>(localId - kSealedTokenBase);
        DecodedTokenInfo info;
        info.tier = static_cast<std::uint8_t>((offset / 16u) + 1u);
        const std::uint32_t rem = offset % 16u;
        info.rarity = (rem >= 8u) ? Core::ItemRarity::Rare : Core::ItemRarity::Magic;
        info.slotGroup = static_cast<Core::TokenSlotGroup>(rem % 8u);
        return info;
    }
}
