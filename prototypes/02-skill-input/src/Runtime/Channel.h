#pragma once

#include "Core/Config.h"

#include <RE/Skyrim.h>

#include <chrono>
#include <mutex>
#include <string>
#include <string_view>

namespace poe
{
    // 채널링(사이클론) 한 번의 시작부터 끝까지를 맡는다.
    // 입력은 읽기만 하고 게임에 가는 입력 이벤트를 고치지 않는다. 게임이 강공격을 시작하려고 보내는
    // 애니메이션 이벤트를 받지 않게 하는 것이 유일한 개입이다.
    //
    // 스레드: OnButton, Tick, OnLoad는 메인 스레드에서만 불린다(입력 배분, 플레이어 Update, SKSE 메시지).
    // OnNotify는 어느 스레드에서든 불릴 수 있으므로 잠금을 기다리지 않는다. 잠겨 있으면 이벤트를 그대로 통과시킨다.
    class Channel
    {
    public:
        static Channel& Get();

        void Configure(const Config& config);
        void SetBackends(bool oarAvailable, bool injectorAvailable);

        // 입력 싱크에서 버튼 이벤트 하나마다. pressed = 눌려 있음, first = 이번에 처음 눌림.
        void OnButton(RE::INPUT_DEVICE device, std::uint32_t idCode, bool pressed, bool first);

        // 플레이어의 NotifyAnimationGraph 훅에서. true를 돌려주면 그 이벤트를 게임에 넘기지 않는다.
        bool OnNotify(const RE::BSFixedString& event);

        // 플레이어 Update가 끝난 뒤 매 프레임.
        void Tick();

        // 세이브 로드 직전(playerReady = false)과 로드·새 게임 직후(playerReady = true).
        void OnLoad(bool playerReady);

        // 이 DLL이 직접 보내는 애니메이션 이벤트가 훅을 지나는 중인지.
        static bool IsSending();

    private:
        using Clock = std::chrono::steady_clock;

        enum class Key
        {
            kNone,
            kRight,  // 오른손 공격에 매핑된 버튼
            kLeft,   // 왼손 공격·방어에 매핑된 버튼
            kExtra,  // INI의 ExtraChannelKey
        };

        struct KeyState
        {
            bool              down = false;
            bool              eligible = false;  // 게임이 멈춰 있지 않을 때 눌렀는가
            RE::INPUT_DEVICE  device = RE::INPUT_DEVICE::kNone;
            std::uint32_t     idCode = 0;
            Clock::time_point downAt{};
            Clock::time_point lastSeen{};  // 눌려 있다는 이벤트를 마지막으로 받은 때
        };

        Key             Classify(RE::INPUT_DEVICE device, std::uint32_t idCode) const;
        Key             FindDown(RE::INPUT_DEVICE device, std::uint32_t idCode) const;
        KeyState&       State(Key key);
        const KeyState& State(Key key) const;
        bool            IsDown(Key key) const;
        Key             HeldChannelKey() const;
        bool            AnyChannelKeyDown() const;
        void            ExpireStaleKeys(Clock::time_point now);
        const char*     BackendBlockReason() const;

        void Start(RE::PlayerCharacter* player, Key key, const char* why);
        void Stop(RE::PlayerCharacter* player, const char* why, bool needRelease);
        void CycleMode();
        void LogEvent(std::string_view name, std::string_view outcome);
        void FlushEventLog();

        mutable std::recursive_mutex _mutex;
        Config                       _config;
        bool                         _oarAvailable = false;
        bool                         _injectorAvailable = false;

        KeyState _right;
        KeyState _left;
        KeyState _extra;

        bool              _active = false;
        Motion            _activeMotion = Motion::kOar;
        Key               _channelKey = Key::kNone;
        Clock::time_point _startedAt{};
        bool              _injectorChecked = false;  // 인젝터 동작이 실제로 시작됐는지 한 번 확인했는가
        bool              _needRelease = false;      // 조건 때문에 끝났으면 버튼을 한 번 떼야 다시 시작한다
        Key               _pendingStart = Key::kNone;

        std::string _lastEventName;
        std::string _lastEventOutcome;
        int         _lastEventRepeats = 0;
    };
}
