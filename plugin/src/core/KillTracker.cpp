#include "core/KillTracker.h"

#include <algorithm>
#include <cmath>

namespace SkyrimPoE::Core
{
    namespace
    {
        void ApplyXpToGemTree(GemInstance& gem, std::uint64_t gemXp, GemLevelUpSummary& summary, std::uint32_t depth = 0) noexcept
        {
            if (depth > 4 || gem.gemId.empty()) {
                return;
            }
            if (gemXp > 0) {
                if (gem.level < kMaxGemLevel) {
                    ++summary.gemsGainedXp;
                    gem.xp += gemXp;
                    while (gem.level < kMaxGemLevel) {
                        const auto req = KillTracker::RequiredXpForNextGemLevel(gem.level);
                        if (gem.xp < req) {
                            break;
                        }
                        gem.xp -= req;
                        ++gem.level;
                        ++summary.gemLevelsGained;
                    }
                    if (gem.level >= kMaxGemLevel) {
                        gem.xp = 0;
                    }
                }
            }
            const std::size_t maxSockets = std::min<std::size_t>(gem.socketedGems.size(), gem.socketCount);
            for (std::size_t i = 0; i < maxSockets; ++i) {
                ApplyXpToGemTree(gem.socketedGems[i], gemXp, summary, depth + 1);
            }
        }
    }

    KillTracker::DirectHitScope::DirectHitScope(KillTracker* tracker, std::uint32_t actorFormId)
        : tracker_(tracker), actorFormId_(actorFormId)
    {
        if (!tracker_) {
            return;
        }
        std::scoped_lock lock(tracker_->mutex_);
        const auto it = tracker_->records_.find(actorFormId_);
        if (it == tracker_->records_.end() || it->second.deathStarted || it->second.rewardClaimed) {
            return;
        }
        auto& rec = it->second;
        if (rec.directHitDepth++ == 0) {
            rec.directHitScopeId = ++tracker_->seqCounter_;
        }
        scopeId_ = rec.directHitScopeId;
    }

    KillTracker::DirectHitScope::~DirectHitScope()
    {
        if (scopeId_ == 0) {
            return;
        }
        std::scoped_lock lock(tracker_->mutex_);
        const auto it = tracker_->records_.find(actorFormId_);
        // 리셋이나 FormID 재사용 뒤에 시작한 별도 피해의 표시는 건드리지 않는다.
        if (it != tracker_->records_.end() && it->second.directHitScopeId == scopeId_ &&
            --it->second.directHitDepth == 0) {
            it->second.directHitScopeId = 0;
        }
    }

    void KillTracker::RecordFirstHitHostility(std::uint32_t actorFormId, bool wasHostileToPlayer, bool actorAliveForNewLife)
    {
        if (actorFormId == 0) {
            return;
        }
        std::scoped_lock lock(mutex_);
        auto& rec = records_[actorFormId];
        // C-12: 보상 종료된 기록에 살아 있는 액터가 다시 피격되면 새 삶(부활/FF FormID 재사용)으로 lifeGeneration 갱신
        if (rec.rewardClaimed && actorAliveForNewLife) {
            const auto nextGen = rec.lifeGeneration + 1;
            rec = ActorLifeRecord{};
            rec.lifeGeneration = nextGen;
        }
        // 사망 시작/확정 이후(시체 타격 등)에는 첫 적대나 기여를 새로 기록하지 않는다.
        if (rec.deathStarted || rec.rewardClaimed) {
            return;
        }
        rec.lastTouchedSeq = ++seqCounter_;
        if (!rec.firstHostilityRecorded) {
            rec.firstHostilityRecorded = true;
            rec.wasHostileOnFirstHit = wasHostileToPlayer;
        }
    }

    void KillTracker::ConfirmDamageContribution(std::uint32_t actorFormId, double actualLoss)
    {
        if (actorFormId == 0 || !std::isfinite(actualLoss) || actualLoss <= 0.0) {
            return;
        }
        std::scoped_lock lock(mutex_);
        auto& rec = records_[actorFormId];
        if (rec.rewardClaimed) {
            return;
        }
        // Q-2: 한 방 처치로 deathStarted가 먼저 켜졌더라도 직전 첫 피격 적대가 이미 기록된 동일 타격이면 기여 확정 허용
        if (rec.deathStarted && !rec.firstHostilityRecorded) {
            return;
        }
        rec.lastTouchedSeq = ++seqCounter_;
        rec.playerContributed = true;
    }

    void KillTracker::OnDeathStart(std::uint32_t actorFormId, bool isSummoned, bool isReanimated, bool isCommanded)
    {
        if (actorFormId == 0) {
            return;
        }
        std::scoped_lock lock(mutex_);
        // C-12: 이미 보상이 끝난 오래된 기록(기여 여부 무관)을 상한 관리로 정리해 메모리 누적 및 FF 재사용 충돌을 줄인다.
        if (records_.size() > 2048) {
            const std::uint64_t cutoff = (seqCounter_ > 1024) ? (seqCounter_ - 1024) : 0;
            std::erase_if(records_, [cutoff](const auto& kv) {
                return kv.second.rewardClaimed && kv.second.lastTouchedSeq <= cutoff;
            });
        }
        auto& rec = records_[actorFormId];
        rec.lastTouchedSeq = ++seqCounter_;
        if (!rec.deathStarted) {
            rec.deathStarted = true;
            rec.deathExcluded = (isSummoned || isReanimated || isCommanded);
        }
    }

    KillRewardResult KillTracker::OnDeathConfirm(
        std::uint32_t actorFormId,
        std::uint32_t enemySkyrimLevel,
        std::uint32_t playerSkyrimLevel,
        bool isBoss,
        bool isDragon,
        std::uint32_t currentExileLevel,
        const Config& config,
        bool fallbackExcluded,
        std::uint32_t overrideEffectiveMonsterLevel)
    {
        KillRewardResult res;
        if (actorFormId == 0) {
            return res;
        }

        bool eligible = false;
        {
            std::scoped_lock lock(mutex_);
            const auto it = records_.find(actorFormId);
            if (it == records_.end()) {
                return res;
            }
            auto& rec = it->second;
            const bool excluded = rec.deathStarted ? rec.deathExcluded : fallbackExcluded;
            res.playerContributed = rec.playerContributed || rec.directHitDepth != 0;
            if (!rec.rewardClaimed &&
                res.playerContributed &&
                rec.firstHostilityRecorded &&
                rec.wasHostileOnFirstHit &&
                !excluded) {
                eligible = true;
                rec.playerContributed = true;
            }
            // 중복 사망 확정 이벤트 방지를 위해 지급 여부와 무관하게 청구 완료 표시
            rec.rewardClaimed = true;
            rec.deathStarted = true;
            if (!rec.playerContributed && records_.size() > 1024) {
                records_.erase(it);
            }
        }

        if (!eligible) {
            return res;
        }

        const std::uint32_t lv = std::clamp<std::uint32_t>(currentExileLevel, 1, kMaxExileLevel);
        res.effectiveMonsterLevel = (overrideEffectiveMonsterLevel > 0u)
            ? std::clamp<std::uint32_t>(overrideEffectiveMonsterLevel, 1, kMaxExileLevel)
            : ComputeEffectiveMonsterLevel(
                  lv, enemySkyrimLevel, playerSkyrimLevel, isBoss, isDragon, config);
        res.xpMultiplier = ComputeLevelDiffXpMultiplier(lv, res.effectiveMonsterLevel, config);

        const double rawXp = std::max(0.0, config.baseKillXp) * res.xpMultiplier;
        res.exileXpGained = static_cast<std::uint64_t>(std::llround(rawXp));
        if (res.exileXpGained == 0 && rawXp > 0.0) {
            res.exileXpGained = 1;
        }
        const double rawGemXp = static_cast<double>(res.exileXpGained) * std::max(0.0, config.gemXpRatio);
        res.gemXpGained = static_cast<std::uint64_t>(std::llround(rawGemXp));
        if (res.gemXpGained == 0 && rawGemXp > 0.0) {
            res.gemXpGained = 1;
        }
        res.awarded = true;
        return res;
    }

    void KillTracker::OnActorReset(std::uint32_t actorFormId)
    {
        if (actorFormId == 0) {
            return;
        }
        std::scoped_lock lock(mutex_);
        records_.erase(actorFormId);
    }

    void KillTracker::ClearOnRevert()
    {
        std::scoped_lock lock(mutex_);
        records_.clear();
    }

    std::size_t KillTracker::TrackedActorCount() const
    {
        std::scoped_lock lock(mutex_);
        return records_.size();
    }

    ActorKillRecordSnapshot KillTracker::GetActorRecordSnapshot(std::uint32_t actorFormId) const
    {
        ActorKillRecordSnapshot snap;
        if (actorFormId == 0) {
            return snap;
        }
        std::scoped_lock lock(mutex_);
        const auto it = records_.find(actorFormId);
        if (it == records_.end()) {
            return snap;
        }
        snap.found = true;
        snap.firstHostilityRecorded = it->second.firstHostilityRecorded;
        snap.wasHostileOnFirstHit = it->second.wasHostileOnFirstHit;
        snap.playerContributed = it->second.playerContributed;
        snap.deathStarted = it->second.deathStarted;
        snap.deathExcluded = it->second.deathExcluded;
        snap.rewardClaimed = it->second.rewardClaimed;
        snap.directHitActive = it->second.directHitDepth != 0;
        snap.lifeGeneration = it->second.lifeGeneration;
        return snap;
    }

    std::uint32_t KillTracker::ComputeEffectiveMonsterLevel(
        std::uint32_t exileLevel,
        std::uint32_t enemySkyrimLevel,
        std::uint32_t playerSkyrimLevel,
        bool isBoss,
        bool isDragon,
        const Config& config) noexcept
    {
        const std::int32_t L = static_cast<std::int32_t>(std::clamp<std::uint32_t>(exileLevel, 1, kMaxExileLevel));
        const std::int32_t diffRaw = static_cast<std::int32_t>(enemySkyrimLevel) - static_cast<std::int32_t>(playerSkyrimLevel);
        const std::int32_t clampLimit = std::max<std::int32_t>(0, config.levelDiffClamp);
        const std::int32_t diff = std::clamp(diffRaw, -clampLimit, clampLimit);

        std::int32_t bonus = 0;
        if (isBoss) {
            bonus += std::max<std::int32_t>(0, config.bossLevelBonus);
        }
        if (isDragon) {
            bonus += std::max<std::int32_t>(0, config.bossLevelBonus);
        }
        bonus = std::min(bonus, std::max<std::int32_t>(0, config.maxBossLevelBonus));

        const std::int32_t eff = std::clamp<std::int32_t>(L + diff + bonus, 1, static_cast<std::int32_t>(kMaxExileLevel));
        return static_cast<std::uint32_t>(eff);
    }

    double KillTracker::ComputeLevelDiffXpMultiplier(
        std::uint32_t exileLevel,
        std::uint32_t effectiveMonsterLevel,
        const Config& config) noexcept
    {
        const std::uint32_t L = std::clamp<std::uint32_t>(exileLevel, 1, kMaxExileLevel);
        const std::uint32_t M = std::clamp<std::uint32_t>(effectiveMonsterLevel, 1, kMaxExileLevel);

        if (M == L) {
            return 1.0;
        }
        if (M > L) {
            // 결정 136, 146: 높은 적은 레벨당 +10% (상한 2배)
            const double delta = static_cast<double>(M - L);
            const double mult = 1.0 + delta * std::max(0.0, config.higherEnemyBonusPerLevel);
            const double cap = std::max(1.0, config.higherEnemyMaxMultiplier);
            return std::min(cap, mult);
        }

        // 결정 146: 낮은 적은 PoB 2.68.1 벌칙 식(안전 폭 3 + floor(L/16), d <= 0 -> 1.0, 하한 1%)
        const std::int32_t safeZone = 3 + static_cast<std::int32_t>(L / 16);
        const std::int32_t d = static_cast<std::int32_t>(L - M) - safeZone;
        if (d <= 0) {
            return 1.0;
        }
        const double lPlus5 = static_cast<double>(L) + 5.0;
        const double denom = lPlus5 + std::pow(static_cast<double>(d), 2.5);
        const double penalty = std::pow(lPlus5 / denom, 1.5);
        const double floorMult = std::clamp(config.lowerEnemyMinMultiplier, 0.0, 1.0);
        if (!std::isfinite(penalty)) {
            return floorMult;
        }
        return std::clamp(penalty, floorMult, 1.0);
    }

    std::uint64_t KillTracker::RequiredXpForNextExileLevel(
        std::uint32_t exileLevel,
        const Config& config) noexcept
    {
        const std::uint32_t L = std::clamp<std::uint32_t>(exileLevel, 1, kMaxExileLevel);
        // 결정 147: kills(L) = 10 + 2 * (L - 1), 기본 처치 XP 100
        const double kills = std::max(1.0, config.killsBase + config.killsSlope * static_cast<double>(L - 1));
        const double req = kills * std::max(1.0, config.baseKillXp);
        return static_cast<std::uint64_t>(std::max<long long>(1, std::llround(req)));
    }

    ExileLevelUpResult KillTracker::ApplyExileXp(
        std::uint32_t& exileLevel,
        std::uint64_t& exileXp,
        std::uint32_t& unallocatedPassivePoints,
        std::uint64_t gainedXp,
        const Config& config) noexcept
    {
        ExileLevelUpResult out;
        exileLevel = std::clamp<std::uint32_t>(exileLevel, 1, kMaxExileLevel);
        out.newLevel = exileLevel;
        if (exileLevel >= kMaxExileLevel || gainedXp == 0) {
            if (exileLevel >= kMaxExileLevel) {
                exileXp = 0;
            }
            return out;
        }

        exileXp += gainedXp;
        while (exileLevel < kMaxExileLevel) {
            const auto req = RequiredXpForNextExileLevel(exileLevel, config);
            if (exileXp < req) {
                break;
            }
            exileXp -= req;
            ++exileLevel;
            ++unallocatedPassivePoints;
            ++out.levelsGained;
        }
        if (exileLevel >= kMaxExileLevel) {
            exileXp = 0;
        }
        out.newLevel = exileLevel;
        return out;
    }

    std::uint64_t KillTracker::RequiredXpForNextGemLevel(std::uint8_t gemLevel) noexcept
    {
        const std::uint32_t lv = std::clamp<std::uint32_t>(gemLevel, 1, kMaxGemLevel);
        // 2단계 선형 임시 젬 경험치 곡선: 젬이 처치 XP의 10%를 받으므로 약 10~50마리당 성장하도록 설정
        return static_cast<std::uint64_t>(100 + 80 * (lv - 1));
    }

    GemLevelUpSummary KillTracker::ApplyGemXpToSlots(
        std::array<SkillSlot, kSkillSlotCount>& slots,
        std::uint64_t gemXp) noexcept
    {
        GemLevelUpSummary summary;
        if (gemXp == 0) {
            return summary;
        }
        for (auto& slot : slots) {
            if (!slot.occupied) {
                continue;
            }
            ApplyXpToGemTree(slot.gem, gemXp, summary, 0);
        }
        return summary;
    }
}
