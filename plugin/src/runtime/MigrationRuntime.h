#pragma once

#include "core/MapSave.h"
#include "core/Migration.h"
#include "runtime/MapDeviceRuntime.h"

#include <chrono>
#include <map>

namespace SkyrimPoE::Runtime
{
    enum class MigrationSpeedResult { Reverted, AlreadyZero, Deferred, Lost };

    struct MigrationEngineResidue
    {
        float variable09 = 0.0f;
        std::size_t proxySpells = 0;
        std::size_t proxyPowers = 0;

        [[nodiscard]] bool Empty() const noexcept { return variable09 == 0.0f && proxySpells == 0 && proxyPowers == 0; }
    };

    struct MigrationDiagnostics
    {
        Core::CleanupStep step = Core::CleanupStep::Begin;
        std::string candidateReport;
        std::size_t damagedRecords = 0;
        std::array<std::size_t, 4> returned{};
        std::array<std::size_t, 4> lostReturns{};
        std::map<RE::FormID, std::size_t> lostReturnsByForm;
        std::size_t lostReturnsWithoutForm = 0;
        std::uint32_t lostCount = 0;
        double healthDelta = 0.0;
        double magickaDelta = 0.0;
        double healthBefore = 0.0;
        double healthAfter = 0.0;
        double magickaBefore = 0.0;
        double magickaAfter = 0.0;
        MigrationEngineResidue engineResidue;
        bool engineRevertMismatch = false;
        std::array<std::size_t, 4> speedResults{};
        std::uint32_t speedInputCount = 0;
        std::uint32_t speedModsFiltered = 0;
        std::uint32_t speedModsUnresolved = 0;
        std::array<std::size_t, 4> dropResults{};
        enum class MapResult { None, Closed, Partial };
        MapResult map = MapResult::None;
        LegacyRunCloseResult mapClose;
        bool starterGranted = false;
        std::uint32_t tickThreadId = 0;

        [[nodiscard]] std::string Describe() const;
        [[nodiscard]] std::string_view MapResultName() const noexcept;
    };

    struct MigrationCandidateSnapshot
    {
        std::string report;
        std::vector<Core::ReturnEntry> returns;
        double healthDelta = 0.0;
        double magickaDelta = 0.0;
        bool hasMapRun = false;
        bool inMapCell = false;
    };

    // SaveLoad serializes access with the co-save lock. Engine work runs on the main update thread.
    class MigrationRuntime
    {
    public:
        enum class Phase { None, AwaitingLoad, Escaping, Ready };

        [[nodiscard]] std::string Prepare(const Core::SaveRecordBatch& batch, const Core::SaveGeneration& target,
            const Core::FormIdResolver& resolve, std::uint64_t world);
        void Confirm(std::uint64_t world, RE::PlayerCharacter* player);
        void Reset();
        void ReleaseForTest() noexcept { releasedForTest_ = true; }
        [[nodiscard]] bool IsReleasedForTest() const noexcept { return releasedForTest_; }
        [[nodiscard]] Phase GetPhase() const noexcept { return phase_; }
        [[nodiscard]] bool IsForWorld(std::uint64_t world) const noexcept { return world_ == world; }
        [[nodiscard]] MigrationCandidateSnapshot Snapshot(RE::PlayerCharacter* player) const;

        // Returns a reason if escape timed out. The commit gate is checked separately.
        [[nodiscard]] std::string_view TickEscape(RE::PlayerCharacter* player, bool eligible);
        [[nodiscard]] std::string_view CheckCommitGate(RE::PlayerCharacter* player) const;
        [[nodiscard]] MigrationDiagnostics Execute(RE::PlayerCharacter* player, RE::ActorValueOwner& values,
            const std::function<MigrationSpeedResult(const Core::MigrationSpeedMod&)>& speed,
            const std::function<void()>& clearCosave);

        [[nodiscard]] static bool CanTick(RE::PlayerCharacter* player);
        [[nodiscard]] static RE::TESObjectMISC* ReturnForm(const Core::ReturnEntry& entry);
        [[nodiscard]] static MigrationEngineResidue ObserveEngineResidue(RE::PlayerCharacter* player);

    private:
        Core::LegacyMigrationResult candidate_;
        std::optional<Core::ActiveMapState> run_;
        RE::FormID legacyCell_ = 0;
        Phase phase_ = Phase::None;
        std::uint64_t world_ = 0;
        std::uint32_t unresolvedMapForms_ = 0;
        std::optional<Core::MapTravelRequest> escape_;
        bool escapeAttempted_ = false;
        double escapeSeconds_ = 0.0;
        std::optional<std::chrono::steady_clock::time_point> lastEligible_;
        bool releasedForTest_ = false;
    };
}
