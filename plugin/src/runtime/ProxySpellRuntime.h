#pragma once

#include "core/CommandQueue.h"
#include "core/DataPack.h"
#include "core/SkillSystem.h"
#include "core/StatEngine.h"
#include "core/Types.h"

#include <cstdint>

namespace SkyrimPoE::Runtime
{
    // 5단계(결정 88~91, 163, 프로토타입 10 검증):
    // - SPOE_ProxySpell_1..9 / SPOE_ProxyPower_1..9 레코드 동기화(중복 이름 [칸 N] 접미사, OnRevert 일괄 초기화)
    // - RE::VTABLE_ActorMagicCaster[0] CheckCast(0x0A) + SpellCast(0x09) 체인 훅(DMMF 공존)
    // - 멀티스레드 안전 CheckCast 관문 허가 플래그 -> SpellCast에서 CommandQueue 주 갱신 이연 실행
    // - Molten Shell 가드 버프 틱·만료/파괴 반사 화염 폭발·StatEngine Layer2 동기화
    class ProxySpellRuntime
    {
    public:
        // 시험 진단용: 플레이어의 CheckCast·SpellCast 판정을 기록한다(AutoTest만 켬).
        static void SetCastTraceForTest(bool enabled);

        // 시험 진단용(W8): ExecuteHandCastSlot이 DLL에서 직접 매지카를 차감한 횟수와 마지막 금액.
        struct MagickaChargeDiagnostics
        {
            std::uint64_t chargeCount = 0;
            double lastChargedAmount = 0.0;
            std::uint8_t lastChargedSlot = 0;
        };
        [[nodiscard]] static MagickaChargeDiagnostics GetMagickaChargeDiagnostics() noexcept;

        // 대리 주문·파워 가운데 GetCostliestEffectItem()이 null인 수를 기록한다(다른 모드 호환 진단).
        static void EnsureProxyCostliestEffects();

        static bool Install(
            Core::CommandQueue* queue,
            Core::StatEngine* statEngine,
            Core::SkillSystem* skillSystem,
            const Core::DataPack* dataPack,
            const Core::Config* config);

        // 주 갱신 스레드 전용: 현재 PersistentState 슬롯(0..8)과 StatSnapshot을 18개 대리 레코드에 반영
        static void SyncProxySpellsFromState();
        // 6단계 안전 제거 정리용: 플레이어에게 부여된 대리 주문·파워 18종 일괄 제거
        static void RemoveAllProxySpells(RE::PlayerCharacter* player);

        // 주 갱신 스레드 틱: Molten Shell 가드 버프 타이머·만료 폭발·Layer2 방어도 동기화
        static void Tick(double deltaSec, bool gamePaused);

        // AutoTest F5 및 내부 공용 직접 시전/가드 발동 헬퍼(주 갱신 스레드 전용)
        static bool ExecuteHandCastSlot(
            std::uint8_t slotIndex,
            bool chargeMagicka,
            const RE::NiPoint3* capturedOrigin = nullptr,
            const RE::NiPoint3* capturedDir = nullptr,
            bool dualCast = false);
        static bool ExecuteGuardPowerSlot(std::uint8_t slotIndex);

        [[nodiscard]] static std::uint64_t HandCastCount() noexcept;
        [[nodiscard]] static std::uint64_t GuardActivationCount() noexcept;
        static void ResetOnRevert();
    };
}
