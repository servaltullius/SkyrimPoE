#pragma once

#include "core/MapTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SkyrimPoE::Core
{
    class DataPack;

    struct MapMonsterMultipliers
    {
        double lifeMultiplier = 1.0;
        double damageMultiplier = 1.0;
    };

    struct MapActorScaling
    {
        std::uint64_t runId = 0;
        double lifeMultiplier = 1.0;
        double damageMultiplier = 1.0;

        [[nodiscard]] double ScaleIncomingDamage(double damage) const noexcept;
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
        [[nodiscard]] static std::uint8_t DungeonIndexForTier(std::uint8_t tier) noexcept;
        [[nodiscard]] static const MapDungeonDef& GetMapDungeon(std::uint8_t dungeonIndex) noexcept;
        [[nodiscard]] static const MapDungeonDef& GetArena4Dungeon() noexcept;
        [[nodiscard]] static const MapDungeonDef& GetDungeonForState(const ActiveMapState& state) noexcept;

        // 지도 어픽스 16종 집계 + IIQ/IIR/PackSize 보상 합산
        [[nodiscard]] static MapModSummary AggregateMapMods(const std::vector<RolledAffix>& affixes) noexcept;

        // 아틀라스 +24 패시브 포인트 계산 (T1~T16 첫 클리어 최대 16점 + 1~4차 보스/결투장 이정표 최대 8점)
        [[nodiscard]] static std::uint32_t CountAtlasTierPoints(std::uint16_t completedTierMask) noexcept;
        [[nodiscard]] static std::uint32_t CountMilestonePassivePoints(std::uint32_t ascendancyMilestonesBitmask) noexcept;
        [[nodiscard]] static double AtlasBonusIiqPct(std::uint16_t completedTierMask) noexcept;
        [[nodiscard]] static double AtlasBonusIirPct(std::uint16_t completedTierMask) noexcept;

        // 지도 보스 클리어 반영: 해당 티어가 첫 클리어이면 true 반환
        static bool RecordMapClear(AtlasState& atlas, std::uint8_t tier) noexcept;

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

        // 재입장 시 포털 1 소모 (포털 0이거나 실패 상태면 false)
        [[nodiscard]] static bool TryConsumePortalForReenter(ActiveMapState& state) noexcept;

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
