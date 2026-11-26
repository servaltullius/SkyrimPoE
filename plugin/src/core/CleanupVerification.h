#pragma once

#include "core/GemForms.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace SkyrimPoE::Core
{
    // A10: 세대 1 G1 시점 독립 기대 시작 물건 표의 단일 출처(생산 코드 ReadyStarterContent/StarterForms와 분리).
    struct Gen1StarterExpectation
    {
        std::string_view contentId;
        std::uint32_t localFormId = 0;
        std::int32_t count = 1;
    };

    inline constexpr std::array<Gen1StarterExpectation, 1> kGen1StarterExpectations{{
        { "gem.Spark", 0x000880u, 1 }
    }};

    struct CleanupItemExpectationInput
    {
        std::size_t plannedSlottedSpark = 0;
        std::size_t plannedOldChain = 0;
        bool giveStarterGems = true;
        std::int32_t sparkPre = 0;
        std::int32_t sparkActual = 0;
        std::size_t sparkLost = 0;
        std::int32_t oldChainPre = 0;
        std::int32_t oldChainActual = 0;
        std::size_t oldChainLost = 0;
        std::int32_t newChainPre = 0;
        std::int32_t newChainActual = 0;
        std::size_t newChainLost = 0;
        GemForms::StarterGrantReport starterReport;
    };

    struct CleanupItemExpectationResult
    {
        bool ok = false;
        bool inputRequirementMet = false;
        std::int32_t expectedSparkDelta = 0;
        std::int32_t expectedOldChainDelta = 0;
        std::int32_t expectedNewChainDelta = 0;
        std::int32_t expectedStarterSparkDelta = 0;
        std::int32_t actualStarterSparkDelta = 0;
        bool sparkCountOk = false;
        bool oldChainCountOk = false;
        bool newChainCountOk = false;
        bool zeroLossOk = false;
        bool starterGrantOk = false;
        std::string summary;
    };

    [[nodiscard]] inline std::vector<std::string> ExpectedGen1StarterIdsForConfig(bool giveStarterGems)
    {
        std::vector<std::string> ids;
        if (!giveStarterGems) {
            return ids;
        }
        ids.reserve(kGen1StarterExpectations.size());
        for (const auto& item : kGen1StarterExpectations) {
            ids.emplace_back(item.contentId);
        }
        return ids;
    }

    // F6: cleanup C3 독립 기대값 판정(제품 returnedByForm에 의존하지 않고 입력 계획·설정·손실·시작 지급 ID별 증분을 따로 검증).
    [[nodiscard]] inline CleanupItemExpectationResult EvaluateCleanupItemExpectation(
        const CleanupItemExpectationInput& in)
    {
        CleanupItemExpectationResult out;
        // 소켓 구판 Spark 수 N은 입력마다 다르다(지금 시험 세이브는 0). Spark(0x880)는 구판 반환 N + 시작 지급 몫.
        out.inputRequirementMet = (in.plannedOldChain >= 1);
        out.expectedStarterSparkDelta = in.giveStarterGems ? 1 : 0;
        out.expectedSparkDelta = static_cast<std::int32_t>(in.plannedSlottedSpark) + out.expectedStarterSparkDelta;
        out.expectedOldChainDelta = static_cast<std::int32_t>(in.plannedOldChain);
        out.expectedNewChainDelta = 0;

        out.sparkCountOk = (in.sparkActual == in.sparkPre + out.expectedSparkDelta);
        out.oldChainCountOk = (in.oldChainActual == in.oldChainPre + out.expectedOldChainDelta);
        out.newChainCountOk = (in.newChainActual == in.newChainPre + out.expectedNewChainDelta);
        out.zeroLossOk = (in.sparkLost == 0 && in.oldChainLost == 0 && in.newChainLost == 0);

        std::size_t sparkEntries = 0;
        bool otherEntriesOk = true;
        for (const auto& entry : in.starterReport.entries) {
            if (entry.contentId == "gem.Spark") {
                ++sparkEntries;
                out.actualStarterSparkDelta += entry.countDelta;
                if (in.giveStarterGems) {
                    otherEntriesOk &= (entry.status == GemForms::StarterGrantStatus::Granted && entry.countDelta == 1);
                } else {
                    otherEntriesOk &= (entry.status == GemForms::StarterGrantStatus::DisabledByConfig && entry.countDelta == 0);
                }
            } else {
                otherEntriesOk = false;
            }
        }
        const bool reportCountsOk = in.giveStarterGems
            ? (in.starterReport.GrantedCount() == 1 && in.starterReport.FailedCount() == 0 && in.starterReport.DisabledCount() == 0)
            : (in.starterReport.GrantedCount() == 0 && in.starterReport.FailedCount() == 0 && in.starterReport.DisabledCount() == 1);
        out.starterGrantOk = (sparkEntries == 1) &&
            (out.actualStarterSparkDelta == out.expectedStarterSparkDelta) &&
            otherEntriesOk && reportCountsOk;

        out.ok = out.inputRequirementMet && out.sparkCountOk && out.oldChainCountOk &&
            out.newChainCountOk && out.zeroLossOk && out.starterGrantOk;

        out.summary = std::format(
            "GiveStarterGems={}, Spark(0x880)[pre={}, indepExp={} (+{}), actual={}, lost={}], "
            "oldChain(0x882)[pre={}, planned={}, indepExp={} (+{}), actual={}, lost={}], "
            "newChainI(0x8D2)[pre={}, indepExp={} (+0), actual={}, lost={}], "
            "starterSparkDelta={}/{} (ok={})",
            in.giveStarterGems,
            in.sparkPre, in.sparkPre + out.expectedSparkDelta, out.expectedSparkDelta, in.sparkActual, in.sparkLost,
            in.oldChainPre, in.plannedOldChain, in.oldChainPre + out.expectedOldChainDelta, out.expectedOldChainDelta,
            in.oldChainActual, in.oldChainLost,
            in.newChainPre, in.newChainPre, in.newChainActual, in.newChainLost,
            out.actualStarterSparkDelta, out.expectedStarterSparkDelta, out.starterGrantOk);
        return out;
    }
}
