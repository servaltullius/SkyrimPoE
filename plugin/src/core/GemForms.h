#pragma once

#include "runtime/GemFormTables.generated.h"

#include <algorithm>
#include <span>

namespace SkyrimPoE::Core::GemForms
{
    inline constexpr std::size_t kStage5GemStartIndex = 9;

    [[nodiscard]] constexpr const GemForm* FindPhysical(std::uint32_t localId) noexcept
    {
        const auto it = std::ranges::find(kPhysicalForms, localId, &GemForm::localFormId);
        return it == kPhysicalForms.end() ? nullptr : &*it;
    }

    [[nodiscard]] constexpr const LegacyGemKey* FindLegacy(std::string_view gemId) noexcept
    {
        const auto it = std::ranges::find(kLegacyKeys, gemId, &LegacyGemKey::gemId);
        return it == kLegacyKeys.end() ? nullptr : &*it;
    }

    [[nodiscard]] constexpr const ContentGemKey* FindContent(std::string_view contentId) noexcept
    {
        const auto it = std::ranges::find(kContentKeys, contentId, &ContentGemKey::contentId);
        return it == kContentKeys.end() ? nullptr : &*it;
    }

    [[nodiscard]] constexpr std::span<const LegacyGemKey> StarterForms(
        std::uint32_t generation, bool stage5Only = false) noexcept
    {
        if (generation != 0) return {};
        const std::span<const LegacyGemKey> forms = kLegacyKeys;
        return stage5Only ? forms.subspan(kStage5GemStartIndex) : forms;
    }

    [[nodiscard]] inline std::span<const std::string_view> DropIds(std::uint32_t generation) noexcept
    {
        static constexpr auto legacy = [] {
            std::array<std::string_view, kLegacyKeys.size() - kStage5GemStartIndex> ids{};
            std::ranges::transform(StarterForms(0, true), ids.begin(), &LegacyGemKey::gemId);
            return ids;
        }();
        static constexpr auto ready = [] {
            constexpr auto count = std::ranges::count(kContentKeys, true, &ContentGemKey::ready);
            std::array<std::string_view, count> ids{};
            auto out = ids.begin();
            for (const auto& entry : kContentKeys) {
                if (entry.ready) *out++ = entry.contentId;
            }
            return ids;
        }();
        if (generation == 0) return legacy;
        if (generation == 1) return ready;
        return {};
    }

    // Cleanup always interprets the source save's unprefixed keys.
    [[nodiscard]] constexpr std::uint32_t LegacyReturnForm(std::string_view gemId) noexcept
    {
        const auto* entry = FindLegacy(gemId);
        return entry ? entry->localFormId : 0;
    }

    [[nodiscard]] constexpr std::uint32_t ActiveForm(std::uint32_t generation, std::string_view gemId) noexcept
    {
        if (generation == 0) return LegacyReturnForm(gemId);
        const auto* entry = generation == 1 ? FindContent(gemId) : nullptr;
        return entry && entry->ready ? entry->localFormId : 0;
    }

    [[nodiscard]] constexpr bool IsRecoveryForm(std::uint32_t generation, const GemForm& form) noexcept
    {
        return (generation == 0 && !form.legacyId.empty()) || generation == 1;
    }

    [[nodiscard]] constexpr std::uint32_t RecoveryForm(std::uint32_t generation, std::string_view gemId) noexcept
    {
        if (generation == 1) {
            if (const auto* entry = FindContent(gemId)) return entry->localFormId;
        }
        return generation <= 1 ? LegacyReturnForm(gemId) : 0;
    }

    template <class Observe>
    void ObservePhysicalForms(Observe&& observe)
    {
        for (const auto& form : kPhysicalForms) {
            observe(form);
        }
    }

    [[nodiscard]] constexpr std::string_view InventoryId(std::uint32_t generation, const GemForm& form) noexcept
    {
        if (generation == 0) return form.legacyId;
        return generation == 1 && !form.contentId.empty() ? form.contentId : form.legacyId;
    }

    enum class GemUseReason { Unknown, Available, Legacy, NotReady };

    struct GemUse
    {
        bool recognized = false;
        GemUseReason reason = GemUseReason::Unknown;
        bool equip = false;
        bool socket = false;
        bool execute = false;
    };

    [[nodiscard]] constexpr GemUse ClassifyUse(std::uint32_t generation, std::string_view gemId) noexcept
    {
        if (generation == 0 && FindLegacy(gemId)) {
            const bool support = gemId.starts_with("Support") && gemId != "SupportCastOnCriticalStrike";
            return { true, GemUseReason::Available, !support, true, !support };
        }
        if (generation == 1) {
            if (const auto* entry = FindContent(gemId)) {
                const bool active = entry->execution != GemExecution::S11;
                return { true, entry->ready ? GemUseReason::Available : GemUseReason::NotReady,
                    active && entry->ready, entry->ready, active && entry->ready };
            }
            if (FindLegacy(gemId)) return { true, GemUseReason::Legacy };
        }
        return {};
    }

    [[nodiscard]] constexpr std::string_view UseReasonText(GemUseReason reason) noexcept
    {
        switch (reason) {
        case GemUseReason::Available: return {};
        case GemUseReason::Legacy: return "구판 PoE1 젬입니다.";
        case GemUseReason::NotReady: return "PoE2 젬: 아직 구현 안 됨";
        case GemUseReason::Unknown: return "알 수 없는 젬입니다.";
        }
        return {};
    }

    // Generation-zero execution retains its exact ID dispatch. Generation one gates by readiness.
    [[nodiscard]] constexpr bool AllowsExecution(std::uint32_t generation, std::string_view gemId) noexcept
    {
        return generation == 0 || ClassifyUse(generation, gemId).execute;
    }
}
