#include "Runtime/Channel.h"

#include "Core/EventNames.h"

#include <SKSE/SKSE.h>

#include <string>
#include <string_view>

namespace poe
{
    namespace
    {
        // 프로토타입 1의 두 시험 모드와 맞춘 이름.
        constexpr auto kOarValue = RE::ActorValue::kVariable09;        // 1B의 OAR 조건이 보는 액터 값
        constexpr auto kInjectorStartEvent = "PoEProto_CycloneStart";  // 1A의 <Start event>
        constexpr auto kInjectorStopEvent = "PoEProto_CycloneStop";    // 1A의 <Transition event>
        constexpr auto kAttackStopEvent = "attackStop";
        constexpr auto kAnimationDriven = "bAnimationDriven";

        // Animation Injector는 동작을 시작하면서 bAnimationDriven을 켠다. 시작 요청 뒤 이만큼 지났을 때
        // 그 값을 한 번 읽어 로그에 남긴다. 판단 근거가 소스뿐이라 채널링을 끊는 데는 쓰지 않는다.
        constexpr std::chrono::milliseconds kInjectorGrace{ 500 };

        // 누르고 있는 버튼은 매 프레임 "눌려 있음" 이벤트가 온다. 이만큼 끊기면 뗀 것으로 본다.
        // 창이 포커스를 잃어 뗌 이벤트가 오지 않은 경우에 회전이 멈추지 않는 것을 막는다.
        constexpr std::chrono::milliseconds kInputTimeout{ 500 };

        thread_local bool t_sending = false;

        struct SendingScope
        {
            SendingScope() { t_sending = true; }
            ~SendingScope() { t_sending = false; }
            SendingScope(const SendingScope&) = delete;
            SendingScope& operator=(const SendingScope&) = delete;
        };

        bool Send(RE::PlayerCharacter* player, const char* event)
        {
            const SendingScope scope;
            return player->NotifyAnimationGraph(event);
        }

        bool GamePaused()
        {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GameIsPaused();
        }

        // 플레이어의 상태 때문에 채널링을 할 수 없는 이유. 할 수 있으면 nullptr.
        const char* PlayerBlockReason(RE::PlayerCharacter* player)
        {
            if (!player || !player->Is3DLoaded()) {
                return "플레이어가 준비되지 않음";
            }
            if (player->IsDead()) {
                return "사망";
            }
            const auto* state = player->AsActorState();
            if (!state) {
                return "액터 상태를 읽지 못함";
            }
            if (!state->IsWeaponDrawn()) {
                return "무기를 꺼내지 않음";
            }
            if (state->IsSwimming()) {
                return "수영 중";
            }
            if (state->IsBleedingOut()) {
                return "빈사 상태";
            }
            if (state->IsStaggered()) {
                return "경직";
            }
            if (state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
                return "앉거나 누워 있음";
            }
            if (player->IsOnMount()) {
                return "탑승 중";
            }
            if (player->IsInKillMove()) {
                return "킬무브 중";
            }
            if (player->IsInRagdollState()) {
                return "래그돌";
            }
            const auto* controls = RE::ControlMap::GetSingleton();
            if (!controls || !controls->IsFightingControlsEnabled() || !controls->IsMovementControlsEnabled()) {
                return "전투 또는 이동 조작이 꺼져 있음";
            }
            const auto* form = player->GetEquippedObject(false);
            const auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
            if (!weapon || !weapon->IsMelee() || weapon->IsHandToHandMelee()) {
                return "오른손에 근접 무기가 없음";
            }
            return nullptr;
        }

        bool AnimationDriven(RE::PlayerCharacter* player)
        {
            bool driven = false;
            return player->GetGraphVariableBool(kAnimationDriven, driven) && driven;
        }

        void SetOarValue(RE::PlayerCharacter* player, float value)
        {
            if (auto* owner = player->AsActorValueOwner()) {
                owner->SetActorValue(kOarValue, value);
            }
        }

        const char* MotionLabel(Motion motion)
        {
            return motion == Motion::kInjector ? "Animation Injector" : "OAR 클립 교체";
        }
    }

    Channel& Channel::Get()
    {
        static Channel instance;
        return instance;
    }

    bool Channel::IsSending()
    {
        return t_sending;
    }

    void Channel::Configure(const Config& config)
    {
        const std::scoped_lock lock(_mutex);
        _config = config;
    }

    void Channel::SetBackends(bool oarAvailable, bool injectorAvailable)
    {
        const std::scoped_lock lock(_mutex);
        _oarAvailable = oarAvailable;
        _injectorAvailable = injectorAvailable;
    }

    // 지금 고른 모션 쪽을 쓸 수 없는 이유. 쓸 수 있으면 nullptr.
    const char* Channel::BackendBlockReason() const
    {
        if (_config.motion == Motion::kOar && !_oarAvailable) {
            return "OpenAnimationReplacer.dll이 로드되지 않음";
        }
        if (_config.motion == Motion::kInjector && !_injectorAvailable) {
            return "AnimationInjector.dll이 로드되지 않음";
        }
        return nullptr;
    }

    Channel::Key Channel::Classify(RE::INPUT_DEVICE device, std::uint32_t idCode) const
    {
        if (device == RE::INPUT_DEVICE::kKeyboard && _config.extraChannelKey != 0 && idCode == _config.extraChannelKey) {
            return Key::kExtra;
        }
        const auto* controls = RE::ControlMap::GetSingleton();
        const auto* events = RE::UserEvents::GetSingleton();
        if (!controls || !events) {
            return Key::kNone;
        }
        const auto mapped = [&](const RE::BSFixedString& userEvent) {
            const auto code = controls->GetMappedKey(std::string_view(userEvent), device);
            return code != static_cast<std::uint32_t>(RE::ControlMap::kInvalid) && code == idCode;
        };
        if (mapped(events->rightAttack)) {
            return Key::kRight;
        }
        if (mapped(events->leftAttack)) {
            return Key::kLeft;
        }
        return Key::kNone;
    }

    // 눌린 것으로 기록해 둔 채널 버튼 가운데 이 장치·코드와 같은 것.
    Channel::Key Channel::FindDown(RE::INPUT_DEVICE device, std::uint32_t idCode) const
    {
        for (const auto key : { Key::kRight, Key::kLeft, Key::kExtra }) {
            const auto& state = State(key);
            if (state.down && state.device == device && state.idCode == idCode) {
                return key;
            }
        }
        return Key::kNone;
    }

    Channel::KeyState& Channel::State(Key key)
    {
        switch (key) {
        case Key::kLeft:
            return _left;
        case Key::kExtra:
            return _extra;
        default:
            return _right;
        }
    }

    const Channel::KeyState& Channel::State(Key key) const
    {
        switch (key) {
        case Key::kLeft:
            return _left;
        case Key::kExtra:
            return _extra;
        default:
            return _right;
        }
    }

    bool Channel::IsDown(Key key) const
    {
        return key != Key::kNone && State(key).down;
    }

    bool Channel::AnyChannelKeyDown() const
    {
        return _right.down || _left.down || _extra.down;
    }

    // 지금 눌려 있는 채널 버튼 가운데 가장 나중에 누른 것.
    Channel::Key Channel::HeldChannelKey() const
    {
        auto best = Key::kNone;
        for (const auto key : { Key::kRight, Key::kLeft, Key::kExtra }) {
            const auto& state = State(key);
            if (!state.down || !state.eligible) {
                continue;
            }
            if (best == Key::kNone || state.downAt > State(best).downAt) {
                best = key;
            }
        }
        return best;
    }

    void Channel::ExpireStaleKeys(Clock::time_point now)
    {
        for (const auto key : { Key::kRight, Key::kLeft, Key::kExtra }) {
            auto& state = State(key);
            if (state.down && now - state.lastSeen > kInputTimeout) {
                state.down = false;
                state.eligible = false;
                SKSE::log::info("[입력] 누르고 있다는 이벤트가 {}ms 넘게 끊겨 뗀 것으로 처리함(뗌 이벤트 누락)",
                    kInputTimeout.count());
            }
        }
    }

    void Channel::OnButton(RE::INPUT_DEVICE device, std::uint32_t idCode, bool pressed, bool first)
    {
        const std::scoped_lock lock(_mutex);
        if (!_config.enabled) {
            return;
        }

        if (!pressed) {
            if (const auto key = FindDown(device, idCode); key != Key::kNone) {
                auto& state = State(key);
                state.down = false;
                state.eligible = false;
            }
            return;
        }

        const auto now = Clock::now();
        if (!first) {
            if (const auto key = FindDown(device, idCode); key != Key::kNone) {
                State(key).lastSeen = now;
            }
            return;
        }

        if (device == RE::INPUT_DEVICE::kKeyboard && _config.cycleKey != 0 && idCode == _config.cycleKey) {
            if (!GamePaused()) {
                CycleMode();
            }
            return;
        }

        const auto key = Classify(device, idCode);
        if (key == Key::kNone) {
            return;
        }
        auto& state = State(key);
        state.down = true;
        state.eligible = !GamePaused();
        state.device = device;
        state.idCode = idCode;
        state.downAt = now;
        state.lastSeen = now;
    }

    bool Channel::OnNotify(const RE::BSFixedString& event)
    {
        const char* raw = event.c_str();
        if (!raw || raw[0] == '\0') {
            return false;
        }
        const std::string_view name(raw);
        if (!IsAttackRelatedEvent(name)) {
            return false;
        }

        // 다른 스레드가 잠금을 쥐고 엔진 안에 들어가 있을 수 있다. 기다리면 서로 물릴 수 있으므로
        // 잠그지 못하면 이 이벤트는 손대지 않고 넘긴다. 같은 스레드의 재진입은 잠금이 된다.
        const std::unique_lock lock(_mutex, std::try_to_lock);
        if (!lock.owns_lock() || !_config.enabled) {
            return false;
        }

        const bool       attackStart = IsAttackStartEvent(name);
        const bool       powerStart = attackStart && IsPowerAttackStartEvent(name);
        std::string_view outcome = "통과";
        bool             swallow = false;

        if (_active || _pendingStart != Key::kNone) {
            // 채널링 중에는 누르고 있는 버튼 때문에 공격이 다시 시작되지 않게 한다.
            if (attackStart) {
                swallow = true;
                outcome = "채널링 중이라 막음";
            }
        } else if (powerStart && _config.trigger == Trigger::kPowerAttack && !_needRelease) {
            const auto key = HeldChannelKey();
            if (key == Key::kNone) {
                outcome = "통과(누르고 있는 공격 버튼이 없음)";
            } else if (const char* backend = BackendBlockReason()) {
                outcome = backend;
            } else if (const char* blocked = PlayerBlockReason(RE::PlayerCharacter::GetSingleton())) {
                outcome = blocked;
            } else {
                // 훅 안에서 다시 이벤트를 보내지 않도록 시작은 다음 Tick으로 미룬다.
                _pendingStart = key;
                swallow = true;
                outcome = "강공격을 채널링으로 바꿈";
            }
        }

        if (_config.logEvents) {
            LogEvent(name, outcome);
        }
        return swallow;
    }

    // 버튼을 누르고 있는 동안 같은 이벤트가 매 프레임 올 수 있어, 연달아 같은 줄은 횟수만 센다.
    void Channel::LogEvent(std::string_view name, std::string_view outcome)
    {
        if (_lastEventName == name && _lastEventOutcome == outcome) {
            ++_lastEventRepeats;
            return;
        }
        FlushEventLog();
        _lastEventName.assign(name);
        _lastEventOutcome.assign(outcome);
        SKSE::log::info("[이벤트] '{}' -> {}", name, outcome);
    }

    void Channel::FlushEventLog()
    {
        if (_lastEventRepeats > 0) {
            SKSE::log::info("[이벤트] '{}' -> {} (같은 줄 {}회 더)", _lastEventName, _lastEventOutcome,
                _lastEventRepeats);
        }
        _lastEventRepeats = 0;
        _lastEventName.clear();
        _lastEventOutcome.clear();
    }

    void Channel::Tick()
    {
        const std::scoped_lock lock(_mutex);
        if (!_config.enabled) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        const auto now = Clock::now();
        ExpireStaleKeys(now);

        if (_active) {
            if (!IsDown(_channelKey)) {
                Stop(player, "버튼을 뗌", false);
            } else if (const char* blocked = PlayerBlockReason(player)) {
                Stop(player, blocked, true);
            } else if (std::chrono::duration<float>(now - _startedAt).count() >= _config.maxSeconds) {
                Stop(player, "최대 시간에 도달", true);
            } else if (_activeMotion == Motion::kInjector && !_injectorChecked && now - _startedAt >= kInjectorGrace) {
                _injectorChecked = true;
                SKSE::log::info("[채널링] Animation Injector 동작 확인: bAnimationDriven = {}",
                    AnimationDriven(player) ? "켜짐(동작이 시작된 것으로 보임)" :
                                              "꺼짐(동작이 시작되지 않았을 수 있음)");
            }
            return;
        }

        if (_needRelease) {
            if (!AnyChannelKeyDown()) {
                _needRelease = false;
            }
            _pendingStart = Key::kNone;
            return;
        }

        if (_pendingStart != Key::kNone) {
            const auto key = _pendingStart;
            _pendingStart = Key::kNone;
            if (!IsDown(key)) {
                SKSE::log::info("[채널링] 시작 취소: 강공격을 바꾸려는 사이에 버튼을 뗌");
            } else if (const char* blocked = PlayerBlockReason(player)) {
                SKSE::log::info("[채널링] 시작 취소: {}", blocked);
            } else {
                Start(player, key, "강공격 가로채기");
            }
            return;
        }

        if (_config.trigger == Trigger::kHold && _right.down && _right.eligible &&
            std::chrono::duration<float>(now - _right.downAt).count() >= _config.holdSeconds) {
            const char* blocked = BackendBlockReason();
            if (!blocked) {
                blocked = PlayerBlockReason(player);
            }
            if (blocked) {
                // 조건이 안 맞는 채로 계속 누르고 있을 때 매 프레임 다시 검사하지 않는다.
                _needRelease = true;
                SKSE::log::info("[채널링] 길게 눌렀지만 시작하지 않음: {}", blocked);
            } else {
                Start(player, Key::kRight, "길게 누르기");
            }
        }
    }

    void Channel::Start(RE::PlayerCharacter* player, Key key, const char* why)
    {
        _active = true;
        _activeMotion = _config.motion;
        _channelKey = key;
        _startedAt = Clock::now();
        _injectorChecked = false;

        if (_activeMotion == Motion::kOar) {
            SetOarValue(player, 1.0f);
            // 누르는 순간 시작된 일반 공격을 끊어 이동 상태로 돌려보낸다. 회전 모션은 이동 클립 자리에 재생된다.
            const bool accepted = Send(player, kAttackStopEvent);
            SKSE::log::info("[채널링] 시작({}, {}): Variable09 = 1, attackStop {}", MotionLabel(_activeMotion), why,
                accepted ? "받아들여짐" : "거부됨(공격 중이 아니었을 수 있음)");
        } else {
            const bool accepted = Send(player, kInjectorStartEvent);
            SKSE::log::info("[채널링] 시작({}, {}): '{}' {}", MotionLabel(_activeMotion), why, kInjectorStartEvent,
                accepted ? "받아들여짐" : "거부됨");
        }
    }

    void Channel::Stop(RE::PlayerCharacter* player, const char* why, bool needRelease)
    {
        const float seconds = std::chrono::duration<float>(Clock::now() - _startedAt).count();
        if (_activeMotion == Motion::kOar) {
            SetOarValue(player, 0.0f);
        } else {
            Send(player, kInjectorStopEvent);
        }
        FlushEventLog();
        SKSE::log::info("[채널링] 끝({}): {} / {:.2f}초", MotionLabel(_activeMotion), why, seconds);
        _active = false;
        _channelKey = Key::kNone;
        _pendingStart = Key::kNone;
        _needRelease = needRelease;
    }

    void Channel::CycleMode()
    {
        if (_active) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                Stop(player, "방식 전환", true);
            }
        }

        // OAR+강공격 -> 인젝터+강공격 -> OAR+길게 -> 인젝터+길게 -> 처음으로
        if (_config.motion == Motion::kOar) {
            _config.motion = Motion::kInjector;
        } else {
            _config.motion = Motion::kOar;
            _config.trigger = _config.trigger == Trigger::kPowerAttack ? Trigger::kHold : Trigger::kPowerAttack;
        }

        std::string message = "[PoE Proto] 모션: ";
        message += MotionLabel(_config.motion);
        message += _config.trigger == Trigger::kPowerAttack ? " / 시작: 강공격 가로채기" : " / 시작: 길게 누르기";
        if (const char* backend = BackendBlockReason()) {
            message += " (동작하지 않음: ";
            message += backend;
            message += ")";
        }
        RE::SendHUDMessage::ShowHUDMessage(message.c_str());
        SKSE::log::info("{}", message);
    }

    void Channel::OnLoad(bool playerReady)
    {
        const std::scoped_lock lock(_mutex);
        if (!_config.enabled) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();

        // 로드가 실패하거나 취소돼도 회전이 남지 않도록, 진행 중인 채널링은 여기서 제대로 끝낸다.
        if (_active && player) {
            Stop(player, playerReady ? "세이브를 불러옴" : "세이브 로드 시작", false);
        }
        _active = false;
        _channelKey = Key::kNone;
        _pendingStart = Key::kNone;
        _needRelease = false;
        _right = {};
        _left = {};
        _extra = {};

        if (!playerReady) {
            return;
        }
        // 회전 중에 저장된 세이브는 Variable09가 1로 남아 있다. 불러온 직후에는 항상 꺼 둔다.
        auto* owner = player ? player->AsActorValueOwner() : nullptr;
        if (owner && owner->GetActorValue(kOarValue) != 0.0f) {
            owner->SetActorValue(kOarValue, 0.0f);
            SKSE::log::info("[채널링] 불러온 세이브의 Variable09가 0이 아니어서 0으로 되돌림");
        }
    }
}
