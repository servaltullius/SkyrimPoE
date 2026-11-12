#pragma once

#include "Core/Config.h"
#include "Core/Spin.h"

#include <RE/Skyrim.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <string>
#include <string_view>
#include <vector>

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

        // 설정에 적힌 발동용 폼(주문 또는 연출용 폭발)을 찾아 둔다. 게임 데이터가 다 불린 뒤(kDataLoaded)에 한 번.
        void ResolveForms();

        // 입력 싱크에서 버튼 이벤트 하나마다. pressed = 눌려 있음, first = 이번에 처음 눌림.
        void OnButton(RE::INPUT_DEVICE device, std::uint32_t idCode, bool pressed, bool first);

        // 플레이어의 NotifyAnimationGraph 훅에서. true를 돌려주면 그 이벤트를 게임에 넘기지 않는다.
        bool OnNotify(const RE::BSFixedString& event);

        // 플레이어 Update가 끝난 뒤 매 프레임.
        void Tick();

        // 세이브 로드 직전(playerReady = false)과 로드·새 게임 직후(playerReady = true).
        void OnLoad(bool playerReady);

        // 누군가 맞았을 때. 플레이어가 때린 것만 세고 기록한다. 어느 스레드에서든 불릴 수 있다.
        void OnHit(const RE::TESHitEvent& event);

        // 누군가 죽었을 때. 누가 죽였는지를 기록한다(직접 준 피해의 처치가 플레이어의 것으로 남는지 확인용).
        // 어느 스레드에서든 불릴 수 있다.
        void OnDeath(const RE::TESDeathEvent& event);

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
        static const char* KeyLabel(Key key);

        void Start(RE::PlayerCharacter* player, Key key, const char* why);
        void Stop(RE::PlayerCharacter* player, const char* why, bool needRelease);
        void CycleMode();
        bool AdvanceSpin(RE::PlayerCharacter* player, float frame);
        void RestoreSpinNode(RE::PlayerCharacter* player);
        std::vector<RE::NiPointer<RE::Actor>> CollectTargets(RE::PlayerCharacter* player, const RE::NiPoint3& origin,
            float radius) const;
        void AreaHit(RE::PlayerCharacter* player);
        void RollTrigger(RE::PlayerCharacter* player, RE::Actor* target);
        bool TriggerBySpell(RE::PlayerCharacter* player, RE::Actor* target);
        bool TriggerDirect(RE::PlayerCharacter* player, RE::Actor* target);
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

        // 회전과 주기 타격(OAR 방식에서 DLL이 맡을 때). 메인 스레드에서만 쓴다.
        Clock::time_point _lastTick{};
        bool              _spinning = false;        // 이번 채널링에서 DLL이 몸을 돌리는가
        bool              _spinResetWarned = false;
        bool              _spinRestorePending = false;  // 끝날 때 노드를 못 찾아 되돌리지 못했는가
        SpinClock         _spin;                    // 돈 각도와 마무리 상태
        float             _turnsPerSecond = 1.0f;
        RE::NiMatrix3     _spinBase;                // 시작할 때 NPC 노드의 회전. 끝나면 이 값으로 되돌린다
        RE::NiMatrix3     _spinApplied;             // 마지막으로 써 넣은 회전(다른 것이 덮어쓰는지 확인용)
        bool              _hitting = false;         // 이번 채널링에서 DLL이 타격을 내는가
        Clock::duration   _hitInterval{};
        Clock::time_point _nextHitAt{};

        // 치명타 시 발동. 주문과 폭발은 게임이 끝날 때까지 살아 있는 기본 폼이라 포인터로 들고 있어도 된다.
        bool              _triggerReady = false;       // 발동을 쓸 수 있는가(설정이 켜져 있고 필요한 폼을 찾았는가)
        RE::SpellItem*    _triggerSpell = nullptr;     // spell 방식에서 시전할 주문
        RE::BGSExplosion* _triggerExplosion = nullptr; // direct 방식에서 연출로 띄울 폭발. 없으면 연출 없이 피해만 준다
        std::mt19937      _random{ std::random_device{}() };
        Clock::time_point _nextTriggerAt{};
        int               _rolls = 0;   // 이번 채널링에서 치명타를 굴린 횟수
        int               _crits = 0;   // 그중 치명타로 나온 횟수
        int               _casts = 0;   // 이번 채널링의 발동 수(쿨다운에 걸린 치명타는 빠진다)
        int               _directHits = 0;  // 이번 채널링에서 direct 방식으로 피해를 준 횟수(대상 수의 합)
        int               _directCasts = 0; // direct 방식으로 발동한 누적 횟수. 난이도 인자 교대 시험의 순번으로 쓴다
        bool              _critChanceLogged = false;  // 이번 채널링의 치명타 확률을 로그에 남겼는가

        // 적중 집계. OnHit이 잠금 없이 읽고 쓴다.
        std::atomic<bool> _hitActive{ false };  // 채널링 중인가
        std::atomic<int>  _hitVariant{ 1 };     // 그 채널링의 클립 번호
        std::atomic<int>  _hits{ 0 };           // 그 채널링에서 플레이어가 맞힌 횟수
        std::atomic<bool> _logHits{ true };

        std::string _lastEventName;
        std::string _lastEventOutcome;
        int         _lastEventRepeats = 0;
    };
}
