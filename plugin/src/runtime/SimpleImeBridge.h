#pragma once

#include <cstdint>
#include <string_view>

namespace SkyrimPoE::Runtime::SimpleImeBridge
{
    inline constexpr const char* kMenuName = "SkyrimPoETextSink";
    inline constexpr const char* kSimpleImeMenuName = "SimpleImeMenu";

    enum class Mode : std::uint8_t
    {
        kAuto,  // SimpleIME.dll 로드 시에만 활성
        kOn,    // 강제 활성(시험용)
        kOff,   // 비활성
    };

    using TextCallback = void (*)(const char* utf8Text);
    using KeyCallback = void (*)(const char* keyName);

    [[nodiscard]] Mode ParseMode(std::string_view modeStr) noexcept;

    // kDataLoaded 이후 1회 호출: SimpleIME 유무 확인 및 깊이 12 투명 수신 메뉴 등록
    void Initialize(Mode mode, TextCallback onText, KeyCallback onKey);

    [[nodiscard]] bool Available() noexcept;

    // 주 갱신 스레드 전용: 검색 입력칸 포커스 진입(true) / 해제(false). 중복 호출은 무시한다.
    void SetTextEntry(bool active);

    [[nodiscard]] bool TextEntryActive() noexcept;

    // 입력 훅(메인 스레드)에서 호출: 편집 키의 눌림·홀드 반복·뗌을 SimpleIME 조합 2프레임 이력과 함께 전달한다.
    void OnGameKey(std::uint32_t scanCode, bool pressed, float heldSeconds);
}
