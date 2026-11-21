#pragma once

#include "REL/Relocation.h"
#include "SKSE/Version.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace SkyrimPoE::Runtime::Addresses
{
    // 결정 141: 2단계 첫 조각은 AE 1.6.1170 우선, SE 1.5.97 주소는 추후 일괄 대조(미확인 시 안전 가드).
    inline constexpr REL::Version kSupportedAeVersion = SKSE::RUNTIME_SSE_1_6_1170;

    // 결정 124: AE 1.6.1170 ID 36544 + 0x160 -> Main::Update (ID 36564), 시그니처 void(RE::Main*)
    inline constexpr std::uint64_t kAeMainUpdateCallerId = 36544;
    inline constexpr std::ptrdiff_t kAeMainUpdateCallOffset = 0x160;
    inline constexpr std::uint64_t kAeMainUpdateTargetId = 36564;

    // 결정 65, 76, 80 (프로토타입 8·9: 정적 분석으로 확인. 실행으로 확인되지 않은 자리는 설계-현황 4장 참고)
    inline constexpr std::uint64_t kAeApplyHitId = 38586;
    inline constexpr std::uint64_t kAeModifyActorValueId = 35086;
    inline constexpr std::uint64_t kAeSetEffectivenessId = 34052;
    inline constexpr std::uint64_t kAeExplosionDamageStepsId = 44016;

    inline constexpr std::size_t kModifyActorValueVtableSlot = 0x20;
    inline constexpr std::size_t kHandleHealthDamageVtableSlot = 0x104;
    inline constexpr std::size_t kNotifyAnimationGraphVtableSlot = 0x1;

    struct CallSiteSpec
    {
        std::string_view name;
        std::uint64_t id;
        std::ptrdiff_t offset;
    };

    // HitData 적용(38586) 호출 자리 7곳 + 작업 큐 1곳
    // (C-14: melee·explosion·task_queue 등 주요 경로는 프로토타입 8·9 실게임 확인, 나머지 5곳은 Ghidra/IDA 대조·실게임 미확인)
    inline constexpr std::array<CallSiteSpec, 8> kAeApplyHitSites = {{
        { "melee",          38627, 0x4A8 },
        { "explosion",      43849, 0x412 },
        { "beam",           44123, 0x269 },
        { "nonactor_arrow", 44204, 0x392 },
        { "trap",           37509, 0x043 },
        { "collision",      38185, 0x21C },
        { "unknown",        49044, 0x191 },
        { "task_queue",     36991, 0xA3D },
    }};

    inline constexpr CallSiteSpec kAeExplosionStepsSite{ "Explosion::DamageSteps", 44004, 0xC1 };
    inline constexpr CallSiteSpec kAeSetEffectivenessSite{ "AddTarget::SetEffectiveness", 34547, 0x656 };

    // 35086(ValueModifierEffect::ModifyActorValue) 직접 호출 8곳 (프로토타입 9 정적 분석. 일부 자리는 실행 미확인, 설계-현황 4장)
    inline constexpr std::array<CallSiteSpec, 8> kAeMagicDirectCalls = {{
        { "DualValueModifierEffect+0x28",  34324, 0x28 },
        { "DualValueModifierEffect+0x5C",  34324, 0x5C },
        { "GrabActorEffect+0x34",          34332, 0x34 },
        { "GrabActorEffect+0x84",          34332, 0x84 },
        { "ValueAndConditionsEffect+0x36", 35067, 0x36 },
        { "ValueAndConditionsEffect+0x4A", 35067, 0x4A },
        { "ValueAndConditionsEffect+0x5C", 35067, 0x5C },
        { "ValueAndConditionsEffect+0x7E", 35067, 0x7E },
    }};

    [[nodiscard]] inline bool IsVerifiedRuntime(const REL::Version& ver) noexcept
    {
        return ver == kSupportedAeVersion;
    }
}
