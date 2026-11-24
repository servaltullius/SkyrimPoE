#include "core/MapSystem.h"

#include "core/DataPack.h"
#include "core/ItemData.h"
#include "core/RulesPack.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>

namespace SkyrimPoE::Core
{
    bool MapSystem::IsMapKillRewardEligible(bool killRewardAwarded, bool rewardBlocked, bool runFailed) noexcept
    {
        return killRewardAwarded && !rewardBlocked && !runFailed;
    }

    bool MapSystem::IsArena4ClearEligible(bool mapKillRewardEligible, Arena4BossDeathState death) noexcept
    {
        return mapKillRewardEligible && death.completedPhase >= kArena4FinalPhase && !death.invulnerable;
    }

    Arena4EntryStatus MapSystem::GetArena4EntryStatus(std::uint32_t milestonesMask) noexcept
    {
        const auto prerequisites = milestonesMask & kArena4PrerequisiteMask;
        const auto milestonesCompleted = std::popcount(prerequisites);
        if ((milestonesMask & kArena4MilestoneBit) != 0u) {
            return { Arena4EntryDenial::AlreadyCleared, milestonesCompleted };
        }
        return { prerequisites == kArena4PrerequisiteMask ? Arena4EntryDenial::None : Arena4EntryDenial::MissingMilestones,
            milestonesCompleted };
    }

    Arena4EntryDenial MapSystem::TryUnlockArena4(std::uint32_t& milestonesMask) noexcept
    {
        const auto status = GetArena4EntryStatus(milestonesMask);
        if (status.CanEnter()) {
            milestonesMask |= kArena4MilestoneBit;
        }
        return status.denial;
    }

    bool MapSystem::IsArena4PhaseInvulnerable(
        std::uint8_t requestedPhase, std::uint8_t completedPhase, double remainingSec) noexcept
    {
        return remainingSec > 0.0 || requestedPhase > completedPhase;
    }

    std::uint8_t MapSystem::Arena4PhaseForSave(std::uint8_t requestedPhase, std::uint8_t completedPhase) noexcept
    {
        return std::min(requestedPhase, completedPhase);
    }

    bool MapSystem::CanCheckBossLoss(const ActiveMapState& state, const MapBossLossObservation& observation) noexcept
    {
        return state.active && !state.failed && !state.bossKilled && !state.endRequested &&
            observation.stableRunCell && observation.playerAuthorized && observation.spawned && !state.spawnsDisabled &&
            !observation.travelPending && !observation.deathConfirmed && !observation.deathPending;
    }

    MapBossLossProgress MapSystem::ObserveBossLoss(const ActiveMapState& state,
        const MapBossLossObservation& observation, double missingSec, double deltaSec) noexcept
    {
        if (!CanCheckBossLoss(state, observation) || !observation.bossMissing) {
            return {};
        }
        if (observation.paused) {
            return { missingSec, false };
        }
        if (std::isfinite(deltaSec) && deltaSec > 0.0) {
            missingSec = std::min(kMapBossMissingSec, missingSec + deltaSec);
        }
        return { missingSec, state.unresolvedBossRefFormId != 0 || missingSec >= kMapBossMissingSec };
    }

    std::uint32_t MapSystem::SelectMapBossCandidate(std::uint32_t livingBoss,
        std::span<const std::uint32_t> deadBosses, std::span<const std::uint32_t> spawnedRefIds) noexcept
    {
        if (livingBoss != 0) {
            return livingBoss;
        }
        const auto owned = std::ranges::find_if(deadBosses, [spawnedRefIds](std::uint32_t corpseId) {
            return IsRunDrop(corpseId, spawnedRefIds);
        });
        return owned != deadBosses.end() ? *owned : 0;
    }

    bool MapSystem::MatchesMapBoss(std::uint32_t bossBaseId, std::uint32_t runCellId,
        std::uint32_t actorBaseId, std::uint32_t actorCellId) noexcept
    {
        return bossBaseId != 0 && bossBaseId == actorBaseId && runCellId != 0 && runCellId == actorCellId;
    }

    void MapSystem::RebindMapBoss(ActiveMapState& state, std::uint32_t bossId)
    {
        auto& spawned = state.spawnedRefFormIds;
        if (state.bossRefFormId != bossId) {
            std::erase(spawned, state.bossRefFormId);
        }
        if (!IsRunDrop(bossId, spawned) && spawned.size() < kMaxMapSpawnedActors) {
            spawned.push_back(bossId);
        }
        state.bossRefFormId = bossId;
        state.unresolvedBossRefFormId = 0;
    }

    std::vector<std::uint32_t> MapSystem::MapSpawnCleanupIds(const ActiveMapState& state)
    {
        auto ids = state.spawnedRefFormIds;
        if (state.bossRefFormId != 0 && !IsRunDrop(state.bossRefFormId, ids)) {
            ids.push_back(state.bossRefFormId);
        }
        return ids;
    }

    bool MapSystem::ReleaseDeathEscape(std::atomic<std::uint64_t>& owner, std::uint64_t runId) noexcept
    {
        return runId != 0 && owner.compare_exchange_strong(runId, 0, std::memory_order_acq_rel);
    }

    namespace
    {
        // 결정 170 U-4: 바닐라 타격 역산에 쓰는 스카이림 체력 기준값.
        constexpr double kVanillaHitReferenceLife = 100.0;

        [[nodiscard]] double FirstRollValue(const RolledAffix& affix) noexcept
        {
            if (affix.values.empty()) {
                return 0.0;
            }
            const double v = affix.values[0];
            return std::isfinite(v) ? std::abs(v) : 0.0;
        }
    }

    std::uint32_t MapSystem::MonsterLevelForTier(std::uint8_t tier) noexcept
    {
        const auto clamped = std::clamp<std::uint8_t>(tier, kMinMapTier, kMaxMapTier);
        return kMapBaseMonsterLevel + static_cast<std::uint32_t>(clamped - 1);
    }

    std::uint32_t MapSystem::RewardMonsterLevel(const ActiveMapState& state) noexcept
    {
        // 결정 116·170 U-4: 결투장은 Lv75, 일반 지도는 회차 티어로 처치 보상 레벨을 정한다.
        if (!state.active) {
            return 0;
        }
        return state.isArena4 ? kArena4MonsterLevel : MonsterLevelForTier(state.tier);
    }

    MapMonsterMultipliers MapSystem::GetMapMonsterMultipliers(std::uint32_t mapLevel, const DataPack& pack) noexcept
    {
        // 결정 170 U-3: 일반 몬스터 생명과 지도 생명 배율을 곱하고, 생명·피해 모두 T1을 기준으로 삼는다.
        const double baseLife = pack.MonsterLife(kMapBaseMonsterLevel) * pack.MapLevelLifeMult(kMapBaseMonsterLevel);
        const double baseDamage = pack.MonsterDamage(kMapBaseMonsterLevel);
        return {
            pack.MonsterLife(mapLevel) * pack.MapLevelLifeMult(mapLevel) / baseLife,
            pack.MonsterDamage(mapLevel) / baseDamage
        };
    }

    double MapSystem::AilmentThreshold(std::uint32_t monsterLevel) noexcept
    {
        const auto& tables = ActiveRuleValues().levelTables;
        return tables.At(tables.ailmentThreshold, monsterLevel);
    }

    double MapSystem::PoiseThreshold(std::uint32_t monsterLevel) noexcept
    {
        const auto& tables = ActiveRuleValues().levelTables;
        return tables.At(tables.poiseThreshold, monsterLevel);
    }

    double MapSystem::EstimateVanillaHitPoe(double rawHitDamage, std::uint32_t exileLevel, const DataPack& pack) noexcept
    {
        // 결정 170 U-4 및 R3 브리프: 바닐라 타격은 유배자 레벨의 몬스터 생명으로 역산한다.
        return rawHitDamage * pack.MonsterLife(exileLevel) / kVanillaHitReferenceLife;
    }

    double MapActorScaling::ScaleIncomingDamage(double damage, double referenceLife, double permanentHealth) const noexcept
    {
        // 결정 173: 스카이림 최대 체력을 유배자 레벨의 기준 체력으로 맞춘 뒤 티어 배율을 적용한다.
        const double healthMatch = std::isfinite(permanentHealth) && permanentHealth >= 1.0 &&
                                   std::isfinite(referenceLife) && referenceLife > 0.0
            ? referenceLife / permanentHealth
            : 1.0;
        const double scaled = damage / (lifeMultiplier * healthMatch);
        // 설정이 허용하는 극단값(아주 작은 offenseK 등)에서 나눗셈이 넘치면 티어 배율만 적용한다.
        return std::isfinite(scaled) ? scaled : damage / lifeMultiplier;
    }

    void MapActorRegistry::Register(std::uint32_t actorId, MapActorScaling scaling)
    {
        if (actorId != 0) {
            actors_.insert_or_assign(actorId, scaling);
        }
    }

    std::optional<MapActorScaling> MapActorRegistry::Find(std::uint32_t actorId) const
    {
        const auto it = actors_.find(actorId);
        if (it == actors_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void MapActorRegistry::Rebuild(const ActiveMapState& state, const DataPack& pack)
    {
        Clear();
        if (!state.active) {
            return;
        }

        const auto multipliers = MapSystem::GetMapMonsterMultipliers(MapSystem::RewardMonsterLevel(state), pack);
        const MapActorScaling scaling{ state.runId, multipliers.lifeMultiplier, multipliers.damageMultiplier };
        for (const auto actorId : state.spawnedRefFormIds) {
            Register(actorId, scaling);
        }
        Register(state.bossRefFormId, scaling);
    }

    void MapActorRegistry::Clear() noexcept
    {
        actors_.clear();
    }

    bool MapSystem::AreSavedMapRollsValid(const ItemData& data, std::span<const RolledAffix> affixes)
    {
        for (const auto& affix : affixes) {
            const auto* def = data.FindAffix(affix.affixId);
            if (!def || affix.values.size() != def->TotalRangeCount()) {
                return false;
            }
            std::size_t index = 0;
            for (const auto& line : def->lines) {
                for (const auto& range : line.ranges) {
                    const double value = affix.values[index++];
                    if (!std::isfinite(value) || value < range.minVal || value > range.maxVal) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    MapModSummary MapSystem::AggregateMapMods(const std::vector<RolledAffix>& affixes) noexcept
    {
        MapModSummary out;
        for (const auto& a : affixes) {
            const double v = FirstRollValue(a);
            const std::string_view id = a.affixId;
            if (id == "MapMod_MonsterDamage") {
                out.monsterDamageIncPct += v;
                out.bonusIiqPct += 12.0;
                out.bonusIirPct += 8.0;
                out.packSizePct += 5.0;
            } else if (id == "MapMod_ExtraFire") {
                out.monsterExtraFirePct += v;
                out.bonusIiqPct += 12.0;
                out.bonusIirPct += 8.0;
                out.packSizePct += 5.0;
            } else if (id == "MapMod_ExtraCold") {
                out.monsterExtraColdPct += v;
                out.bonusIiqPct += 12.0;
                out.bonusIirPct += 8.0;
                out.packSizePct += 5.0;
            } else if (id == "MapMod_ExtraLightning") {
                out.monsterExtraLightningPct += v;
                out.bonusIiqPct += 12.0;
                out.bonusIirPct += 8.0;
                out.packSizePct += 5.0;
            } else if (id == "MapMod_MonsterHaste") {
                out.forceHasted = true;
                out.monsterHasteSpeedBonus += v;
                out.bonusIiqPct += 14.0;
                out.bonusIirPct += 8.0;
                out.packSizePct += 6.0;
            } else if (id == "MapMod_MonsterCrit") {
                out.forceDeadly = true;
                out.monsterCritChanceIncPct += v;
                out.bonusIiqPct += 14.0;
                out.bonusIirPct += 10.0;
                out.packSizePct += 5.0;
            } else if (id == "MapMod_MonsterLife") {
                out.monsterReducedTakenPct += v;
                out.bonusIiqPct += 10.0;
                out.bonusIirPct += 10.0;
                out.packSizePct += 5.0;
            } else if (id == "MapMod_MoreMagicRare") {
                out.moreMagicRarePct += v;
                out.bonusIiqPct += 10.0;
                out.bonusIirPct += 12.0;
                out.packSizePct += 8.0;
            } else if (id == "MapMod_MinusMaxRes") {
                out.playerMaxResPenalty += v;
                out.bonusIiqPct += 18.0;
                out.bonusIirPct += 12.0;
                out.packSizePct += 8.0;
            } else if (id == "MapMod_LessArmour") {
                out.playerLessArmourPct += v;
                out.bonusIiqPct += 12.0;
                out.bonusIirPct += 8.0;
                out.packSizePct += 5.0;
            } else if (id == "MapMod_LessEvasion") {
                out.playerLessEvasionPct += v;
                out.bonusIiqPct += 12.0;
                out.bonusIirPct += 8.0;
                out.packSizePct += 5.0;
            } else if (id == "MapMod_LessRecovery") {
                out.playerLessEsRechargePct += v;
                out.bonusIiqPct += 15.0;
                out.bonusIirPct += 10.0;
                out.packSizePct += 6.0;
            } else if (id == "MapMod_MonsterElemRes") {
                out.monsterElemResPct += v;
                out.bonusIiqPct += 10.0;
                out.bonusIirPct += 8.0;
                out.packSizePct += 5.0;
            } else if (id == "MapMod_MonsterChaosRes") {
                out.monsterChaosResPct += v;
                out.bonusIiqPct += 8.0;
                out.bonusIirPct += 8.0;
                out.packSizePct += 4.0;
            } else if (id == "MapMod_Vulnerability") {
                out.playerPhysTakenIncPct += v;
                out.bonusIiqPct += 15.0;
                out.bonusIirPct += 10.0;
                out.packSizePct += 6.0;
            } else if (id == "MapMod_PackSize") {
                out.packSizePct += v + 12.0;
                out.bonusIiqPct += 10.0;
                out.bonusIirPct += 8.0;
            }
        }
        // 안전 상한 클램프
        out.monsterHasteSpeedBonus = std::clamp(out.monsterHasteSpeedBonus, 0.0, 50.0);
        out.monsterCritChanceIncPct = std::clamp(out.monsterCritChanceIncPct, 0.0, 100.0);
        out.monsterReducedTakenPct = std::clamp(out.monsterReducedTakenPct, 0.0, 60.0);
        out.playerMaxResPenalty = std::clamp(out.playerMaxResPenalty, 0.0, 25.0);
        out.playerLessArmourPct = std::clamp(out.playerLessArmourPct, 0.0, 75.0);
        out.playerLessEvasionPct = std::clamp(out.playerLessEvasionPct, 0.0, 75.0);
        out.playerLessEsRechargePct = std::clamp(out.playerLessEsRechargePct, 0.0, 75.0);
        out.monsterElemResPct = std::clamp(out.monsterElemResPct, 0.0, 75.0);
        out.monsterChaosResPct = std::clamp(out.monsterChaosResPct, 0.0, 75.0);
        return out;
    }

    std::uint32_t MapSystem::CountAtlasTierPoints(std::uint16_t completedTierMask) noexcept
    {
        return static_cast<std::uint32_t>(std::popcount(completedTierMask));
    }

    std::uint32_t MapSystem::CountMilestonePassivePoints(std::uint32_t ascendancyMilestonesBitmask) noexcept
    {
        return static_cast<std::uint32_t>(std::popcount(ascendancyMilestonesBitmask & 0xFu)) * 2u;
    }

    double MapSystem::AtlasBonusIiqPct(std::uint16_t completedTierMask) noexcept
    {
        return static_cast<double>(CountAtlasTierPoints(completedTierMask)) * 2.0;
    }

    double MapSystem::AtlasBonusIirPct(std::uint16_t completedTierMask) noexcept
    {
        return static_cast<double>(CountAtlasTierPoints(completedTierMask)) * 3.0;
    }

    bool MapSystem::RecordMapClear(AtlasState& atlas, std::uint8_t tier) noexcept
    {
        if (tier < kMinMapTier || tier > kMaxMapTier) {
            return false;
        }
        const std::uint16_t bit = static_cast<std::uint16_t>(1u << (tier - 1));
        const bool firstClear = (atlas.completedTierMask & bit) == 0;
        atlas.completedTierMask |= bit;
        if (atlas.totalMapsCleared < std::numeric_limits<std::uint32_t>::max()) {
            ++atlas.totalMapsCleared;
        }
        if (tier > atlas.highestTierCleared) {
            atlas.highestTierCleared = tier;
        }
        return firstClear;
    }

    ActiveMapState MapSystem::CreateNormalMapRun(
        std::uint64_t runId,
        std::uint8_t tier,
        const std::vector<RolledAffix>& mods,
        std::uint32_t returnCellFormId,
        std::uint32_t returnWorldspaceFormId,
        const std::array<float, 3>& returnPos,
        const std::array<float, 3>& returnAngle)
    {
        const auto clampedTier = std::clamp<std::uint8_t>(tier, kMinMapTier, kMaxMapTier);
        ActiveMapState st;
        st.active = true;
        st.isArena4 = false;
        st.bossKilled = false;
        st.failed = false;
        st.runId = runId;
        st.tier = clampedTier;
        // 개방 즉시 최초 입장 포털 1개 소모: 6 -> 5
        st.portalsRemaining = kNormalMapMaxPortals - 1;
        st.arenaPhase = 1;
        st.mods = mods;
        st.summary = AggregateMapMods(mods);
        st.returnCellFormId = returnCellFormId;
        st.returnWorldspaceFormId = returnWorldspaceFormId;
        st.returnPos = returnPos;
        st.returnAngle = returnAngle;
        return st;
    }

    ActiveMapState MapSystem::CreateArena4Run(
        std::uint64_t runId,
        std::uint32_t returnCellFormId,
        std::uint32_t returnWorldspaceFormId,
        const std::array<float, 3>& returnPos,
        const std::array<float, 3>& returnAngle)
    {
        ActiveMapState st;
        st.active = true;
        st.isArena4 = true;
        st.bossKilled = false;
        st.failed = false;
        st.runId = runId;
        st.tier = 8;
        // 4차 결투장은 1포털 단판: 개방·입장 즉시 0 잔여 (재입장·부활 불가)
        st.portalsRemaining = 0;
        st.arenaPhase = 1;
        st.returnCellFormId = returnCellFormId;
        st.returnWorldspaceFormId = returnWorldspaceFormId;
        st.returnPos = returnPos;
        st.returnAngle = returnAngle;
        return st;
    }

    bool MapSystem::TryConsumePortalForReenter(ActiveMapState& state) noexcept
    {
        if (!state.active || state.failed || state.isArena4 || state.endRequested || state.portalsRemaining == 0) {
            return false;
        }
        --state.portalsRemaining;
        return true;
    }

    bool MapSystem::ShouldEndOnReturn(const ActiveMapState& state) noexcept
    {
        return state.bossKilled || state.portalsRemaining == 0 || state.endRequested;
    }

    bool MapSystem::ShouldRefuseExitZoneReturn(const ActiveMapState& state, bool bossDeadNow) noexcept
    {
        return state.active && !state.failed && !state.endRequested && !state.spawnsDisabled && !state.bossKilled && !bossDeadNow &&
            state.portalsRemaining == 0;
    }

    bool MapSystem::ShouldDeferReturnForBossDeath(
        bool bossKilled, bool bossDeadNow, bool deathConfirmed, bool killPending, double waitedSec) noexcept
    {
        if (bossKilled || waitedSec >= kBossDeathProcessWaitSec) {
            return false;
        }
        return killPending || (bossDeadNow && !deathConfirmed);
    }

    MapBossKillJudgement MapSystem::JudgeBossKill(bool deathStartedBeforeFailure,
        bool rewardBlockedAtDeathStart, bool runFailedNow, bool rewardBlockedNow) noexcept
    {
        if (deathStartedBeforeFailure) {
            return { false, rewardBlockedAtDeathStart };
        }
        return { runFailedNow, rewardBlockedNow };
    }

    bool MapSystem::ShouldHoldEndingDisableForBossDeath(bool killPending, bool bossKilled, double waitedSec) noexcept
    {
        const double waited = std::isfinite(waitedSec) ? waitedSec : kBossDeathProcessWaitSec;
        return killPending && !bossKilled && waited < kBossDeathProcessWaitSec;
    }

    std::size_t MapSystem::MapPackActorCount(std::uint32_t basePacks, double packSizePct) noexcept
    {
        const double baseActors = std::clamp(basePacks, 1u, 6u) * 3.0;
        const double sizePct = std::isfinite(packSizePct) ? packSizePct : 0.0;
        const double total = std::round(baseActors + baseActors * sizePct / 100.0);
        return static_cast<std::size_t>(std::clamp(total, 1.0, static_cast<double>(kMaxMapSpawnedActors - 1)));
    }

    bool MapSystem::IsRunDrop(std::uint32_t corpseId, std::span<const std::uint32_t> spawnedRefIds) noexcept
    {
        return corpseId != 0 && std::ranges::find(spawnedRefIds, corpseId) != spawnedRefIds.end();
    }

    bool MapSystem::CanDeleteMapSpawn(std::uint32_t actorBaseId, std::uint32_t runCellId,
        const MapSpawnDeletionObservation& observed) noexcept
    {
        return observed.isActor && !observed.isPlayer && observed.isDisabled &&
            actorBaseId != 0 && actorBaseId == observed.actorBaseId &&
            runCellId != 0 && runCellId == observed.cellId;
    }

    bool MapSystem::IsLeftoverMapSpawn(std::uint32_t sweptCellId, bool cellAttached,
        std::span<const std::uint32_t> templateIds, std::span<const std::uint32_t> currentRunIds,
        const MapLeftoverObservation& observed) noexcept
    {
        constexpr std::uint32_t kDynamicFormMask = 0xFF000000u;
        if (!cellAttached || sweptCellId == 0 || observed.cellId != sweptCellId || !observed.isActor || observed.isPlayer ||
            observed.isFollowerOrCommanded ||
            observed.isDeleted || observed.isDisabled || (observed.formId & kDynamicFormMask) != kDynamicFormMask ||
            observed.actorBaseId == 0) {
            return false;
        }
        const auto contains = [](std::span<const std::uint32_t> ids, std::uint32_t id) {
            for (const auto candidate : ids) {
                if (candidate == id) {
                    return true;
                }
            }
            return false;
        };
        return contains(templateIds, observed.actorBaseId) && !contains(currentRunIds, observed.formId);
    }

    bool MapSystem::ShouldGuaranteeFirstTribute(bool activeRun, bool isMapBoss, bool rewardBlocked,
        std::uint8_t dropMapTier, bool firstTributeGiven) noexcept
    {
        return activeRun && isMapBoss && !rewardBlocked && dropMapTier >= 6u && !firstTributeGiven;
    }

    bool MapSystem::ConsumePortalOnDeathEscape(ActiveMapState& state) noexcept
    {
        if (!state.active || state.failed) {
            return false;
        }
        if (state.isArena4 || state.portalsRemaining == 0) {
            state.portalsRemaining = 0;
            state.failed = true;
            state.active = false;
            return false;
        }
        // PoE1 6포털 표준: 진입/재입장 시에만 포털을 1 소모하므로 사망 귀환 자체는 이중 차감하지 않고
        // 남은 포털(>0)을 유지해 재입장 가능 상태를 보장한다(6번째 마지막 포털에서 사망 시 위 분기에서 즉시 실패 종료).
        return true;
    }

    ArenaDamageClampResult MapSystem::ClampArenaBossDamage(
        std::uint8_t currentPhase,
        bool invulnActive,
        double currentHp,
        double maxHp,
        double incomingEffectiveDamage) noexcept
    {
        ArenaDamageClampResult res;
        if (incomingEffectiveDamage <= 0.0 || !std::isfinite(incomingEffectiveDamage)) {
            return res;
        }
        if (invulnActive) {
            res.clampedDamage = 0.0;
            res.blockedByInvuln = true;
            return res;
        }
        if (!std::isfinite(currentHp) || !std::isfinite(maxHp)) {
            res.clampedDamage = 0.0;
            return res;
        }
        if (maxHp <= 1.0 || currentPhase >= 3) {
            res.clampedDamage = incomingEffectiveDamage;
            return res;
        }

        const double floorHp = (currentPhase <= 1) ? (maxHp * 0.665) : (maxHp * 0.335);
        const std::uint8_t nextPhase = (currentPhase <= 1) ? 2 : 3;
        if (currentHp - incomingEffectiveDamage <= floorHp) {
            res.clampedDamage = std::max(0.0, currentHp - floorHp);
            res.triggeredNextPhase = nextPhase;
            return res;
        }
        res.clampedDamage = incomingEffectiveDamage;
        return res;
    }

    std::string MapSystem::FormatMapModKoreanLine(const RolledAffix& affix)
    {
        const int v = static_cast<int>(std::lround(FirstRollValue(affix)));
        const std::string_view id = affix.affixId;
        if (id == "MapMod_MonsterDamage")    return std::format("몬스터 피해 +{}%", v);
        if (id == "MapMod_ExtraFire")        return std::format("몬스터 물리 피해의 {}% 추가 화염", v);
        if (id == "MapMod_ExtraCold")        return std::format("몬스터 물리 피해의 {}% 추가 냉기", v);
        if (id == "MapMod_ExtraLightning")   return std::format("몬스터 물리 피해의 {}% 추가 번개", v);
        if (id == "MapMod_MonsterHaste")     return std::format("몬스터 이동·공격 속도 +{}% (가속)", v);
        if (id == "MapMod_MonsterCrit")      return std::format("몬스터 치명타 확률 +{}% (치명)", v);
        if (id == "MapMod_MonsterLife")      return std::format("몬스터 받는 피해 -{}%", v);
        if (id == "MapMod_MoreMagicRare")    return std::format("매직·레어 몬스터 등장 +{}%", v);
        if (id == "MapMod_MinusMaxRes")      return std::format("플레이어 최대 모든 저항 -{}%", v);
        if (id == "MapMod_LessArmour")       return std::format("플레이어 방어도 {}% 감폭", v);
        if (id == "MapMod_LessEvasion")      return std::format("플레이어 회피 {}% 감폭", v);
        if (id == "MapMod_LessRecovery")     return std::format("플레이어 ES 재충전 속도 {}% 감폭", v);
        if (id == "MapMod_MonsterElemRes")   return std::format("몬스터 원소 저항 +{}%", v);
        if (id == "MapMod_MonsterChaosRes")  return std::format("몬스터 카오스 저항 +{}%", v);
        if (id == "MapMod_Vulnerability")    return std::format("플레이어 받는 물리 피해 +{}%", v);
        if (id == "MapMod_PackSize")         return std::format("몬스터 무리 규모 +{}%", v);
        return affix.affixId;
    }
}
