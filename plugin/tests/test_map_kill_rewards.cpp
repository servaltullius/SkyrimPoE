#include "test_harness.h"

#include "core/KillTracker.h"
#include "core/MapSystem.h"

#include <cstdint>
#include <optional>

using namespace SkyrimPoE::Core;

POE_TEST("Map kill rewards require a qualified kill in an authorized non-failed run")
{
    POE_CHECK(MapSystem::IsMapKillRewardEligible(true, false, false));
    POE_CHECK(!MapSystem::IsMapKillRewardEligible(false, false, false));
    POE_CHECK(!MapSystem::IsMapKillRewardEligible(true, true, false));
    POE_CHECK(!MapSystem::IsMapKillRewardEligible(true, false, true));
    POE_CHECK(!MapSystem::IsMapKillRewardEligible(false, true, true));
}

POE_TEST("Arena4 clear requires contribution, completed phase three, and expired immunity")
{
    for (std::uint8_t completedPhase = 1; completedPhase <= 3; ++completedPhase) {
        for (const bool invulnerable : { false, true }) {
            for (const bool eligible : { false, true }) {
                POE_CHECK(MapSystem::IsArena4ClearEligible(eligible, { completedPhase, invulnerable }) ==
                    (eligible && completedPhase == 3 && !invulnerable));
            }
        }
    }
}

POE_TEST("Arena4 entry checks all three prerequisite bits and prioritizes the cleared reason")
{
    constexpr int completedByMask[] = { 0, 1, 1, 2, 1, 2, 2, 3 };
    for (std::uint32_t mask = 0; mask <= 0xFu; ++mask) {
        const auto entry = MapSystem::GetArena4EntryStatus(mask);
        POE_CHECK(entry.milestonesCompleted == completedByMask[mask & 0x7u]);
        POE_CHECK(entry.CanEnter() == (mask == 0x7u));
        const auto denial = mask >= 0x8u ? Arena4EntryDenial::AlreadyCleared :
            (mask == 0x7u ? Arena4EntryDenial::None : Arena4EntryDenial::MissingMilestones);
        POE_CHECK(entry.denial == denial);
    }
    POE_CHECK(MapSystem::GetArena4EntryStatus(0x17u).CanEnter());
}

POE_TEST("An incomplete Arena4 transition blocks even lethal damage after its timer expires")
{
    const auto pending = MapSystem::IsArena4PhaseInvulnerable(kArena4FinalPhase, kArena4FinalPhase - 1u, 0.0);
    const auto hit = MapSystem::ClampArenaBossDamage(kArena4FinalPhase, pending, 30.0, 100.0, 100.0);
    POE_CHECK(pending && hit.blockedByInvuln && hit.clampedDamage == 0.0 && hit.triggeredNextPhase == 0u);
    POE_CHECK(MapSystem::IsArena4PhaseInvulnerable(kArena4FinalPhase, kArena4FinalPhase, 0.1));
    POE_CHECK(!MapSystem::IsArena4PhaseInvulnerable(kArena4FinalPhase, kArena4FinalPhase, 0.0));
}

POE_TEST("Arena4 saves the lesser phase and retries an unfinished transition on the next hit after loading")
{
    for (std::uint8_t requested = kArena4FirstPhase; requested <= kArena4FinalPhase; ++requested) {
        for (std::uint8_t completed = kArena4FirstPhase; completed <= kArena4FinalPhase; ++completed) {
            POE_CHECK(MapSystem::Arena4PhaseForSave(requested, completed) == (requested < completed ? requested : completed));
        }
    }

    for (std::uint8_t requested = kArena4FirstPhase + 1u; requested <= kArena4FinalPhase; ++requested) {
        const auto saved = MapSystem::Arena4PhaseForSave(requested, static_cast<std::uint8_t>(requested - 1u));
        const auto immune = MapSystem::IsArena4PhaseInvulnerable(saved, saved, 0.0);
        const auto hit = MapSystem::ClampArenaBossDamage(saved, immune, 25.0, 100.0, 1.0);
        POE_CHECK(!hit.blockedByInvuln && hit.triggeredNextPhase == requested && hit.clampedDamage == 0.0);
    }
}

POE_TEST("Arena4 unlock rechecks prerequisites for victories in runs restored from older saves")
{
    constexpr std::uint32_t unrelatedBit = 0x10u;
    for (std::uint32_t prerequisites = 0; prerequisites < kArena4PrerequisiteMask; ++prerequisites) {
        auto milestones = prerequisites | unrelatedBit;
        POE_CHECK(MapSystem::TryUnlockArena4(milestones) == Arena4EntryDenial::MissingMilestones);
        POE_CHECK(milestones == (prerequisites | unrelatedBit));
    }

    auto milestones = kArena4PrerequisiteMask | unrelatedBit;
    POE_CHECK(MapSystem::TryUnlockArena4(milestones) == Arena4EntryDenial::None);
    POE_CHECK(milestones == (kArena4PrerequisiteMask | kArena4MilestoneBit | unrelatedBit));
    POE_CHECK(MapSystem::TryUnlockArena4(milestones) == Arena4EntryDenial::AlreadyCleared);
}

POE_TEST("A hostile kill qualifies with player damage before a finishing blow from another source")
{
    constexpr std::uint32_t actor = 1;
    KillTracker tracker;
    const Config config;
    tracker.RecordFirstHitHostility(actor, true);
    tracker.ConfirmDamageContribution(actor, 10.0);
    // 막타 시점에 플레이어의 직접 피해 진행 표시는 없다.
    tracker.OnDeathStart(actor, false, false, false);
    const auto reward = tracker.OnDeathConfirm(actor, 1, 1, true, false, 1, config);
    POE_CHECK(reward.playerContributed && reward.awarded);
}

POE_TEST("Synchronous death during direct damage awards a hostile kill once")
{
    constexpr std::uint32_t actor = 1;
    KillTracker tracker;
    const Config config;
    tracker.RecordFirstHitHostility(actor, true);
    {
        const KillTracker::DirectHitScope hit(&tracker, actor);
        tracker.OnDeathStart(actor, false, false, false);
        const auto reward = tracker.OnDeathConfirm(actor, 1, 1, false, false, 1, config);
        POE_CHECK(reward.awarded);
        POE_CHECK(reward.playerContributed);
        POE_CHECK(!tracker.OnDeathConfirm(actor, 1, 1, false, false, 1, config).awarded);
    }
}

POE_TEST("Direct damage scope preserves hostility and exclusion rules and unwinds after an exception")
{
    constexpr std::uint32_t neutral = 1;
    constexpr std::uint32_t summoned = 2;
    constexpr std::uint32_t survived = 3;
    KillTracker tracker;
    const Config config;
    tracker.RecordFirstHitHostility(neutral, false);
    tracker.RecordFirstHitHostility(summoned, true);
    tracker.RecordFirstHitHostility(survived, true);

    const KillTracker::DirectHitScope neutralHit(&tracker, neutral);
    POE_CHECK(!tracker.OnDeathConfirm(neutral, 1, 1, false, false, 1, config).awarded);
    const KillTracker::DirectHitScope summonedHit(&tracker, summoned);
    tracker.OnDeathStart(summoned, true, false, false);
    POE_CHECK(!tracker.OnDeathConfirm(summoned, 1, 1, false, false, 1, config).awarded);
    // 모델링된 예외 뒤 동료가 처치해도 이전 피해 진행 표시가 남아 있지 않다.
    struct DamageError {};
    try {
        const KillTracker::DirectHitScope hit(&tracker, survived);
        throw DamageError{};
    } catch (const DamageError&) {
    }
    POE_CHECK(!tracker.OnDeathConfirm(survived, 1, 1, false, false, 1, config).awarded);
}

POE_TEST("Ending an old direct hit after actor reset preserves the new life's active hit")
{
    constexpr std::uint32_t actor = 1;
    KillTracker tracker;
    const Config config;
    tracker.RecordFirstHitHostility(actor, true);
    std::optional<KillTracker::DirectHitScope> oldHit;
    oldHit.emplace(&tracker, actor);
    tracker.OnActorReset(actor);
    tracker.RecordFirstHitHostility(actor, true);
    const KillTracker::DirectHitScope newHit(&tracker, actor);
    {
        const KillTracker::DirectHitScope innerHit(&tracker, actor);
    }
    oldHit.reset();
    POE_CHECK(tracker.OnDeathConfirm(actor, 1, 1, false, false, 1, config).awarded);
}
