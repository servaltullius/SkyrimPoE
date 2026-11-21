#pragma once

#include "core/ItemData.h"
#include "core/MonsterModSystem.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace SkyrimPoE::Runtime
{
    struct StolenHeadhunterBuff
    {
        std::uint16_t modMask = 0;
        double remainingSec = 0.0;
    };

    // 5단계(결정 164): 매직·레어 몬스터 런타임 승격(0.5s 주 갱신 선승격), 적 ES 틱, 헤드헌터 60초 속성 훔치기
    class MonsterRuntime
    {
    public:
        static constexpr double kHeadhunterDurationSec = 60.0;

        static void Initialize(
            Core::StatEngine* statEngine,
            const Core::ItemData* itemData,
            const Core::Config* config);

        // 주 갱신 스레드 틱: 0.5s 주기 고프로세스 적대 액터 선승격 + 적 ES 재충전 + 헤드헌터 버프 타이머
        static void Tick(double deltaSec, bool gamePaused);

        // 스레드 안전 조회·적용(CombatHooks 워커 스레드 포함)
        [[nodiscard]] static std::optional<Core::MonsterProfile> GetProfileCopy(RE::FormID actorId);
        [[nodiscard]] static Core::MonsterIncomingResult ApplyPlayerHitToMonster(
            RE::FormID actorId,
            double rawSkyrimDamage,
            Core::DamageType dominantType,
            bool bypassesEs = false,
            RE::Actor* targetActor = nullptr,
            double engineDiffMult = 1.0);
        [[nodiscard]] static Core::MonsterOutgoingModifier GetOutgoingModifier(RE::FormID actorId);

        // DeathListener 주 갱신 스레드 호출: 처치된 적의 프로필 반환 및 헤드헌터 훔치기 실행
        [[nodiscard]] static Core::MonsterProfile OnMonsterKilled(RE::FormID actorId, bool playerHasHeadhunter);

        // 헤드헌터 활성 버프 상태 조회(HUD/AutoTest용)
        [[nodiscard]] static std::uint16_t ActiveHeadhunterModMask() noexcept;
        [[nodiscard]] static std::size_t ActiveHeadhunterStackCount() noexcept;
        [[nodiscard]] static std::size_t PromotedActorCount() noexcept;

        // AutoTest F6 결정적 승격 헬퍼
        static void ForcePromoteForTest(
            RE::Actor* actor,
            Core::MonsterRarity rarity,
            std::uint16_t modMask,
            double customMaxEs = -1.0);

        // 결정 114: TESResetEvent / 세이브 전환 시 액터·전체 초기화
        static void OnActorReset(RE::FormID actorId);
        // 6단계 안전 제거 정리용: 적용 중인 모든 Hasted SpeedMult 즉시 원복
        static void RevertAllHasteSpeedsNow();
        // Q-1: 세이브 시점에 적용 중인 Hasted SpeedMult 델타 스냅샷(순수 메모리 읽기, 엔진 호출 금지)
        [[nodiscard]] static std::vector<std::pair<RE::FormID, float>> TakeActiveSpeedDeltasForSave();
        static void ResetOnRevert();
    };
}
