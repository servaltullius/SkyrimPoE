#pragma once

#include "core/ItemTypes.h"
#include "core/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace SkyrimPoE::Runtime::FormIds
{
    inline constexpr std::string_view kPluginName = "SkyrimPoE.esp";

    // houseCARL 생성 고정 FormID (esp/records.json, 0x800..0x8BF, 총 192폼 ESL)
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

    // 2단계(9종) + 5단계(19종) = 총 28종 스킬·보조·발동·오라·전령·가드 젬 통합 테이블
    inline constexpr std::array<GemFormEntry, 28> kAllGemForms = {{
        { "Cyclone",                          0x000814 },
        { "SupportCastOnCriticalStrike",      0x000815 },
        { "IceNova",                          0x000816 },
        { "ShockNova",                        0x000817 },
        { "SupportIncreasedCriticalStrikes",  0x000818 },
        { "SupportMeleePhysicalDamage",       0x000819 },
        { "SupportBrutality",                 0x00081A },
        { "SupportIncreasedAreaOfEffect",     0x00081B },
        { "SupportConcentratedEffect",        0x00081C },
        // 5단계 신규 19종 (0x00087D..0x00088F)
        { "Fireball",                         0x00087D },
        { "FreezingPulse",                    0x00087E },
        { "Arc",                              0x00087F },
        { "Spark",                            0x000880 },
        { "SupportGreaterMultipleProjectiles",0x000881 },
        { "SupportChain",                     0x000882 },
        { "SupportFork",                      0x000883 },
        { "SupportPierce",                    0x000884 },
        { "SupportSpellEcho",                 0x000885 },
        { "Determination",                    0x000886 },
        { "Grace",                            0x000887 },
        { "Discipline",                       0x000888 },
        { "Hatred",                           0x000889 },
        { "Wrath",                            0x00088A },
        { "Anger",                            0x00088B },
        { "PurityOfElements",                 0x00088C },
        { "HeraldOfIce",                      0x00088D },
        { "HeraldOfAsh",                      0x00088E },
        { "MoltenShell",                      0x00088F },
    }};

    // 2단계 호출부 호환 별칭 및 5단계 신규 젬 시작 인덱스(0-based, 총 19종)
    inline constexpr const auto& kStage2GemForms = kAllGemForms;
    inline constexpr std::size_t kStage5GemStartIndex = 9;
    inline constexpr std::size_t kStage5GemCount = kAllGemForms.size() - kStage5GemStartIndex;

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

    // 5단계(결정 162, 163, 164): 벨트 5종, 벨트 봉인 토큰 10종, 주얼 3종, 참 6종, 투사체 3종, 화염 폭발 VFX 1종, 고유 토큰/빛기둥
    inline constexpr RE::FormID kBeltBaseStart = 0x000890;
    inline constexpr std::array<RE::FormID, 5> kBeltBaseForms = {
        0x000890,  // LeatherBelt
        0x000891,  // HeavyBelt
        0x000892,  // ChainBelt
        0x000893,  // RusticSash
        0x000894,  // StygianVise
    };

    inline constexpr RE::FormID kBeltTokenBase = 0x000895;  // 10종: T1..T5 × (Magic, Rare)
    inline constexpr std::size_t kBeltTokenCount = 10;

    inline constexpr RE::FormID kJewelBaseStart = 0x00089F;
    inline constexpr std::array<RE::FormID, 3> kJewelBaseForms = {
        0x00089F,  // Crimson Jewel (0)
        0x0008A0,  // Viridian Jewel (1)
        0x0008A1,  // Cobalt Jewel (2)
    };

    inline constexpr RE::FormID kCharmBaseStart = 0x0008A2;
    inline constexpr std::array<RE::FormID, Core::kCharmKindCount> kCharmForms = {
        0x0008A2,  // Ruby
        0x0008A3,  // Sapphire
        0x0008A4,  // Topaz
        0x0008A5,  // Amethyst
        0x0008A6,  // Purifying
        0x0008A7,  // Bastion
    };

    inline constexpr RE::FormID kProjFireball = 0x0008A8;
    inline constexpr RE::FormID kProjFreezingPulse = 0x0008A9;
    inline constexpr RE::FormID kProjSpark = 0x0008AA;
    inline constexpr RE::FormID kVfxFireBurst = 0x0008AB;
    inline constexpr RE::FormID kUniqueSealedToken = 0x0008AC;
    inline constexpr RE::FormID kLootBeamUnique = 0x0008AD;

    // 6단계(결정 166): 지도 아이템 T1..T16 (0x0008AE..0x0008BD), 4차 전직 공물 (0x0008BE), 지도 빛기둥 (0x0008BF)
    inline constexpr RE::FormID kMapItemBase = 0x0008AE;
    inline constexpr std::size_t kMapItemCount = 16;
    inline constexpr std::array<RE::FormID, kMapItemCount> kMapItemForms = {
        0x0008AE, 0x0008AF, 0x0008B0, 0x0008B1,
        0x0008B2, 0x0008B3, 0x0008B4, 0x0008B5,
        0x0008B6, 0x0008B7, 0x0008B8, 0x0008B9,
        0x0008BA, 0x0008BB, 0x0008BC, 0x0008BD,
    };
    inline constexpr RE::FormID kAscendancy4Tribute = 0x0008BE;
    inline constexpr RE::FormID kLootBeamMap = 0x0008BF;

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
        for (const auto& entry : kAllGemForms) {
            if (entry.gemId == gemId) {
                return entry.localFormId;
            }
        }
        return 0;
    }

    [[nodiscard]] inline std::string_view FindGemIdForLocalFormId(RE::FormID localId) noexcept
    {
        for (const auto& entry : kAllGemForms) {
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

    [[nodiscard]] inline bool IsBeltBaseLocalFormId(RE::FormID localId) noexcept
    {
        return localId >= kBeltBaseStart && localId < kBeltBaseStart + kBeltBaseForms.size();
    }

    [[nodiscard]] inline std::optional<std::uint8_t> JewelColorFromLocalFormId(RE::FormID localId) noexcept
    {
        if (localId < kJewelBaseStart || localId >= kJewelBaseStart + kJewelBaseForms.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(localId - kJewelBaseStart);
    }

    [[nodiscard]] inline RE::FormID JewelLocalFormId(std::uint8_t color) noexcept
    {
        return kJewelBaseForms[color < 3 ? color : 0];
    }

    [[nodiscard]] inline RE::FormID CharmLocalFormId(Core::CharmKind kind) noexcept
    {
        const auto idx = static_cast<std::size_t>(kind);
        return (idx < kCharmForms.size()) ? kCharmForms[idx] : 0;
    }

    [[nodiscard]] inline std::optional<Core::CharmKind> CharmKindFromLocalFormId(RE::FormID localId) noexcept
    {
        if (localId < kCharmBaseStart || localId >= kCharmBaseStart + kCharmForms.size()) {
            return std::nullopt;
        }
        return static_cast<Core::CharmKind>(localId - kCharmBaseStart);
    }

    [[nodiscard]] inline RE::FormID MapItemLocalFormId(std::uint8_t tier) noexcept
    {
        const std::uint8_t clamped = (tier < 1u) ? 1u : (tier > 16u ? 16u : tier);
        return kMapItemForms[clamped - 1u];
    }

    [[nodiscard]] inline std::optional<std::uint8_t> MapTierFromLocalFormId(RE::FormID localId) noexcept
    {
        if (localId < kMapItemBase || localId >= kMapItemBase + kMapItemCount) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>((localId - kMapItemBase) + 1u);
    }

    [[nodiscard]] inline bool IsMapItemLocalFormId(RE::FormID localId) noexcept
    {
        return MapTierFromLocalFormId(localId).has_value();
    }

    [[nodiscard]] inline bool IsAscendancy4TributeLocalFormId(RE::FormID localId) noexcept
    {
        return localId == kAscendancy4Tribute;
    }

    [[nodiscard]] inline RE::FormID SealedTokenLocalFormId(
        std::uint8_t tier,
        Core::ItemRarity rarity,
        Core::TokenSlotGroup slotGroup) noexcept
    {
        if (static_cast<std::size_t>(slotGroup) >= Core::kDroppableTokenSlotGroupCount) {
            return 0;
        }
        const std::uint8_t clampedTier = (tier < 1) ? 1 : (tier > Core::kItemTierCount ? Core::kItemTierCount : tier);
        if (slotGroup == Core::TokenSlotGroup::Belt) {
            const std::uint32_t rarOff = (rarity == Core::ItemRarity::Rare) ? 1u : 0u;
            return kBeltTokenBase + static_cast<RE::FormID>((clampedTier - 1u) * 2u + rarOff);
        }
        const std::uint32_t rarityOffset = (rarity == Core::ItemRarity::Rare) ? 8u : 0u;
        const std::uint32_t slotOffset = static_cast<std::uint32_t>(slotGroup) % static_cast<std::uint32_t>(Core::kStandardTokenSlotGroupCount);
        return kSealedTokenBase + static_cast<RE::FormID>((clampedTier - 1u) * 16u + rarityOffset + slotOffset);
    }

    [[nodiscard]] inline std::optional<DecodedTokenInfo> DecodeSealedTokenLocalFormId(RE::FormID localId) noexcept
    {
        if (localId >= kBeltTokenBase && localId < kBeltTokenBase + kBeltTokenCount) {
            const std::uint32_t off = static_cast<std::uint32_t>(localId - kBeltTokenBase);
            DecodedTokenInfo info;
            info.tier = static_cast<std::uint8_t>((off / 2u) + 1u);
            info.rarity = ((off % 2u) == 1u) ? Core::ItemRarity::Rare : Core::ItemRarity::Magic;
            info.slotGroup = Core::TokenSlotGroup::Belt;
            return info;
        }
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

    // 주어진 전체 runtime FormID가 SkyrimPoE.esp 소속이면 로컬 FormID를, 아니면 0을 반환
    [[nodiscard]] inline RE::FormID ResolveLocalFormIdIfOurs(RE::FormID runtimeFormId) noexcept
    {
        auto* handler = RE::TESDataHandler::GetSingleton();
        if (!handler || runtimeFormId == 0) {
            return 0;
        }
        const auto* file = handler->LookupModByName(kPluginName);
        if (!file) {
            return 0;
        }
        if (file->IsLight()) {
            const std::uint32_t expectedPrefix = 0xFE000000u | (static_cast<std::uint32_t>(file->GetSmallFileCompileIndex()) << 12u);
            if ((runtimeFormId & 0xFFFFF000u) == expectedPrefix) {
                return runtimeFormId & 0x00000FFFu;
            }
        } else {
            const std::uint32_t expectedPrefix = static_cast<std::uint32_t>(file->GetCompileIndex()) << 24u;
            if ((runtimeFormId & 0xFF000000u) == expectedPrefix) {
                return runtimeFormId & 0x00FFFFFFu;
            }
        }
        return 0;
    }
}
