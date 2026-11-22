#pragma once

#include "core/CharmSystem.h"
#include "core/CommandQueue.h"
#include "core/EnergyShield.h"
#include "core/ItemData.h"
#include "core/ItemTypes.h"
#include "core/MapSystem.h"
#include "core/PassiveTreeData.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimPoE::Runtime
{
    struct MapActionResult
    {
        bool ok = false;
        std::string message;
    };

    struct MapKillContext
    {
        std::uint64_t runId = 0;
        std::uint32_t rewardMonsterLevel = 0;
        std::uint8_t dropMapTier = 0;
        double bonusIiqPct = 0.0;
        double bonusIirPct = 0.0;
        bool isBoss = false;
    };

    struct PlayerMapItemEntry
    {
        Core::InventoryItemKey key{};
        RE::FormID baseFormId = 0;
        std::uint8_t tier = 1;
        std::int32_t count = 1;
        bool tracked = false;
        Core::ItemRarity rarity = Core::ItemRarity::Normal;
        std::string displayName;
        std::vector<std::string> modLines;
        Core::MapModSummary summary{};
    };

    // 6단계(결정 166): 런타임 오버레이 지도 장치 · 4차 전직 3페이즈 보스 · 치명타 사망 가로채기 · 안전 제거 정리
    class MapDeviceRuntime
    {
    public:
        static void Initialize(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            const Core::ItemData* itemData,
            const Core::PassiveTreeData* treeData,
            const Core::Config* config,
            Core::EnergyShieldPool* esPool,
            Core::CharmSystem* charmSystem);
        static void BindDataPack(const Core::DataPack* dataPack);

        // 주 갱신 스레드 프레임 틱: 셀 소속 원자 갱신, 비동기 셀 로드 완료 후 스폰, 3페이즈 무적 타이머, 탈출 쿨다운
        static void Tick(double deltaSec, bool gamePaused);

        // 주 갱신 스레드 전용: 지도·4차 투기장 입장/재입장/귀환/포기
        static MapActionResult OpenNormalMap(
            RE::PlayerCharacter* player,
            const Core::InventoryItemKey& mapKey,
            RE::FormID baseFormId);
        static MapActionResult OpenArena4(RE::PlayerCharacter* player);
        static MapActionResult ReenterActiveMap(RE::PlayerCharacter* player);
        static MapActionResult ReturnPortal(RE::PlayerCharacter* player);
        static MapActionResult AbandonActiveMap(RE::PlayerCharacter* player);

        // 주 갱신 스레드 전용: 안전 제거 일괄 정리 절차(검수 C-4: 완료 후 g_uninstallCleanupArmed=true로 재오염 차단)
        static MapActionResult RunSafeUninstallCleanup(RE::PlayerCharacter* player);

        // 주 갱신 스레드 전용(DeathListener 호출): 지도 보스 클리어(아틀라스 +1, 첫 T6+ 공물) 및 4차 전직 보스 처치
        static void OnActorKilled(RE::FormID actorId, RE::Actor* victim, std::uint64_t runId);

        // 스레드 안전(CombatHooks / MonsterRuntime 호출): 4차 전직 3페이즈 보스 체력 하한 동기 클램프 + 전환 예약
        // rawSkyrimDamage는 난이도 배율 적용 전 피해량이며, diffMult는 실제 체력 차감 배율(fDiffMultHPByPC)이다.
        [[nodiscard]] static double FilterArena4BossIncomingDamage(
            RE::Actor* boss,
            double rawSkyrimDamage,
            double diffMult);

        // 락프리 원자 조회(CombatHooks 워커 스레드 안전): 지도/결투장 내부 치명타 가로채기
        // true 반환 시 호출부는 들어오는 피해를 즉시 0.0f로 무효화해야 한다.
        [[nodiscard]] static bool TryInterceptLethalHitFromHook(
            double curHp,
            double effectiveDamage);

        // 락프리 원자 상태 조회
        [[nodiscard]] static bool IsPlayerInActiveMapCell() noexcept;
        [[nodiscard]] static bool IsActiveMapRun() noexcept;
        [[nodiscard]] static bool IsActiveMapCell(RE::FormID cellFormId) noexcept;
        [[nodiscard]] static std::uint8_t GetActiveMapTier() noexcept;
        [[nodiscard]] static bool IsActiveMapOrArenaBoss(RE::FormID actorFormId) noexcept;
        [[nodiscard]] static bool IsActiveArena4Boss(RE::FormID actorFormId) noexcept;
        [[nodiscard]] static bool IsDeathEscapeInFlight() noexcept;
        [[nodiscard]] static bool IsUninstallCleanupArmed() noexcept;
        static void DisarmUninstallCleanup() noexcept;

        // 뮤텍스 보호 스냅샷 조회
        [[nodiscard]] static Core::ActiveMapState GetActiveMapStateCopy();

        // 전용 잎 잠금으로 회차 소유와 해당 회차의 어픽스·보상 정보를 일관되게 읽는다.
        [[nodiscard]] static Core::MapActorScaling GetMapActorScaling(
            RE::FormID actorId, Core::MapModSummary* mods = nullptr);
        [[nodiscard]] static MapKillContext GetMapKillContext(RE::FormID actorId);

        // 주 갱신 스레드 전용: 인벤토리 보유 지도 목록·4차 공물 수량·안전 텔레포트 래퍼
        [[nodiscard]] static std::vector<PlayerMapItemEntry> ListPlayerMaps(RE::PlayerCharacter* player);
        [[nodiscard]] static std::int32_t CountPlayerTributes(RE::PlayerCharacter* player);
        static bool MovePlayerToSpace(
            RE::PlayerCharacter* player,
            RE::TESObjectCELL* targetCell,
            RE::TESWorldSpace* targetWorldspace,
            const RE::NiPoint3& pos,
            const RE::NiPoint3& angle);

        // Layer2Input에 플레이어의 지도 셀 소속과 불이익을 동기화
        static void SyncMapLayer2ToStatEngine();

        // SKSE 코세이브 'MAPS' v2 스냅샷/복원/초기화(순수 메모리) 및 PostLoad 재바인딩
        [[nodiscard]] static Core::ActiveMapState TakeSnapshotForSave();
        static void RestoreFromSave(Core::ActiveMapState saved, RE::FormID legacyCell);
        static void OnPostLoadRebind();
        static void ResetOnRevert();
    };
}
