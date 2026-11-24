#include "test_harness.h"

#include "core/LegacyRunRefund.h"

#include <array>

using namespace SkyrimPoE::Core;

POE_TEST("Legacy run refund: both boss observations and both MAPS flags")
{
    using enum LegacyBossObservation;
    using enum LegacyBossDecisionSource;
    struct BossCase
    {
        LegacyBossObservation confirmation;
        LegacyBossObservation commit;
        bool refundIfFlagsClear;
        LegacyBossDecisionSource source;
    };
    constexpr std::array cases{
        BossCase{ Unknown, Unknown, true, MapsRecord },
        BossCase{ Unknown, Alive, true, CommitLookup },
        BossCase{ Unknown, Dead, false, CommitLookup },
        BossCase{ Alive, Unknown, true, LoadConfirmation },
        BossCase{ Alive, Alive, true, CommitLookup },
        BossCase{ Alive, Dead, false, CommitLookup },
        BossCase{ Dead, Unknown, false, LoadConfirmation },
        BossCase{ Dead, Alive, false, LoadConfirmation },
        BossCase{ Dead, Dead, false, CommitLookup },
    };
    for (const auto& test : cases) {
        for (const bool bossKilled : { false, true }) {
            for (const bool failed : { false, true }) {
                const auto result = DecideLegacyRunRefund(test.confirmation, test.commit, bossKilled, failed);
                POE_CHECK(result.refund == (test.refundIfFlagsClear && !bossKilled && !failed));
                POE_CHECK(result.source == test.source);
            }
        }
    }
}
