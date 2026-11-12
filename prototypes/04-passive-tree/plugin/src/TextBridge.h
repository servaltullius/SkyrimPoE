#pragma once

// SimpleIME와 함께 쓰는 글자 입력 다리.
//
// Prisma UI는 키보드를 게임 창의 창 메시지로만 받는다. SimpleIME가 켜져 있으면 키보드는 SimpleIME의 창으로 가고,
// SimpleIME는 받은 글자를 게임 창이 아니라 게임의 Scaleform 메뉴 쪽으로만 보낸다(글자 입력칸이 열려 있을 때만).
// 그래서 두 모드가 함께 켜져 있으면 Prisma UI 화면에는 글자가 닿지 않는다(프로토타입 4의 3·5차 시험).
//
// 이 다리는 그 사이를 잇는다.
//   1. 화면의 글자 입력칸이 포커스를 받으면 게임에 "글자 입력칸이 열렸다"고 알린다(ControlMap::AllowTextInput).
//      SimpleIME가 이것을 보고 입력기를 켠다. SimpleIME가 다른 모드에 안내하는 연동 방법이다.
//   2. 메뉴 하나를 열어, 메뉴들을 따라 내려오는 Scaleform 이벤트에서 글자를 받는다.
//   3. 편집 키(지우기, 방향키, Enter, Esc 등)는 그 길로 오지 않는다(6차 시험: 게임의 키 입력으로만 온다).
//      키 이벤트를 만들어 메뉴들에 흘려보내도 우리 메뉴까지 내려오지 않는다(7차 시험: 중간에서 막힘).
//      그래서 게임 입력에서 본 편집 키를 화면으로 바로 넘기되, 조합 중이면 넘기지 않는다(조합 중의 지우기가 이미 들어간
//      글자까지 지우지 않게). 조합 중인지는 SimpleIME의 메뉴에 아무 뜻 없는 키 이벤트를 직접 건네 보고 안다. SimpleIME의 메뉴는
//      조합 중일 때 키 이벤트를 "처리함"으로 돌려준다(아래 메뉴로 가지 않게 막는 것).
//      키를 누른 순간에 물으면 늦다. 입력기가 그 키를 먼저 받아 조합을 끝내 버린 뒤이기 때문이다(8차 시험: 조합 중인 낱자 하나를
//      지울 때 앞 글자도 지워짐). 그래서 프레임마다 물어 두고, 키가 왔을 때 직전 두 프레임의 답도 함께 본다.
//   4. 받은 것을 화면으로 넘긴다(넘기는 일은 등록된 콜백이 한다).
//
// SimpleIME가 없으면 아무 일도 하지 않는다. 그때는 Prisma UI가 창 메시지로 직접 받으므로 다리를 놓으면 글자가 두 번 들어간다.

#include <cstdint>

namespace TextBridge
{
    enum class Mode : std::uint8_t
    {
        kAuto,  // SimpleIME.dll이 로드돼 있을 때만 켠다
        kOn,    // 항상 켠다(시험용)
        kOff,   // 켜지 않는다
    };

    struct Options
    {
        Mode        mode = Mode::kAuto;
        bool        logEvents = false;  // 받은 글자와 키를 로그에 남긴다(친 글자가 그대로 남는다)
    };

    using TextCallback = void (*)(const char* utf8Text);
    using KeyCallback = void (*)(const char* keyName);

    // 게임 데이터가 불린 뒤(kDataLoaded) 한 번. 메뉴를 등록하고 SimpleIME가 있는지 본다.
    void Initialize(const Options& options, TextCallback onText, KeyCallback onKey);

    // 다리를 쓸 수 있는가(설정과 SimpleIME의 유무로 정해진다).
    [[nodiscard]] bool Available();

    // 메인 스레드에서. 글자 입력칸이 포커스를 받았을 때 true, 잃었을 때 false. 같은 값을 거듭 불러도 한 번만 반영한다.
    void SetTextEntry(bool active);

    [[nodiscard]] bool TextEntryActive();

    // 메인 스레드에서(게임의 입력 처리 중). 게임의 키 입력에서 본 키(키보드 DirectInput 스캔 코드)를 알린다.
    // 글자 입력칸이 열려 있고 편집 키이며 SimpleIME가 조합 중이 아니면 화면으로 넘긴다.
    // 누르고 있는 동안에는 heldSeconds로 되풀이를 정한다. pressed가 false면 뗀 것이다.
    void OnGameKey(std::uint32_t scanCode, bool pressed, float heldSeconds);
}
