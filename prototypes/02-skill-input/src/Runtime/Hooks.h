#pragma once

namespace poe
{
    // 플레이어의 NotifyAnimationGraph와 Update 가상 함수에 훅을 건다. 플러그인 로드 때 한 번.
    void InstallPlayerHooks();

    // 입력 장치 관리자에 읽기 전용 싱크를 붙인다. SKSE의 kInputLoaded 뒤에 한 번.
    void InstallInputSink();

    // 적중 이벤트를 받아 로그와 집계에 쓴다. SKSE의 kDataLoaded 뒤에 한 번.
    void InstallHitSink();
}
