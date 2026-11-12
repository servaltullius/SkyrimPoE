#pragma once

namespace poe
{
    // 플레이어의 NotifyAnimationGraph와 Update 가상 함수에 훅을 건다. 플러그인 로드 때 한 번.
    void InstallPlayerHooks();

    // 입력 장치 관리자에 읽기 전용 싱크를 붙인다. SKSE의 kInputLoaded 뒤에 한 번.
    void InstallInputSink();
}
