#pragma once

#include "core/ItemTypes.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace SkyrimPoE::Core
{
    inline constexpr std::uint8_t kMinMapTier = 1;
    inline constexpr std::uint8_t kMaxMapTier = 16;
    inline constexpr std::uint32_t kMapBaseMonsterLevel = 68;   // T1 = Lv68 .. T16 = Lv83
    inline constexpr std::uint32_t kArena4MonsterLevel = 75;    // 4차 전직 결투장 고정 레벨
    inline constexpr std::uint8_t kNormalMapMaxPortals = 6;
    inline constexpr std::uint8_t kArena4MaxPortals = 1;
    // 결정 169, R4: 목록과 MAPS v2에 같은 던전 키 길이 제한을 둔다.
    inline constexpr std::size_t kMaxMapDungeonKeyBytes = 64;
    inline constexpr std::size_t kMaxMapSpawnedActors = 64;

    // 활성 지도 어픽스 16종 집계 스냅샷 (StatEngine Layer2Input / MonsterRuntime / CombatHooks 공용)
    struct MapModSummary
    {
        double monsterDamageIncPct = 0.0;       // 몬스터 주는 피해 +%
        double monsterExtraFirePct = 0.0;       // 몬스터 물리→화염 추가 %
        double monsterExtraColdPct = 0.0;       // 몬스터 물리→냉기 추가 %
        double monsterExtraLightningPct = 0.0;  // 몬스터 물리→번개 추가 %
        bool forceHasted = false;               // 몬스터 전원 가속
        double monsterHasteSpeedBonus = 0.0;    // 몬스터 가속 이동·공격 속도 +% (롤링값 반영)
        bool forceDeadly = false;               // 몬스터 전원 치명타 강화
        double monsterCritChanceIncPct = 0.0;   // 몬스터 치명타 확률 +% (롤링값 반영)
        double monsterReducedTakenPct = 0.0;    // 몬스터 받는 피해 -% (고체력)
        double moreMagicRarePct = 0.0;          // 매직/레어 승격 확률 +%
        double playerMaxResPenalty = 0.0;       // 플레이어 최대 모든 저항 -% (양수 크기, 예: 10.0 = -10%)
        double playerLessArmourPct = 0.0;       // 플레이어 방어도 감폭 %
        double playerLessEvasionPct = 0.0;      // 플레이어 회피 감폭 %
        double playerLessEsRechargePct = 0.0;   // 플레이어 ES 재충전 속도 감폭 %
        double monsterElemResPct = 0.0;         // 몬스터 원소 저항 +%
        double monsterChaosResPct = 0.0;        // 몬스터 카오스 저항 +%
        double playerPhysTakenIncPct = 0.0;     // 플레이어 받는 물리 피해 +%
        double packSizePct = 0.0;               // 몬스터 무리 규모 +%
        double bonusIiqPct = 0.0;               // 전리품 수량(IIQ) +%
        double bonusIirPct = 0.0;               // 전리품 희귀도(IIR) +%

        [[nodiscard]] bool operator==(const MapModSummary&) const noexcept = default;
    };

    // 아틀라스 영구 진행 상태 ('MAPS' v2 코세이브 저장)
    struct AtlasState
    {
        std::uint16_t completedTierMask = 0;  // bit 0..15 = T1..T16 첫 클리어 (+1 패시브/티어, 최대 16점)
        std::uint32_t totalMapsCleared = 0;
        std::uint8_t highestTierCleared = 0;
        bool firstTributeGiven = false;       // 첫 T6+ 보스 처치 시 4차 공물 확정 1회 지급 여부
    };

    // 활성 지도/결투장 실행 상태 ('MAPS' v2 코세이브 저장 + 런타임 상태머신)
    struct ActiveMapState
    {
        bool active = false;
        bool isArena4 = false;
        bool bossKilled = false;
        bool failed = false;
        std::uint64_t runId = 0;
        std::uint8_t tier = 1;                // 1..16 (결투장은 8 표시, 레벨 75)
        std::string dungeonKey;
        std::uint32_t packVersion = 0;
        std::uint8_t portalsRemaining = 0;
        std::uint8_t arenaPhase = 1;          // 1..3 (4차 결투장 보스 페이즈)
        std::vector<RolledAffix> mods;
        MapModSummary summary{};

        // 검수 C-1: 귀환 좌표 영속화 (인테리어 셀 또는 Tamriel 등 야외 월드스페이스)
        std::uint32_t returnCellFormId = 0;
        std::uint32_t returnWorldspaceFormId = 0;
        std::array<float, 3> returnPos{ 0.0f, 0.0f, 0.0f };
        std::array<float, 3> returnAngle{ 0.0f, 0.0f, 0.0f };

        // 검수 C-3: 저장→재로드 후 보스 처치/3페이즈/정리 연속성 보장을 위한 동적 스폰 FormID 영속화
        std::uint32_t bossRefFormId = 0;
        std::vector<std::uint32_t> spawnedRefFormIds;
    };

    struct ArenaDamageClampResult
    {
        double clampedDamage = 0.0;
        std::uint8_t triggeredNextPhase = 0;  // 0 = 전환 없음, 2 = 2페이즈 진입, 3 = 3페이즈 진입
        bool blockedByInvuln = false;
    };
}
