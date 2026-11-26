#pragma once

#include "core/ItemData.h"
#include "core/MonsterModSystem.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <chrono>
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
            const Core::DataPack* dataPack,
            const Core::Config* config);

        // 주 갱신 스레드 틱: 0.5s 주기 고프로세스 적대 액터 선승격 + 적 ES 재충전 + 헤드헌터 버프 타이머
        static void Tick(double deltaSec, bool gamePaused);

        // 결정 185: 적 ES 표시용 마지막 교전 적(플레이어가 맞혔거나 플레이어를 맞힌 적). 아무 스레드에서나 부를 수 있다.
        // 대상과 시각을 64비트 원자 변수 하나에 함께 담아 서로 다른 교전이 섞이지 않는다.
        // 부르는 쪽이 실제 타격(지속 피해 틱·회피·막음 제외)이고 적(아군 아님)일 때만 부른다.
        struct EngagedEnemy
        {
            RE::FormID actorId = 0;
            double ageSec = -1.0;  // 기록이 없으면 음수
        };
        // 기록한 자리(시험 추적 기록에만 쓰임). 마법 기록은 주문과 효과의 FormID를 함께 넘긴다.
        enum class EngagementSource : std::uint8_t
        {
            PlayerSkill,       // DLL 스킬 직접 피해
            PlayerHit,         // 플레이어의 엔진 물리 타격
            PlayerMagic,       // 플레이어가 건 해로운 마법의 체력 감소
            IncomingHit,       // 적의 물리 피격(회피·막음 뒤)
            IncomingMagic,     // 적이 건 해로운 마법의 체력 감소(주문 회피·막음 뒤)
        };
        static void NoteEngagedEnemy(RE::FormID actorId, EngagementSource source, RE::FormID spellId = 0,
            RE::FormID effectId = 0) noexcept;
        [[nodiscard]] static EngagedEnemy GetLastEngagedEnemy() noexcept;
        // 시험 전용: 켜 둔 동안 기록 대상·자리·주문이 바뀔 때마다 한 줄, 같은 기록이 이어지면 끝날 때 반복 수를 남긴다.
        static void SetEngagementTraceForTest(bool enabled) noexcept;

        // 결정 186: ES가 있는(최대 ES > 0) 프로필의 현재·최대 ES 사본(TrueHUD 값 표를 만들 때 주 갱신 스레드가 씀)
        struct EnergyShieldEntry
        {
            RE::FormID actorId = 0;
            double currentEs = 0.0;
            double maxEs = 0.0;
        };
        [[nodiscard]] static std::vector<EnergyShieldEntry> ListEnergyShieldProfiles();

        // 스레드 안전 조회·적용(CombatHooks 워커 스레드 포함)
        [[nodiscard]] static std::optional<Core::MonsterProfile> GetProfileCopy(RE::FormID actorId);
        [[nodiscard]] static Core::MonsterIncomingResult ApplyPlayerHitToMonster(
            RE::FormID actorId,
            double rawSkyrimDamage,
            Core::DamageType dominantType,
            bool bypassesEs = false,
            RE::Actor* targetActor = nullptr,
            double engineDiffMult = 1.0,
            std::optional<std::uint64_t> generation = std::nullopt,
            const Core::DamagePacket* hitTypes = nullptr,
            bool isDamageOverTime = false,
            bool isAttack = false,
            const Core::StatSnapshot* hitSnapshot = nullptr);
        [[nodiscard]] static Core::MonsterOutgoingModifier GetOutgoingModifier(RE::FormID actorId);

        // DeathListener 주 갱신 스레드 호출: 처치된 적의 프로필 반환 및 헤드헌터 훔치기 실행
        [[nodiscard]] static Core::MonsterProfile OnMonsterKilled(RE::FormID actorId, bool playerHasHeadhunter);

        // 헤드헌터 활성 버프 상태 조회(HUD/AutoTest용)
        [[nodiscard]] static std::uint16_t ActiveHeadhunterModMask() noexcept;
        [[nodiscard]] static std::size_t ActiveHeadhunterStackCount() noexcept;
        [[nodiscard]] static std::size_t PromotedActorCount() noexcept;

        // 결정 259(B0): 자연 승격 게이트 검증용 누적 계측 카운터.
        struct PromotionGateStats
        {
            std::uint64_t gen1GateRejections = 0;
            std::uint64_t scanEntries = 0;
            std::uint64_t rollEntries = 0;
        };
        [[nodiscard]] static PromotionGateStats GetPromotionGateStatsForTest() noexcept;

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
