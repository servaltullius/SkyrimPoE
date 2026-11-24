#include "test_harness.h"

#include "core/MapTravel.h"

#include <limits>

using namespace SkyrimPoE::Core;

POE_TEST("Ending map return can move when no requested actors remain")
{
    const auto empty = AdvanceMapSpawnDisableWait(0.0, 0.0, 0);
    POE_CHECK(empty.outcome == MapSpawnDisableWaitOutcome::Ready);

    const auto completedAtDeadline = AdvanceMapSpawnDisableWait(kMapSpawnDisableWaitTimeoutSec, 0.0, 0);
    POE_CHECK(completedAtDeadline.outcome == MapSpawnDisableWaitOutcome::Ready);
}

POE_TEST("Ending map return waits for all requested actors until the deadline")
{
    constexpr double halfTimeout = kMapSpawnDisableWaitTimeoutSec / 2.0;
    const auto partial = AdvanceMapSpawnDisableWait(0.0, halfTimeout, 2);
    POE_CHECK(partial.outcome == MapSpawnDisableWaitOutcome::Waiting);
    POE_CHECK(partial.elapsedSec == halfTimeout);

    constexpr double beforeDeadline = kMapSpawnDisableWaitTimeoutSec - 0.25;
    const auto oneRemaining = AdvanceMapSpawnDisableWait(partial.elapsedSec, beforeDeadline - halfTimeout, 1);
    POE_CHECK(oneRemaining.outcome == MapSpawnDisableWaitOutcome::Waiting);
    POE_CHECK(oneRemaining.elapsedSec == beforeDeadline);

    const auto timedOut = AdvanceMapSpawnDisableWait(oneRemaining.elapsedSec, 0.25, 1);
    POE_CHECK(timedOut.outcome == MapSpawnDisableWaitOutcome::TimedOut);
    POE_CHECK(timedOut.elapsedSec == kMapSpawnDisableWaitTimeoutSec);
}

POE_TEST("Ending map return keeps its disable wait time for invalid deltas")
{
    for (const double delta : { 0.0, -1.0, std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN() }) {
        constexpr double elapsed = kMapSpawnDisableWaitTimeoutSec / 2.0;
        const auto wait = AdvanceMapSpawnDisableWait(elapsed, delta, 1);
        POE_CHECK(wait.outcome == MapSpawnDisableWaitOutcome::Waiting);
        POE_CHECK(wait.elapsedSec == elapsed);
    }
}
