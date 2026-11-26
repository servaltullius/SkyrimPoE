#pragma once

#include "runtime/GemFormTables.generated.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace SkyrimPoE::Core::GemForms
{
    inline constexpr std::size_t kStage5GemStartIndex = 9;
    inline constexpr std::size_t kMaxStarterGrantedRecordIds = 256;

    struct StarterContentItem
    {
        std::string_view contentId;
        std::uint32_t localFormId = 0;

        bool operator==(const StarterContentItem&) const = default;
    };

    enum class StarterGrantStatus : std::uint8_t
    {
        Granted = 0,
        Failed = 1,
        DisabledByConfig = 2
    };

    struct StarterGrantEntry
    {
        std::string contentId;
        std::uint32_t localFormId = 0;
        StarterGrantStatus status = StarterGrantStatus::Failed;
        std::int32_t countDelta = 0;

        bool operator==(const StarterGrantEntry&) const = default;
    };

    struct StarterGrantReport
    {
        std::uint32_t generation = 0;
        bool configEnabled = true;
        std::vector<StarterGrantEntry> entries;

        [[nodiscard]] std::size_t CountByStatus(StarterGrantStatus status) const noexcept
        {
            return static_cast<std::size_t>(std::ranges::count(entries, status, &StarterGrantEntry::status));
        }

        [[nodiscard]] std::size_t GrantedCount() const noexcept { return CountByStatus(StarterGrantStatus::Granted); }
        [[nodiscard]] std::size_t FailedCount() const noexcept { return CountByStatus(StarterGrantStatus::Failed); }
        [[nodiscard]] std::size_t DisabledCount() const noexcept { return CountByStatus(StarterGrantStatus::DisabledByConfig); }

        bool operator==(const StarterGrantReport&) const = default;
    };

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

    // 7단계 G1(결정 240): 세대 1 시작 물건 = kContentKeys 중 ready인 항목(contentId, localFormId).
    [[nodiscard]] inline std::span<const StarterContentItem> ReadyStarterContent(std::uint32_t generation = 1) noexcept
    {
        static constexpr auto ready = [] {
            constexpr auto count = std::ranges::count(kContentKeys, true, &ContentGemKey::ready);
            std::array<StarterContentItem, count> items{};
            auto out = items.begin();
            for (const auto& entry : kContentKeys) {
                if (entry.ready) {
                    *out++ = StarterContentItem{ entry.contentId, entry.localFormId };
                }
            }
            return items;
        }();
        return generation == 1 ? std::span<const StarterContentItem>(ready) : std::span<const StarterContentItem>{};
    }

    // 준비된 시작 대상과 이미 지급 기록(STRT)에 있는 ID 목록을 대조해 아직 안 받은 ready 항목만 순서를 유지해 돌려준다.
    [[nodiscard]] inline std::vector<StarterContentItem> PendingStarterContent(
        std::span<const StarterContentItem> readyItems,
        std::span<const std::string> grantedIds)
    {
        std::vector<StarterContentItem> pending;
        pending.reserve(readyItems.size());
        for (const auto& item : readyItems) {
            if (item.contentId.empty()) {
                continue;
            }
            const bool alreadyGranted = std::ranges::find(grantedIds, item.contentId) != grantedIds.end();
            const bool alreadyPending = std::ranges::find(pending, item.contentId, &StarterContentItem::contentId) != pending.end();
            if (!alreadyGranted && !alreadyPending) {
                pending.push_back(item);
            }
        }
        return pending;
    }

    // 순수 시작 지급 실행기: 실물 지급 콜백이 수량 증가(1 이상)를 돌려줄 때만 grantedIds에 기록한다.
    // 2 이상은 실물이 들어간 것이므로 기록한다(기록하지 않으면 로드마다 다시 준다). 0 이하·없음·예외는 실패.
    template <class GrantOne>
    [[nodiscard]] StarterGrantReport ApplyStarterGrant(
        std::uint32_t generation,
        std::span<const StarterContentItem> readyItems,
        bool giveStarterGemsConfig,
        std::vector<std::string>& grantedIds,
        GrantOne&& grantOne)
    {
        StarterGrantReport report{ .generation = generation, .configEnabled = giveStarterGemsConfig };
        const auto pending = PendingStarterContent(readyItems, grantedIds);
        report.entries.reserve(pending.size());
        if (!giveStarterGemsConfig) {
            for (const auto& item : pending) {
                report.entries.push_back(StarterGrantEntry{
                    std::string(item.contentId), item.localFormId, StarterGrantStatus::DisabledByConfig, 0
                });
            }
            return report;
        }
        for (const auto& item : pending) {
            if (grantedIds.size() >= kMaxStarterGrantedRecordIds) {
                report.entries.push_back(StarterGrantEntry{
                    std::string(item.contentId), item.localFormId, StarterGrantStatus::Failed, 0
                });
                continue;
            }
            std::optional<std::int32_t> delta;
            try {
                delta = grantOne(item);
            } catch (...) {
                delta = std::nullopt;
            }
            if (delta && *delta >= 1) {
                grantedIds.emplace_back(item.contentId);
                report.entries.push_back(StarterGrantEntry{
                    std::string(item.contentId), item.localFormId, StarterGrantStatus::Granted, *delta
                });
            } else {
                report.entries.push_back(StarterGrantEntry{
                    std::string(item.contentId), item.localFormId, StarterGrantStatus::Failed, delta.value_or(0)
                });
            }
        }
        return report;
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
