#pragma once

#include "core/MapTypes.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimPoE::Core
{
    class ItemData;
    class DataPack;

    struct MapMonsterMultipliers
    {
        double lifeMultiplier = 1.0;
        double damageMultiplier = 1.0;
    };

    struct MapSpawnDeletionObservation
    {
        bool isActor = false;
        bool isPlayer = false;
        bool isDisabled = false;
        std::uint32_t actorBaseId = 0;
        std::uint32_t cellId = 0;
    };

    // 결정 178 ⑦ 보완: 셀이 다시 붙었을 때 앞선 회차의 지도 스폰이 남아 있는지 보려고 셀의 참조 하나를 관찰한 값.
    struct MapLeftoverObservation
    {
        bool isActor = false;
        bool isPlayer = false;
        // 플레이어 동료나 지휘받는 액터(다른 모드가 데려온 같은 원형 NPC일 수 있음)는 남은 스폰으로 보지 않는다.
        bool isFollowerOrCommanded = false;
        bool isDeleted = false;
        bool isDisabled = false;
        std::uint32_t formId = 0;
        std::uint32_t actorBaseId = 0;
        std::uint32_t cellId = 0;
    };

    inline constexpr double kMapBossMissingSec = 10.0;

    struct MapBossLossObservation
    {
        bool stableRunCell = false;
        bool playerAuthorized = false;
        bool spawned = false;
        bool travelPending = false;
        bool deathConfirmed = false;
        bool deathPending = false;
        bool bossMissing = false;
        bool paused = false;
    };

    struct MapBossLossProgress
    {
        double missingSec = 0.0;
        bool suspected = false;
    };

    inline constexpr std::uint32_t kArena4PrerequisiteMask = 0x7u;
    inline constexpr std::uint32_t kArena4MilestoneBit = 0x8u;
    inline constexpr std::uint8_t kArena4FirstPhase = 1;
    inline constexpr std::uint8_t kArena4FinalPhase = 3;

    enum class Arena4EntryDenial
    {
        None,
        MissingMilestones,
        AlreadyCleared
    };

    struct Arena4EntryStatus
    {
        Arena4EntryDenial denial;
        int milestonesCompleted;

        [[nodiscard]] bool CanEnter() const noexcept { return denial == Arena4EntryDenial::None; }
    };

    struct Arena4BossDeathState
    {
        std::uint8_t completedPhase;
        bool invulnerable;
    };

    // 보스 사망 시작(치명타)부터 사망 확정·처치 반영까지 귀환을 미루는 상한. 사망 연출은 실측 약 1.5초.
    inline constexpr double kBossDeathProcessWaitSec = 10.0;

    // 결정 179 개정(2026-11-23): 보스 처치의 회차 실패·보상 차단은 치명타(사망 시작) 시점으로 판정한다.
    struct MapBossKillJudgement
    {
        bool runFailed;
        bool rewardBlocked;
    };

    struct MapActorScaling
    {
        std::uint64_t runId = 0;
        double lifeMultiplier = 1.0;
        double damageMultiplier = 1.0;

        [[nodiscard]] double ScaleIncomingDamage(
            double damage, double referenceLife = 1.0, double permanentHealth = 1.0) const noexcept;
    };

    // 결정 169·170: 스폰 목록에서 복원하며, 읽는 쪽과 쓰는 쪽의 동기화는 런타임이 맡는다.
    class MapActorRegistry
    {
    public:
        void Register(std::uint32_t actorId, MapActorScaling scaling);
        [[nodiscard]] std::optional<MapActorScaling> Find(std::uint32_t actorId) const;
        void Rebuild(const ActiveMapState& state, const DataPack& pack);
        void Clear() noexcept;

    private:
        std::unordered_map<std::uint32_t, MapActorScaling> actors_;
    };

    // 6단계(결정 166): 지도 티어·던전·모드 집계·포털 거래·아틀라스 +24 패시브·4차 결투장 3페이즈 순수 코어 로직
    class MapSystem
    {
    public:
        [[nodiscard]] static std::uint32_t MonsterLevelForTier(std::uint8_t tier) noexcept;
        [[nodiscard]] static std::uint32_t RewardMonsterLevel(const ActiveMapState& state) noexcept;
        [[nodiscard]] static MapMonsterMultipliers GetMapMonsterMultipliers(
            std::uint32_t mapLevel, const DataPack& pack) noexcept;
        [[nodiscard]] static double AilmentThreshold(
            std::uint32_t exileLevel, double lifeMultiplier, const DataPack& pack) noexcept;
        [[nodiscard]] static double EstimateVanillaHitPoe(
            double rawHitDamage, std::uint32_t exileLevel, const DataPack& pack) noexcept;

        // 지도 어픽스 16종 집계 + IIQ/IIR/PackSize 보상 합산
        [[nodiscard]] static MapModSummary AggregateMapMods(const std::vector<RolledAffix>& affixes) noexcept;
        [[nodiscard]] static bool AreSavedMapRollsValid(const ItemData& data, std::span<const RolledAffix> affixes);
        [[nodiscard]] static std::size_t MapPackActorCount(std::uint32_t basePacks, double packSizePct) noexcept;

        // 아틀라스 +24 패시브 포인트 계산 (T1~T16 첫 클리어 최대 16점 + 1~4차 보스/결투장 이정표 최대 8점)
        [[nodiscard]] static std::uint32_t CountAtlasTierPoints(std::uint16_t completedTierMask) noexcept;
        [[nodiscard]] static std::uint32_t CountMilestonePassivePoints(std::uint32_t ascendancyMilestonesBitmask) noexcept;
        [[nodiscard]] static double AtlasBonusIiqPct(std::uint16_t completedTierMask) noexcept;
        [[nodiscard]] static double AtlasBonusIirPct(std::uint16_t completedTierMask) noexcept;

        // 지도 보스 클리어 반영: 해당 티어가 첫 클리어이면 true 반환
        static bool RecordMapClear(AtlasState& atlas, std::uint8_t tier) noexcept;

        [[nodiscard]] static bool IsMapKillRewardEligible(
            bool killRewardAwarded, bool rewardBlocked, bool runFailed) noexcept;
        [[nodiscard]] static bool IsArena4ClearEligible(
            bool mapKillRewardEligible, Arena4BossDeathState death) noexcept;
        [[nodiscard]] static Arena4EntryStatus GetArena4EntryStatus(std::uint32_t milestonesMask) noexcept;
        [[nodiscard]] static Arena4EntryDenial TryUnlockArena4(std::uint32_t& milestonesMask) noexcept;
        [[nodiscard]] static bool IsArena4PhaseInvulnerable(
            std::uint8_t requestedPhase, std::uint8_t completedPhase, double remainingSec) noexcept;
        [[nodiscard]] static std::uint8_t Arena4PhaseForSave(
            std::uint8_t requestedPhase, std::uint8_t completedPhase) noexcept;

        // 포털 거래 상태머신 (원검토 #42)
        [[nodiscard]] static ActiveMapState CreateNormalMapRun(
            std::uint64_t runId,
            std::uint8_t tier,
            const std::vector<RolledAffix>& mods,
            std::uint32_t returnCellFormId,
            std::uint32_t returnWorldspaceFormId,
            const std::array<float, 3>& returnPos,
            const std::array<float, 3>& returnAngle);

        [[nodiscard]] static ActiveMapState CreateArena4Run(
            std::uint64_t runId,
            std::uint32_t returnCellFormId,
            std::uint32_t returnWorldspaceFormId,
            const std::array<float, 3>& returnPos,
            const std::array<float, 3>& returnAngle);

        // 재입장 시 포털 1 소모 (포털 0, 실패 또는 종료 요청 상태면 false)
        [[nodiscard]] static bool TryConsumePortalForReenter(ActiveMapState& state) noexcept;
        [[nodiscard]] static bool ShouldEndOnReturn(const ActiveMapState& state) noexcept;
        // 결정 182: 출구 구역 귀환이 보스 처치 전에 회차를 끝내게 되는 경우(포털 0, 보스 생존)는 거절한다.
        // 종료 요청·실패한 회차, 끝낼 귀환으로 스폰을 이미 끈 회차, 이미 쓰러진 보스(처치 반영 대기 포함)는 귀환 쪽 규칙에 맡긴다.
        // 런타임은 bossDeadNow=false로 불러 "구역 안에서만 귀환을 시도할 상태"(래치 해제)를 정하는 데도 쓴다.
        [[nodiscard]] static bool ShouldRefuseExitZoneReturn(const ActiveMapState& state, bool bossDeadNow) noexcept;
        [[nodiscard]] static bool ShouldDeferReturnForBossDeath(
            bool bossKilled, bool bossDeadNow, bool deathConfirmed, bool killPending, double waitedSec) noexcept;
        // 사망 시작 기록(실패 전에만 남김)이 있으면 그때의 판정을, 없으면 지금 판정을 쓴다.
        [[nodiscard]] static MapBossKillJudgement JudgeBossKill(bool deathStartedBeforeFailure,
            bool rewardBlockedAtDeathStart, bool runFailedNow, bool rewardBlockedNow) noexcept;
        // 회차를 끝내는 귀환이 스폰 비활성화를 미뤄야 하는지: 보스 처치 처리가 남았고 상한 안일 때.
        [[nodiscard]] static bool ShouldHoldEndingDisableForBossDeath(
            bool killPending, bool bossKilled, double waitedSec) noexcept;
        [[nodiscard]] static bool CanCheckBossLoss(
            const ActiveMapState& state, const MapBossLossObservation& observation) noexcept;
        [[nodiscard]] static MapBossLossProgress ObserveBossLoss(const ActiveMapState& state,
            const MapBossLossObservation& observation, double missingSec, double deltaSec) noexcept;
        [[nodiscard]] static std::uint32_t SelectMapBossCandidate(std::uint32_t livingBoss,
            std::span<const std::uint32_t> deadBosses, std::span<const std::uint32_t> spawnedRefIds) noexcept;
        [[nodiscard]] static bool MatchesMapBoss(std::uint32_t bossBaseId, std::uint32_t runCellId,
            std::uint32_t actorBaseId, std::uint32_t actorCellId) noexcept;
        static void RebindMapBoss(ActiveMapState& state, std::uint32_t bossId);
        [[nodiscard]] static std::vector<std::uint32_t> MapSpawnCleanupIds(const ActiveMapState& state);
        [[nodiscard]] static bool ReleaseDeathEscape(std::atomic<std::uint64_t>& owner, std::uint64_t runId) noexcept;

        [[nodiscard]] static bool IsRunDrop(std::uint32_t corpseId, std::span<const std::uint32_t> spawnedRefIds) noexcept;
        [[nodiscard]] static bool CanDeleteMapSpawn(std::uint32_t actorBaseId, std::uint32_t runCellId,
            const MapSpawnDeletionObservation& observed) noexcept;
        // 결정 178 ⑦ 보완(2026-11-23 사용자 확인): 붙지 않은 셀이라 끄지 않고 남긴 스폰 가운데 셀 초기화 뒤에도 살아 남은 것을,
        // 셀이 다시 붙었을 때 찾는다. 붙은 그 셀에 있는, 지워지지도 꺼지지도 않은 동적(FF) 액터 가운데 그 던전의 스폰 원형이고
        // 지금 회차에 등록되지 않은 것만 남은 스폰으로 본다.
        [[nodiscard]] static bool IsLeftoverMapSpawn(std::uint32_t sweptCellId, bool cellAttached,
            std::span<const std::uint32_t> templateIds, std::span<const std::uint32_t> currentRunIds,
            const MapLeftoverObservation& observed) noexcept;
        [[nodiscard]] static bool ShouldGuaranteeFirstTribute(bool activeRun, bool isMapBoss, bool rewardBlocked,
            std::uint8_t dropMapTier, bool firstTributeGiven) noexcept;

        // 지도 내 치명타 사망 탈출 처리 (PoE1 표준 6포털 = 총 6회 진입/목숨, 이중 차감 및 0포털 좀비 상태 차단):
        // - 일반 지도: 남은 포털 > 0이면 안전 귀환(활성 유지, 재입장 시 포털 -1 소모) -> true;
        //             남은 포털 == 0(마지막 6번째 포털에서 사망)이면 즉시 실패 종료 -> false
        // - 4차 결투장: 단판(1포털)이므로 사망 즉시 실패 종료 -> false
        [[nodiscard]] static bool ConsumePortalOnDeathEscape(ActiveMapState& state) noexcept;

        // 4차 결투장 3페이즈 보스 피해 게이트 (검수 C-5):
        // - invulnActive == true이면 피해 0.0
        // - Phase 1은 체력이 maxHp * 0.665 아래로 내려가지 않게 클램프하고 2페이즈 전환 신호
        // - Phase 2는 체력이 maxHp * 0.335 아래로 내려가지 않게 클램프하고 3페이즈 전환 신호
        // - Phase 3는 클램프 없이 처치 허용
        [[nodiscard]] static ArenaDamageClampResult ClampArenaBossDamage(
            std::uint8_t currentPhase,
            bool invulnActive,
            double currentHp,
            double maxHp,
            double incomingEffectiveDamage) noexcept;

        // 지도 어픽스 한글 설명 요약 문자열 생성
        [[nodiscard]] static std::string FormatMapModKoreanLine(const RolledAffix& affix);
    };
}
