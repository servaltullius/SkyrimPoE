#include "core/MapSystem.h"

#include "core/DataPack.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

namespace SkyrimPoE::Core
{
    namespace
    {
        // 결정 170 U-4: 상태이상 문턱 하한과 바닐라 타격 역산에 쓰는 스카이림 체력 기준값.
        constexpr double kMinAilmentThresholdPoe = 20.0;
        constexpr double kVanillaHitReferenceLife = 100.0;

        // 결정 166: houseCARL Skyrim.esm 실측 대조된 바닐라 던전 5종
        constexpr std::array<MapDungeonDef, kMapDungeonCount> kMapDungeons{ {
            MapDungeonDef{
                0, "EmbershardMine01", "엠버샤드 광산", "광산 · 산적",
                0x000B6BE6, 0x000CEED8, 0x000B6FEE,
                { 0x000B6DBB, 0x000B6FF7, 0x000B6FE4, 0x000B6FE6 }, 4,
                0x0003DECB, 0x0003DF17, 0
            },
            MapDungeonDef{
                1, "HaltedStreamCamp01", "멈춘 시냇가 야영지", "매머드 광산 · 산적",
                0x00043FAB, 0x000460EE, 0x000540A1,
                { 0x00053AE5, 0x00053AE4, 0x000540A2, 0 }, 3,
                0x0003DECB, 0x0003DF17, 0
            },
            MapDungeonDef{
                2, "RedoransRetreat01", "레도란의 은신처", "암석 동굴 · 산적",
                0x00015224, 0x000FD7A0, 0x00036CE0,
                { 0x00036CE1, 0x00036CE2, 0x00036CE3, 0 }, 3,
                0x0003DECB, 0x0003DF17, 0
            },
            MapDungeonDef{
                3, "BrokenFangCave01", "부러진 독니 동굴", "흡혈귀 소굴 · 언데드",
                0x000152B1, 0x00033BBE, 0x000267AC,
                { 0x000267FF, 0x000267B1, 0x00026747, 0 }, 3,
                0x0002F9E1, 0x0002F9E2, 0
            },
            MapDungeonDef{
                4, "ShimmermistCave01", "쉬머미스트 동굴", "팔머 · 드워머 유적",
                0x0001521C, 0x00048CB5, 0x000530C4,
                { 0x0004B688, 0x0004B594, 0x000530C1, 0 }, 3,
                0x0001E7B4, 0x0010F9B9, 0x0010EC89
            }
        } };

        // 결정 18, 166: 4차 전직 단판 3페이즈 결투장 (SilentMoonsCamp01 단일 홀)
        constexpr MapDungeonDef kArena4Dungeon{
            5, "SilentMoonsCamp01", "침묵의 달 제련소 (4차 결투장)", "4차 전직 결투장 · 3페이즈 보스",
            0x00015211, 0x00053FA6, 0x0005439A,
            { 0x0005439C, 0x0005439B, 0, 0 }, 2,
            0x00023A95, 0x00025D1D, 0x00023A95
        };

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

    double MapSystem::AilmentThreshold(std::uint32_t exileLevel, double lifeMultiplier, const DataPack& pack) noexcept
    {
        // 결정 170 U-4: 유배자 레벨로 기본 문턱을 구한 뒤 대상의 생명 배율을 적용한다.
        return std::max(kMinAilmentThresholdPoe, pack.MonsterLife(exileLevel)) * lifeMultiplier;
    }

    double MapSystem::EstimateVanillaHitPoe(double rawHitDamage, std::uint32_t exileLevel, const DataPack& pack) noexcept
    {
        // 결정 170 U-4 및 R3 브리프: 바닐라 타격은 유배자 레벨의 몬스터 생명으로 역산한다.
        return rawHitDamage * pack.MonsterLife(exileLevel) / kVanillaHitReferenceLife;
    }

    double MapActorScaling::ScaleIncomingDamage(double damage) const noexcept
    {
        // 결정 169·170 U-3: 생명 배율을 받는 피해에 역으로 적용한다.
        return damage / lifeMultiplier;
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

    std::uint8_t MapSystem::DungeonIndexForTier(std::uint8_t tier) noexcept
    {
        const auto clamped = std::clamp<std::uint8_t>(tier, kMinMapTier, kMaxMapTier);
        return static_cast<std::uint8_t>((clamped - 1) % kMapDungeonCount);
    }

    const MapDungeonDef& MapSystem::GetMapDungeon(std::uint8_t dungeonIndex) noexcept
    {
        return kMapDungeons[dungeonIndex % kMapDungeonCount];
    }

    const MapDungeonDef& MapSystem::GetArena4Dungeon() noexcept
    {
        return kArena4Dungeon;
    }

    const MapDungeonDef& MapSystem::GetDungeonForState(const ActiveMapState& state) noexcept
    {
        if (state.isArena4) {
            return kArena4Dungeon;
        }
        return GetMapDungeon(state.dungeonIndex);
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
        ++atlas.totalMapsCleared;
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
        st.dungeonIndex = DungeonIndexForTier(clampedTier);
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
        st.dungeonIndex = 5;
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
        if (!state.active || state.failed || state.isArena4 || state.portalsRemaining == 0) {
            return false;
        }
        --state.portalsRemaining;
        return true;
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
