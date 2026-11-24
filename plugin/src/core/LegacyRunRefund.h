#pragma once

namespace SkyrimPoE::Core
{
    enum class LegacyBossObservation { Unknown, Alive, Dead };
    enum class LegacyBossDecisionSource { CommitLookup, LoadConfirmation, MapsRecord };

    struct LegacyRunRefundDecision
    {
        bool refund;
        LegacyBossDecisionSource source;
    };

    [[nodiscard]] constexpr LegacyRunRefundDecision DecideLegacyRunRefund(
        LegacyBossObservation atConfirmation, LegacyBossObservation atCommit, bool bossKilled, bool failed) noexcept
    {
        using enum LegacyBossObservation;
        using enum LegacyBossDecisionSource;

        // A death observed at either point vetoes a refund.
        if (atCommit == Dead) {
            return { false, CommitLookup };
        }
        if (atConfirmation == Dead) {
            return { false, LoadConfirmation };
        }
        const bool refund = !bossKilled && !failed;
        if (atCommit == Alive) {
            return { refund, CommitLookup };
        }
        if (atConfirmation == Alive) {
            return { refund, LoadConfirmation };
        }
        return { refund, MapsRecord };
    }
}
