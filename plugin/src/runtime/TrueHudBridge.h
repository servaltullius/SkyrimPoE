#pragma once

#include "core/Types.h"

namespace SkyrimPoE::Runtime
{
    // 결정 184 ④·186: TrueHUD 특수 바에 적 ES를 넘기는 선택 연동(INI TrueHudSpecialBar, 기본 끔).
    // 켜져 있으면 게임 데이터 준비 뒤 특수 바 소유권을 요청하고, 실제로 얻어 값 함수를 등록했을 때만 쓴다.
    // 그동안 자체 적 ES 표시(UiBridge)는 숨긴다. 못 얻거나 잃으면 자체 표시를 쓴다.
    class TrueHudBridge
    {
    public:
        // kDataLoaded 뒤 주 스레드에서 한 번 부른다(TrueHUD 권장: PostLoad 이후 요청).
        static void Initialize(const Core::Config* config);

        // 주 갱신 스레드에서 매 HUD 틱에 부른다. 소유 중이면 소유자가 아직 우리인지 보고(아니면 자체 표시로 돌아감),
        // 안전 제거 정리가 걸렸으면 바를 놓으며, 값 함수가 읽을 적 ES 표를 0.1초마다 새로 만든다.
        static void TickMainThread();
        static void SuspendSpecialBarOnMainThread();

        // 특수 바를 얻어 값 함수를 등록했고 아직 우리 것인지(어느 스레드에서나 읽음)
        [[nodiscard]] static bool OwnsSpecialBar() noexcept;
    };
}
