#pragma once

#include "core/GemForms.h"
#include "core/SaveGeneration.h"
#include "core/SaveRecords.h"
#include "core/Types.h"

#include <functional>
#include <optional>

namespace SkyrimPoE::Core
{
    inline constexpr std::uint32_t kRecordMigration = 'MIGR';
    inline constexpr std::uint32_t kPreviousMigrationVersion = 1;
    inline constexpr std::uint32_t kMigrationVersion = 2;
    inline constexpr std::uint32_t kLegacyCoreVersion = 2;
    inline constexpr std::uint8_t kMaxReturnAttempts = 5;
    inline constexpr double kLegacyDeltaTolerance = 0.01;

    using FormIdResolver = std::function<bool(std::uint32_t raw, std::uint32_t& resolved)>;

    enum class CleanupStep : std::uint8_t
    {
        Begin = 0,
        CloseMapRun = 1,
        ReturnSlottedGems = 2,
        DetachBagSupports = 3,
        ReturnJewelsAndCharms = 4,
        RevertEngineState = 5,
        SpeedModsAndWorldDrops = 6,
        ClearCosave = 7,
        GrantStarter = 8,
        Finish = 9,
        Complete = 10
    };

    enum class ReturnKind : std::uint8_t { SlottedGem, BagSupport, Jewel, Charm };
    enum class ReturnStatus : std::uint8_t { Pending, Done, Lost };

    struct ReturnEntry
    {
        ReturnKind kind = ReturnKind::SlottedGem;
        std::string gemId;
        std::uint64_t instanceId = 0;
        std::uint32_t baseFormId = 0; // Jewel only. Zero requests the legacy color fallback.
        std::uint8_t jewelColor = 0;
        CharmKind charmKind = CharmKind::Ruby;
        std::uint32_t recipientFormId = 0;
        std::uint8_t depth = 0;
        ReturnStatus status = ReturnStatus::Pending;
        std::uint8_t attempts = 0;

        bool operator==(const ReturnEntry&) const = default;
    };

    struct MigrationMapRun
    {
        bool active = false;
        std::uint32_t version = 0;
        std::vector<std::uint8_t> bytes;

        bool operator==(const MigrationMapRun&) const = default;
    };

    enum class RefundKind : std::uint8_t { Map, Tribute };

    struct MigrationRefund
    {
        RefundKind kind = RefundKind::Map;
        std::uint32_t baseFormId = 0; // Zero means the refund's form was lost.
        std::uint32_t amount = 0;
        bool done = false;

        bool operator==(const MigrationRefund&) const = default;
    };

    enum class VirtualSlot : std::uint8_t { Ring2, Belt };

    struct MigrationVirtualRef
    {
        VirtualSlot slot = VirtualSlot::Ring2;
        InventoryItemKey key;
        std::uint32_t baseFormId = 0;
        bool released = false;

        bool operator==(const MigrationVirtualRef&) const = default;
    };

    struct MigrationSpeedMod
    {
        std::uint32_t actorFormId = 0;
        double delta = 0.0;

        bool operator==(const MigrationSpeedMod&) const = default;
    };

    enum class WorldDropStatus : std::uint8_t { Pending, Done, Retry, CorpseGone };

    struct MigrationWorldDrop
    {
        std::uint32_t dropRefFormId = 0;
        std::uint32_t corpseRefFormId = 0;
        std::uint32_t beamRefFormId = 0;
        std::uint8_t worldDropTier = 2;
        WorldDropStatus status = WorldDropStatus::Pending;

        bool operator==(const MigrationWorldDrop&) const = default;
    };

    [[nodiscard]] constexpr bool IsLegacyRunDrop(bool fromRunCorpse, bool inRunCell, bool packOwnsCell) noexcept
    {
        return fromRunCorpse || (packOwnsCell && inRunCell);
    }

    [[nodiscard]] constexpr std::int32_t RemainingLegacyDropCount(std::int32_t original,
        std::int32_t before, std::int32_t after) noexcept
    {
        const auto increase = static_cast<std::int64_t>(after) - before;
        return increase > 0 && increase <= original ? original - static_cast<std::int32_t>(increase) : original;
    }

    struct MigrationState
    {
        std::string sourceGame;
        std::uint32_t sourceGeneration = 0;
        std::string targetGame;
        std::uint32_t targetGeneration = 0;
        CleanupStep step = CleanupStep::Begin;
        double healthDelta = 0.0;
        double magickaDelta = 0.0;
        double speedMultDelta = 0.0;
        double magickaRateMultDelta = 0.0;
        bool deltasReverted = false;
        MigrationMapRun mapRun;
        bool mapRunClosed = false;
        std::vector<MigrationRefund> refunds;
        std::vector<ReturnEntry> returns;
        std::vector<MigrationVirtualRef> virtualRefs;
        std::vector<MigrationSpeedMod> speedMods;
        std::vector<MigrationWorldDrop> worldDrops;
        bool starterGranted = false;
        std::uint32_t lostCount = 0;

        bool operator==(const MigrationState&) const = default;
    };

    // 7단계 G1(결정 247): 정리 GrantStarter 직후·Normal 전환 전에 남기는 시작 지급 및 같은 틱 증거.
    struct StarterGrantDiagnostics
    {
        GemForms::StarterGrantReport report;
        std::vector<std::string> recordedIds;
        std::vector<std::string> committedIds;     // 정리 커밋 뒤 g_state에서 다시 읽은 STRT ID
        std::uint64_t grantCallsAtCommit = 0;      // 커밋 시점의 지급 함수 누적 호출 수
        std::uint64_t cleanupTickId = 0;
        std::uint64_t grantTickId = 0;
        std::uint64_t cleanupFrame = 0;
        std::uint64_t grantFrame = 0;
        std::uint32_t grantThreadId = 0;
        bool recordedBeforeNormal = false;

        [[nodiscard]] bool RanInCleanupTick() const noexcept
        {
            return cleanupTickId != 0 && grantTickId == cleanupTickId && cleanupFrame != 0 &&
                grantFrame == cleanupFrame && recordedBeforeNormal && committedIds == recordedIds;
        }

        bool operator==(const StarterGrantDiagnostics&) const = default;
    };

    struct MigrationReadResult
    {
        MigrationRecordStatus status = MigrationRecordStatus::Damaged;
        MigrationState state;
        std::uint32_t unresolvedCount = 0; // Affected entries, counted once each.
    };

    // v2 adds the two player rate deltas before the final CRC and still reads v1.
    // Writing over the format's bounds throws std::length_error. No resolver means identity.
    [[nodiscard]] std::vector<std::uint8_t> WriteMigrationRecord(const MigrationState& state);
    [[nodiscard]] MigrationReadResult ReadMigrationRecord(std::uint32_t version,
        std::span<const std::uint8_t> bytes, const FormIdResolver& resolver = {});

    enum class LegacyRecordStatus { Missing, Valid, Damaged };

    struct LegacyReadReport
    {
        LegacyRecordStatus coreFront = LegacyRecordStatus::Missing;
        LegacyRecordStatus playerRates = LegacyRecordStatus::Missing;
        bool slotsDamaged = false;
        LegacyRecordStatus gemKeys = LegacyRecordStatus::Missing;
        LegacyRecordStatus tree = LegacyRecordStatus::Missing;
        LegacyRecordStatus items = LegacyRecordStatus::Missing;
        LegacyRecordStatus gear = LegacyRecordStatus::Missing;
        LegacyRecordStatus spirit = LegacyRecordStatus::Missing;
        LegacyRecordStatus atlas = LegacyRecordStatus::Missing;
        LegacyRecordStatus maps = LegacyRecordStatus::Missing;
        LegacyRecordStatus speedMods = LegacyRecordStatus::Missing;
        LegacyRecordStatus worldDrops = LegacyRecordStatus::Missing;
        std::uint32_t gemKeyCount = 0;
        std::uint32_t treeNodeCount = 0;
        std::uint32_t itemCount = 0;
        std::uint32_t auraCount = 0;
        std::uint32_t atlasClears = 0;
        std::uint32_t speedModCount = 0;
        std::uint32_t speedModsFiltered = 0;
        std::uint32_t speedModsUnresolved = 0;
        std::uint32_t duplicateReturns = 0;
        std::uint32_t unresolvedCount = 0;
        std::string_view failureReason;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return coreFront == LegacyRecordStatus::Valid && playerRates != LegacyRecordStatus::Damaged;
        }
    };

    struct LegacyMigrationResult
    {
        MigrationState state;
        LegacyReadReport report;
    };

    enum class LegacyReturnDestination { SavedOwner, Player };

    [[nodiscard]] LegacyMigrationResult BuildLegacyMigration(const SaveRecordBatch& batch, std::string_view targetGame,
        std::uint32_t targetGeneration, const FormIdResolver& resolver = {},
        LegacyReturnDestination destination = LegacyReturnDestination::SavedOwner);

    enum class DeltaTrust { Trusted, Untrusted };
    [[nodiscard]] DeltaTrust JudgeLegacyDelta(double delta, double modifier) noexcept;
    [[nodiscard]] std::string_view CheckLegacyActorDeltas(const MigrationState& state, double permanentHealth,
        double permanentMagicka, double temporarySpeedMult, double temporaryMagickaRateMult) noexcept;

    enum class StepOutcome { Done, Retry, Waiting };

    class CleanupOps
    {
    public:
        virtual ~CleanupOps() = default;
        virtual StepOutcome CloseMapRun(MigrationState&) = 0;
        virtual StepOutcome ReturnSlottedGems(MigrationState&) = 0;
        virtual StepOutcome DetachBagSupports(MigrationState&) = 0;
        virtual StepOutcome ReturnJewelsAndCharms(MigrationState&) = 0;
        virtual StepOutcome RevertEngineState(MigrationState&) = 0;
        virtual StepOutcome SpeedModsAndWorldDrops(MigrationState&) = 0;
        virtual StepOutcome ClearCosave(MigrationState&) = 0;
        virtual StepOutcome GrantStarter(MigrationState&) = 0;
        virtual StepOutcome Finish(MigrationState&) = 0;
    };

    // Ops update the entries they process. AdvanceCleanup owns the step and return retry counters.
    [[nodiscard]] StepOutcome AdvanceCleanup(MigrationState& state, CleanupOps& ops);
    // One-tick cleanup requires every operation to finish during its call.
    [[nodiscard]] StepOutcome CompleteCleanup(MigrationState& state, CleanupOps& ops);
}
