#include "Runtime/Channel.h"

#include "Core/Crit.h"
#include "Core/EventNames.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

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

        // ---- 회전: 스켈레톤의 "NPC" 노드 ----
        // 플레이어 3D의 뿌리 바로 아래에 "NPC" 노드가 있고 그 아래에 "NPC Root [Root]"부터 뼈가 달린다
        // (바닐라와 XPMSSE 스켈레톤에서 확인). 애니메이션은 "NPC Root [Root]"부터 쓰므로 이 노드의 회전은
        // 애니메이션이 덮어쓰지 않는다. 여기를 돌리면 액터의 방향·이동·충돌은 그대로 두고 보이는 몸만 돈다.
        constexpr float kMaxFrameSeconds = 0.1f; // 프레임이 길게 끊겼을 때 한 번에 너무 많이 돌지 않게 한다

        RE::NiAVObject* FindSpinNode(RE::PlayerCharacter* player)
        {
            static const RE::BSFixedString name{ "NPC" };
            auto* root = player ? player->Get3D(false) : nullptr;
            return root ? root->GetObjectByName(name) : nullptr;
        }

        // 세로축(Z) 둘레로 angle 라디안 돌리는 행렬. 양의 각이 위에서 볼 때 반시계 방향이다.
        RE::NiMatrix3 RotationAboutUp(float angle)
        {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            RE::NiMatrix3 m;
            m.entry[0][0] = c;
            m.entry[0][1] = -s;
            m.entry[1][0] = s;
            m.entry[1][1] = c;
            return m;
        }

        RE::NiMatrix3 Multiply(const RE::NiMatrix3& a, const RE::NiMatrix3& b)
        {
            RE::NiMatrix3 out;
            for (int r = 0; r < 3; ++r) {
                for (int col = 0; col < 3; ++col) {
                    out.entry[r][col] = a.entry[r][0] * b.entry[0][col] + a.entry[r][1] * b.entry[1][col] +
                                        a.entry[r][2] * b.entry[2][col];
                }
            }
            return out;
        }

        float MaxDifference(const RE::NiMatrix3& a, const RE::NiMatrix3& b)
        {
            float worst = 0.0f;
            for (int r = 0; r < 3; ++r) {
                for (int col = 0; col < 3; ++col) {
                    worst = std::max(worst, std::abs(a.entry[r][col] - b.entry[r][col]));
                }
            }
            return worst;
        }

        // ---- 타격: 엔진의 근접 타격 함수 ----
        // 공격자가 든 무기로 대상에게 근접 타격 한 번을 적용한다(피해, 방어·막기, 경직, 적중 이벤트).
        // 주소와 인자는 Precision(ersh1/Precision, GPL-3.0)의 Offsets.h가 쓰는 DealDamage와 같다.
        using DealDamageFn = void*(RE::Actor* attacker, RE::Actor* target, RE::Projectile* projectile, bool leftHand);
        const REL::Relocation<DealDamageFn> kDealDamage{ REL::RelocationID(37673, 38627) };

        constexpr float kHitHeight = 150.0f;  // 타격 대상과의 높이 차이 한도
        constexpr float kMinHitsPerSecond = 0.5f;
        constexpr float kMaxHitsPerSecond = 20.0f;
        constexpr float kMinTurnsPerSecond = 0.2f;
        constexpr float kMaxTurnsPerSecond = 5.0f;

        // 공격 피해 배율을 잠깐 바꿨다가 범위를 벗어날 때 되돌린다.
        class DamageScale
        {
        public:
            DamageScale(RE::Actor* actor, float mult)
            {
                _owner = actor ? actor->AsActorValueOwner() : nullptr;
                if (_owner) {
                    const float current = _owner->GetActorValue(RE::ActorValue::kAttackDamageMult);
                    _delta = current * (mult - 1.0f);
                    _owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kAttackDamageMult, _delta);
                }
            }
            ~DamageScale()
            {
                if (_owner) {
                    _owner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kAttackDamageMult, -_delta);
                }
            }
            DamageScale(const DamageScale&) = delete;
            DamageScale& operator=(const DamageScale&) = delete;

        private:
            RE::ActorValueOwner* _owner = nullptr;
            float                _delta = 0.0f;
        };

        // 스카이림이 이 대상에 대한 타격에 매기는 치명타 확률(퍼센트, 10이 10%).
        // 액터 값 CriticalChance에서 시작해 플레이어의 퍽이 가진 "Calculate My Critical Hit Chance" 진입점을 거친다.
        // 진입점의 인자는 퍽 소유자, 무기, 대상 세 개다(퍽 레코드의 조건 탭 수가 3).
        // 대상의 퍽이 깎는 쪽("Mod Enemy Critical Hit Chance")은 넣지 않았다.
        struct SkyrimCrit
        {
            float actorValue = 0.0f;  // 액터 값 CriticalChance
            float withPerks = 0.0f;   // 퍽의 진입점을 거친 뒤의 값. 엔진이 쓰는 값이 이것이다
        };

        SkyrimCrit SkyrimCritPercent(RE::PlayerCharacter* player, RE::Actor* target)
        {
            SkyrimCrit out;
            out.actorValue = player->AsActorValueOwner()->GetActorValue(RE::ActorValue::kCriticalChance);
            out.withPerks = out.actorValue;
            auto* form = player->GetEquippedObject(false);
            if (auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr) {
                RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kCalculateMyCriticalHitChance, player,
                    weapon, target, std::addressof(out.withPerks));
            }
            return out;
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
        _logHits.store(config.logEvents);
    }

    void Channel::SetBackends(bool oarAvailable, bool injectorAvailable)
    {
        const std::scoped_lock lock(_mutex);
        _oarAvailable = oarAvailable;
        _injectorAvailable = injectorAvailable;
    }

    const char* Channel::KeyLabel(Key key)
    {
        switch (key) {
        case Key::kRight:
            return "오른손 공격 버튼";
        case Key::kLeft:
            return "왼손 공격·방어 버튼";
        case Key::kExtra:
            return "추가 채널 키";
        default:
            return "알 수 없는 버튼";
        }
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

    // 이 버튼에서 나온 강공격을 채널링으로 바꿀 것인가(INI의 PowerAttackInput).
    // 추가 채널 키는 사용자가 강공격 전용으로 지정한 키이므로 어느 설정에서든 바꾼다.
    bool Channel::PowerInputAccepts(Key key) const
    {
        switch (_config.powerInput) {
        case PowerInput::kRightHand:
            return key != Key::kLeft;
        case PowerInput::kLeftHand:
            return key != Key::kRight;
        default:
            return true;
        }
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

        const auto now = Clock::now();
        if (!pressed) {
            if (const auto key = FindDown(device, idCode); key != Key::kNone) {
                auto& state = State(key);
                if (_config.logEvents) {
                    FlushEventLog();
                    SKSE::log::info("[입력] {} 뗌 / {:.2f}초", KeyLabel(key),
                        std::chrono::duration<float>(now - state.downAt).count());
                }
                state.down = false;
                state.eligible = false;
            }
            return;
        }

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
        if (_config.logEvents) {
            FlushEventLog();
            SKSE::log::info("[입력] {} 누름 (장치 {}, 코드 {}){}", KeyLabel(key), static_cast<int>(device), idCode,
                state.eligible ? "" : " / 게임이 멈춘 상태라 채널링 대상에서 제외");
        }
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
            } else if (!PowerInputAccepts(key)) {
                // 가장 나중에 누른 버튼이 바꾸지 않기로 한 쪽이면 원래의 강공격으로 둔다.
                outcome = "통과(이 버튼의 강공격은 채널링으로 바꾸지 않음)";
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
            const float elapsed = std::chrono::duration<float>(now - _startedAt).count();
            const float frame = std::clamp(std::chrono::duration<float>(now - _lastTick).count(), 0.0f, kMaxFrameSeconds);
            _lastTick = now;

            if (const char* blocked = PlayerBlockReason(player)) {
                Stop(player, blocked, true);
                return;
            }
            const bool held = IsDown(_channelKey);
            if (held && elapsed >= _config.maxSeconds) {
                Stop(player, "최대 시간에 도달", true);
                return;
            }
            // 일찍 떼도 최소 시간은 돈다. 그 사이에 다시 누르면 그대로 이어진다.
            // 최소 시간이 최대 시간보다 길게 설정돼 있어도 최대 시간을 넘기지 않는다.
            // 버튼을 떼면 남은 바퀴를 마저 돌아 정면에서 끝낸다(마무리).
            if (held) {
                _spin.Resume();
            } else if (!_spin.Settling() && elapsed >= std::min(_config.minSeconds, _config.maxSeconds)) {
                _spin.BeginSettle(_turnsPerSecond);
            }

            // DLL이 몸을 돌리지 않는 경우에는 마저 돌 것이 없으므로 마무리가 곧 끝이다.
            const bool turnFinished = _spinning ? AdvanceSpin(player, frame) : _spin.Settling();
            if (_hitting && now >= _nextHitAt) {
                AreaHit(player);
                // 프레임이 길게 끊겨도 밀린 타격을 한꺼번에 내지 않는다.
                _nextHitAt = std::max(_nextHitAt + _hitInterval, now);
            }
            if (_spin.Settling() && turnFinished) {
                Stop(player, "버튼을 뗌", false);
                return;
            }
            if (_activeMotion == Motion::kInjector && !_injectorChecked && now - _startedAt >= kInjectorGrace) {
                _injectorChecked = true;
                SKSE::log::info("[채널링] Animation Injector 동작 확인: bAnimationDriven = {}",
                    AnimationDriven(player) ? "켜짐(동작이 시작된 것으로 보임)" :
                                              "꺼짐(동작이 시작되지 않았을 수 있음)");
            }
            return;
        }

        // 끝날 때 몸의 방향을 되돌리지 못했다면(3D가 잠깐 없던 경우) 되돌릴 수 있을 때까지 다시 시도한다.
        RestoreSpinNode(player);

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
        _hits.store(0);
        _misses = 0;
        _lastAreaCensus.clear();
        _rolls = 0;
        _crits = 0;
        _casts = 0;
        _directHits = 0;
        _critChanceLogged = false;
        _hitVariant.store(static_cast<int>(_config.clipVariant));
        _hitActive.store(true);

        _lastTick = _startedAt;
        _spin.Reset();
        _spinning = false;
        _hitting = false;
        // 앞선 채널링이 몸의 방향을 되돌리지 못한 채 끝났다면, 그 값을 기준으로 삼기 전에 먼저 되돌린다.
        RestoreSpinNode(player);

        if (_activeMotion == Motion::kOar) {
            // 액터 값이 클립 번호다. OAR의 서브모드마다 이 값이 자기 번호와 같을 때만 클립을 바꾼다.
            SetOarValue(player, static_cast<float>(_config.clipVariant));
            // 누르는 순간 시작된 일반 공격을 끊어 이동 상태로 돌려보낸다. 회전 모션은 이동 클립 자리에 재생된다.
            const bool accepted = Send(player, kAttackStopEvent);
            SKSE::log::info("[채널링] 시작({}, {}, {}): 클립 {}번(Variable09 = {}), attackStop {}",
                MotionLabel(_activeMotion), why, KeyLabel(key), _config.clipVariant, _config.clipVariant,
                accepted ? "받아들여짐" : "거부됨(공격 중이 아니었을 수 있음)");

            // 회전 속도와 타격 주기는 든 무기의 속도에 맞춘다.
            float weaponSpeed = 1.0f;
            if (const auto* form = player->GetEquippedObject(false)) {
                if (const auto* weapon = form->As<RE::TESObjectWEAP>()) {
                    weaponSpeed = weapon->GetSpeed();
                }
            }
            float speedMult = 1.0f;
            if (auto* owner = player->AsActorValueOwner()) {
                const float value = owner->GetActorValue(RE::ActorValue::kWeaponSpeedMult);
                if (value > 0.0f) {
                    speedMult = value;  // 0은 "배율 없음"을 뜻한다
                }
            }
            const float hitsPerSecond =
                std::clamp(_config.hitsPerSecond * weaponSpeed * speedMult, kMinHitsPerSecond, kMaxHitsPerSecond);
            _turnsPerSecond = std::clamp(hitsPerSecond * _config.turnsPerHit, kMinTurnsPerSecond, kMaxTurnsPerSecond);
            _hitInterval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(1.0f / hitsPerSecond));

            if (_config.spinByDll) {
                if (auto* node = FindSpinNode(player)) {
                    _spinBase = node->local.rotate;
                    _spinApplied = _spinBase;
                    _spinning = true;
                    _spinResetWarned = false;
                } else {
                    SKSE::log::warn("[회전] 플레이어 3D에서 'NPC' 노드를 찾지 못해 몸을 돌리지 못함. 자세만 바뀝니다.");
                }
            }
            if (_config.hitsByDll) {
                _hitting = true;
                _nextHitAt = _startedAt + _hitInterval / 2;
            }
            SKSE::log::info("[채널링] 무기 속도 {:.2f} x 배율 {:.2f}: 초당 타격 {:.2f}회, 초당 {:.2f}바퀴, 회전 {}, 타격 {}",
                weaponSpeed, speedMult, hitsPerSecond, _turnsPerSecond, _spinning ? "DLL" : "클립에 맡김",
                _hitting ? "DLL" : "클립에 맡김");
        } else {
            const bool accepted = Send(player, kInjectorStartEvent);
            SKSE::log::info("[채널링] 시작({}, {}, {}): '{}' {}", MotionLabel(_activeMotion), why, KeyLabel(key),
                kInjectorStartEvent, accepted ? "받아들여짐" : "거부됨");
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
        if (_spinning) {
            // 몸의 방향을 시작할 때의 값으로 되돌린다. 지금 노드를 찾지 못하면 다음 프레임들에서 다시 시도한다.
            _spinning = false;
            _spinRestorePending = true;
            RestoreSpinNode(player);
        }
        _hitting = false;
        _spin.Reset();
        FlushEventLog();
        _hitActive.store(false);
        SKSE::log::info("[채널링] 끝({}): {} / {:.2f}초 / 적중 {}회 / 타격이 안 들어간 차례 {}번 / "
                        "치명타 판정 {}번 중 {}번, 발동 {}회(직접 피해 {}건)",
            MotionLabel(_activeMotion), why, seconds, _hits.load(), _misses, _rolls, _crits, _casts, _directHits);
        _active = false;
        _channelKey = Key::kNone;
        _pendingStart = Key::kNone;
        _needRelease = needRelease;
    }

    // 몸을 frame초만큼 더 돌린다. 마무리 중이고 정면으로 돌아왔으면 true.
    bool Channel::AdvanceSpin(RE::PlayerCharacter* player, float frame)
    {
        const bool finished = _spin.Advance(_turnsPerSecond, frame);

        auto* node = FindSpinNode(player);
        if (!node) {
            return finished;  // 3D가 잠깐 없는 프레임. 각도만 진행한다
        }
        // 우리가 써 둔 값이 바뀌어 있으면 다른 무엇인가가 이 노드를 썼거나 3D가 새로 만들어진 것이다. 한 번만 알린다.
        if (!_spinResetWarned && MaxDifference(node->local.rotate, _spinApplied) > 1e-3f) {
            _spinResetWarned = true;
            SKSE::log::warn("[회전] 'NPC' 노드의 회전이 우리가 쓴 값과 다름. 3D가 새로 만들어졌거나(장비 교체 등) "
                            "다른 모드·엔진이 이 노드를 덮어쓴 것입니다. 회전이 보이지 않는다면 뒤쪽입니다.");
        }
        const float signedAngle = _config.spinClockwise ? -_spin.Angle() : _spin.Angle();
        _spinApplied = Multiply(RotationAboutUp(signedAngle), _spinBase);
        node->local.rotate = _spinApplied;
        return finished;
    }

    // 끝난 채널링이 돌려 둔 몸의 방향을 되돌린다. 노드를 찾지 못하면 표시만 남겨 두고 나중에 다시 시도한다.
    void Channel::RestoreSpinNode(RE::PlayerCharacter* player)
    {
        if (!_spinRestorePending) {
            return;
        }
        auto* node = FindSpinNode(player);
        if (!node) {
            return;
        }
        // 노드가 우리가 마지막으로 쓴 값을 그대로 갖고 있을 때만 되돌린다.
        // 값이 다르면 3D가 새로 만들어져 이미 기본값이거나 다른 것이 써 넣은 것이므로 건드리지 않는다.
        if (MaxDifference(node->local.rotate, _spinApplied) <= 1e-3f) {
            node->local.rotate = _spinBase;
        }
        _spinRestorePending = false;
    }

    std::string Channel::Excluded::Describe() const
    {
        if (teammates == 0 && notHostile == 0 && other == 0) {
            return "제외 없음";
        }
        std::string text = "제외:";
        if (teammates > 0) {
            text += fmt::format(" 동료 {}명({})", teammates, teammateNames);
        }
        if (notHostile > 0) {
            text += fmt::format(" 적대적이지 않음 {}명", notHostile);
        }
        if (other > 0) {
            text += fmt::format(" 유령·킬무브 {}명", other);
        }
        return text;
    }

    // origin 둘레에서 플레이어의 스킬이 맞힐 수 있는 액터를 모은다.
    // 플레이어 자신, 동료, 죽은 액터, 유령, 킬무브 중인 액터는 빼고, HitNeutral이 꺼져 있으면 적대적인 액터만 남긴다.
    // 범위 안에 살아 있었는데 뺀 액터는 이유별로 excluded에 센다.
    std::vector<RE::NiPointer<RE::Actor>> Channel::CollectTargets(RE::PlayerCharacter* player, const RE::NiPoint3& origin,
        float radius, Excluded& excluded) const
    {
        std::vector<RE::NiPointer<RE::Actor>> targets;
        excluded = Excluded{};
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!lists) {
            return targets;
        }
        const float radiusSquared = radius * radius;
        for (const auto& handle : lists->highActorHandles) {
            auto actor = handle.get();
            if (!actor || actor.get() == player) {
                continue;
            }
            if (actor->IsDead() || !actor->Is3DLoaded()) {
                continue;
            }
            const auto offset = actor->GetPosition() - origin;
            if (std::abs(offset.z) > kHitHeight || offset.x * offset.x + offset.y * offset.y > radiusSquared) {
                continue;
            }
            // 여기부터는 범위 안에 살아 있는 액터다. 빼는 것은 이유를 센다.
            if (actor->IsPlayerTeammate()) {
                ++excluded.teammates;
                const char* name = actor->GetDisplayFullName();
                if (!excluded.teammateNames.empty()) {
                    excluded.teammateNames += ", ";
                }
                excluded.teammateNames += (name && *name) ? name : "?";
                continue;
            }
            if (actor->IsGhost() || actor->IsInKillMove()) {
                ++excluded.other;
                continue;
            }
            if (!_config.hitNeutral && !actor->IsHostileToActor(player)) {
                ++excluded.notHostile;
                continue;
            }
            targets.push_back(std::move(actor));
        }
        return targets;
    }

    // 플레이어 둘레의 적에게 근접 타격을 한 번씩 적용한다.
    void Channel::AreaHit(RE::PlayerCharacter* player)
    {
        // 대상 목록을 먼저 만든 뒤에 타격한다. 타격이 액터 목록을 바꿀 수 있기 때문이다.
        Excluded excluded;
        const auto targets = CollectTargets(player, player->GetPosition(), _config.hitRadius, excluded);
        if (_config.logEvents) {
            // 차례마다 남기면 로그가 묻히므로 집계가 바뀔 때만 남긴다.
            auto census = fmt::format("반경 {:.0f} 안의 대상 {}명, {}", _config.hitRadius, targets.size(), excluded.Describe());
            if (census != _lastAreaCensus) {
                SKSE::log::info("[범위 타격] {}", census);
                _lastAreaCensus = std::move(census);
            }
        }
        if (targets.empty()) {
            return;
        }
        // 타격이 실제로 들어간 대상만 모은다. 엔진의 타격 함수는 대상의 상태에 따라 아무 일도 하지 않고 돌아올 수 있다
        // (8차 시험에서 적중 이벤트도 체력 변화도 없는 차례가 있었다). 치명타는 적중한 타격에 붙는 것이므로
        // 들어가지 않은 타격에는 굴리지 않는다.
        // 들어갔다고 보는 근거는 둘 중 하나다: 함수가 도는 동안 그 대상의 적중 이벤트가 왔거나, 체력이 줄었거나 죽었거나.
        std::vector<RE::Actor*> landed;
        landed.reserve(targets.size());
        {
            const DamageScale scale(player, _config.hitDamageMult);
            for (const auto& target : targets) {
                auto*       owner = target->AsActorValueOwner();
                const float before = owner->GetActorValue(RE::ActorValue::kHealth);
                _probeHit.store(false);
                _probeTarget.store(target->GetFormID());
                kDealDamage(player, target.get(), nullptr, false);
                _probeTarget.store(0);
                const bool eventSeen = _probeHit.load();
                const bool hurt = target->IsDead() || owner->GetActorValue(RE::ActorValue::kHealth) < before;
                if (eventSeen || hurt) {
                    landed.push_back(target.get());
                } else {
                    ++_misses;
                }
                if (_config.logEvents && eventSeen != hurt) {
                    // 두 근거가 어긋나는 경우는 따로 남겨 둔다(피해 없이 적중만 났거나, 이벤트가 함수 안에서 오지 않았거나).
                    const char* name = target->GetDisplayFullName();
                    SKSE::log::info("[범위 타격] '{}'({:08X}) 적중 이벤트 {}, 체력 {}", name ? name : "?", target->GetFormID(),
                        eventSeen ? "있음" : "없음", hurt ? "줄어듦" : "그대로");
                } else if (_config.logEvents && !eventSeen) {
                    const char* name = target->GetDisplayFullName();
                    SKSE::log::info("[범위 타격] '{}'({:08X}) 타격이 들어가지 않음(적중 이벤트 없음, 체력 그대로). 치명타를 굴리지 않음",
                        name ? name : "?", target->GetFormID());
                }
            }
        }
        // 피해 배율을 되돌린 뒤에 발동을 굴린다. 발동으로 나간 피해는 여기를 다시 지나지 않으므로
        // 발동이 발동을 부르는 일은 없다. targets가 액터를 붙들고 있으므로 landed의 포인터는 이 함수 안에서 유효하다.
        for (auto* target : landed) {
            RollTrigger(player, target);
        }
    }

    // 타격 한 번에 대해 치명타를 굴리고, 치명타이고 쿨다운이 끝났으면 그 대상을 중심으로 스킬을 발동한다.
    void Channel::RollTrigger(RE::PlayerCharacter* player, RE::Actor* target)
    {
        if (!_triggerReady) {
            return;
        }
        // 우리 쪽 확률(지금은 INI의 임시 값)에 스카이림의 치명타 확률을 더한다(설계-현황 결정 51).
        const SkyrimCrit skyrim = _config.addSkyrimCrit ? SkyrimCritPercent(player, target) : SkyrimCrit{};
        const float chance = CombineCritChance(_config.critChance, skyrim.withPerks);
        if (_config.logEvents && !_critChanceLogged) {
            _critChanceLogged = true;  // 채널링마다 첫 판정에서 한 번만 남긴다
            const char* name = target->GetDisplayFullName();
            SKSE::log::info("[발동] 치명타 확률 {:.1f}% = 기본 {:.1f}% + 스카이림 {:.1f}%{} (액터 값 {:.1f}, 퍽 반영 뒤 {:.1f}, "
                            "대상 '{}')",
                chance * 100.0f, _config.critChance * 100.0f, skyrim.withPerks, _config.addSkyrimCrit ? "" : "(합산 끔)",
                skyrim.actorValue, skyrim.withPerks, name ? name : "?");
        }
        ++_rolls;
        if (std::uniform_real_distribution<float>(0.0f, 1.0f)(_random) >= chance) {
            return;
        }
        ++_crits;
        const auto now = Clock::now();
        if (now < _nextTriggerAt || target->IsDead()) {
            return;  // 쿨다운 중이거나 방금 타격으로 죽은 대상
        }
        const bool fired = _config.triggerMode == TriggerMode::kSpell ? TriggerBySpell(player, target) :
                                                                         TriggerDirect(player, target);
        if (fired) {
            _nextTriggerAt = now + std::chrono::duration_cast<Clock::duration>(
                                       std::chrono::duration<float>(_config.triggerCooldown));
            ++_casts;
        }
    }

    // spell 방식: 주문을 대상에게 시전한다. 손이나 매지카를 쓰지 않는 즉시 시전이고, 조준형 주문은 대상을 향해 날아간다.
    // 주문의 범위 효과는 엔진이 적용하므로 아군과 플레이어 자신에게도 닿는다.
    bool Channel::TriggerBySpell(RE::PlayerCharacter* player, RE::Actor* target)
    {
        auto* caster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
        if (!caster || !_triggerSpell) {
            return false;
        }
        caster->CastSpellImmediate(_triggerSpell, false, target, 1.0f, false, 0.0f, player);
        if (_config.logEvents) {
            const char* name = target->GetDisplayFullName();
            SKSE::log::info("[발동] 치명타 -> '{}' 시전, 대상 '{}'({:08X})", _triggerSpell->GetName(), name ? name : "?",
                target->GetFormID());
        }
        return true;
    }

    // direct 방식: 대상의 자리에 피해 없는 폭발을 연출로 띄우고, 그 둘레의 적대 대상에게 체력 피해를 직접 준다.
    // 누가 맞는지를 엔진의 범위 판정이 아니라 우리가 정하므로 아군과 플레이어 자신은 맞지 않는다(설계-현황 결정 52).
    bool Channel::TriggerDirect(RE::PlayerCharacter* player, RE::Actor* target)
    {
        // 피해를 주면 대상의 상태가 바뀌므로 맞을 대상을 먼저 모은다. 치명타를 맞은 대상도 여기에 들어 있다.
        Excluded   excluded;
        const auto victims = CollectTargets(player, target->GetPosition(), _config.triggerRadius, excluded);
        if (_triggerExplosion) {
            target->PlaceObjectAtMe(_triggerExplosion, false);
        }
        // DoDamage의 셋째 인자(헤더의 이름은 a_dontAdjustDifficulty). 7차 시험에서 true와 false의 결과가 같았다.
        // 플레이어가 NPC에게 주는 피해에는 어느 쪽이든 난이도 배율이 곱해지고, 스킬 피해는 그 배율을 받게 두기로 했다
        // (설계-현황 결정 53). 이 인자가 실제로 하는 일은 모르므로 평소에는 지금까지 써 온 true로 둔다.
        // 시험 설정이 켜져 있으면 발동마다 번갈아 넘기고 로그에 남긴다.
        const bool difficultyArg = _config.triggerDifficultyTest ? (_directCasts % 2 == 0) : true;
        ++_directCasts;
        if (_config.logEvents) {
            const char* name = target->GetDisplayFullName();
            SKSE::log::info("[발동] 치명타 -> 직접 피해 {:.0f}, 중심 '{}'({:08X}), 반경 {:.0f} 안의 대상 {}명, {}, 난이도 인자 {}{}",
                _config.triggerDamage, name ? name : "?", target->GetFormID(), _config.triggerRadius, victims.size(),
                excluded.Describe(), difficultyArg, _triggerExplosion ? "" : ", 연출 없음");
        }
        for (const auto& victim : victims) {
            auto* owner = victim->AsActorValueOwner();
            const float before = owner->GetActorValue(RE::ActorValue::kHealth);
            // 공격자를 플레이어로 넘겨 처치와 어그로가 플레이어의 것으로 남게 한다.
            const bool result = victim->DoDamage(_config.triggerDamage, player, difficultyArg);
            ++_directHits;
            if (_config.logEvents) {
                const float after = owner->GetActorValue(RE::ActorValue::kHealth);
                const char* name = victim->GetDisplayFullName();
                SKSE::log::info("[발동]   '{}'({:08X}) 체력 {:.1f} -> {:.1f}, 깎인 양 {:.1f}(요청의 {:.0f}%){} "
                                "(DoDamage 반환 {})",
                    name ? name : "?", victim->GetFormID(), before, after, before - after,
                    (before - after) / _config.triggerDamage * 100.0f, victim->IsDead() ? ", 사망" : "", result);
            }
        }
        return true;
    }

    void Channel::ResolveForms()
    {
        const std::scoped_lock lock(_mutex);
        _triggerSpell = nullptr;
        _triggerExplosion = nullptr;
        _triggerReady = false;
        if (_config.critChance <= 0.0f) {
            SKSE::log::info("치명타 시 발동: 꺼져 있음(CritChance = 0)");
            return;
        }
        auto* data = RE::TESDataHandler::GetSingleton();
        const char* critNote = _config.addSkyrimCrit ? " + 스카이림의 치명타 확률" : "(스카이림 값은 더하지 않음)";

        if (_config.triggerMode == TriggerMode::kSpell) {
            if (_config.triggerSpellPlugin.empty() || _config.triggerSpellFormId == 0) {
                SKSE::log::info("치명타 시 발동: 꺼져 있음(spell 방식인데 TriggerSpell이 비어 있음)");
                return;
            }
            auto* spell =
                data ? data->LookupForm<RE::SpellItem>(_config.triggerSpellFormId, _config.triggerSpellPlugin) : nullptr;
            if (!spell) {
                SKSE::log::warn("치명타 시 발동: 주문을 찾지 못해 끔({}|0x{:06X}). 플러그인이 켜져 있는지, FormID가 주문인지 "
                                "확인하세요.",
                    _config.triggerSpellPlugin, _config.triggerSpellFormId);
                return;
            }
            _triggerSpell = spell;
            _triggerReady = true;
            SKSE::log::info("치명타 시 발동(spell 방식): 기본 확률 {:.0f}%{}, 쿨다운 {:.2f}초, 주문 '{}'({}|0x{:06X})",
                _config.critChance * 100.0f, critNote, _config.triggerCooldown, spell->GetName(), _config.triggerSpellPlugin,
                _config.triggerSpellFormId);
            return;
        }

        // direct 방식. 폭발은 연출일 뿐이라 찾지 못해도 발동은 켠다.
        _triggerReady = true;
        if (!_config.triggerExplosionPlugin.empty() && _config.triggerExplosionFormId != 0) {
            _triggerExplosion = data ? data->LookupForm<RE::BGSExplosion>(_config.triggerExplosionFormId,
                                           _config.triggerExplosionPlugin) :
                                       nullptr;
            if (!_triggerExplosion) {
                SKSE::log::warn("치명타 시 발동: 연출용 폭발을 찾지 못해 연출 없이 진행({}|0x{:06X}). 플러그인이 켜져 있는지, "
                                "FormID가 폭발인지 확인하세요.",
                    _config.triggerExplosionPlugin, _config.triggerExplosionFormId);
            }
        }
        SKSE::log::info("치명타 시 발동(direct 방식): 기본 확률 {:.0f}%{}, 쿨다운 {:.2f}초, 피해 {:.0f}, 반경 {:.0f}, 연출 {}{}",
            _config.critChance * 100.0f, critNote, _config.triggerCooldown, _config.triggerDamage, _config.triggerRadius,
            _triggerExplosion ? fmt::format("{}|0x{:06X}", _config.triggerExplosionPlugin, _config.triggerExplosionFormId) :
                                std::string("없음"),
            _config.triggerDifficultyTest ? ", 난이도 인자 교대 시험 켬" : "");
    }

    void Channel::CycleMode()
    {
        if (_active) {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                Stop(player, "클립 전환", true);
            }
        }

        // 회전 클립의 번호만 바꾼다. 모션 쪽과 시작 계기는 INI로만 정한다.
        _config.clipVariant = _config.clipVariant >= _config.clipVariants ? 1 : _config.clipVariant + 1;

        std::string message = "[PoE Proto] 회전 클립 ";
        message += std::to_string(_config.clipVariant);
        message += "/";
        message += std::to_string(_config.clipVariants);
        if (const char* backend = BackendBlockReason()) {
            message += " (동작하지 않음: ";
            message += backend;
            message += ")";
        }
        RE::SendHUDMessage::ShowHUDMessage(message.c_str());
        SKSE::log::info("{}", message);
    }

    void Channel::OnDeath(const RE::TESDeathEvent& event)
    {
        // 어느 스레드에서든 올 수 있으므로 잠그지 않고 원자 값만 읽는다.
        // 엔진은 죽기 시작할 때와 죽은 뒤에 한 번씩, 모두 두 번 알린다. 뒤쪽은 2초쯤 늦으므로 둘 다 남긴다.
        if (!_logHits.load()) {
            return;
        }
        auto* dying = event.actorDying.get();
        auto* killer = event.actorKiller.get();
        const char* dyingName = dying ? dying->GetDisplayFullName() : nullptr;
        const char* killerName = killer ? killer->GetDisplayFullName() : nullptr;
        SKSE::log::info("[사망{}] '{}'({:08X}) / 처치자 {} / {}", event.dead ? " 확정" : " 시작", dyingName ? dyingName : "?",
            dying ? dying->GetFormID() : 0,
            killer ? fmt::format("'{}'({:08X}){}", killerName ? killerName : "?", killer->GetFormID(),
                         killer->IsPlayerRef() ? " = 플레이어" : "") :
                     std::string("없음"),
            _hitActive.load() ? "채널링 중" : "채널링 아님");
    }

    void Channel::OnHit(const RE::TESHitEvent& event)
    {
        // 적중 처리는 어느 스레드에서든 올 수 있으므로 잠그지 않고 원자 값만 읽고 쓴다.
        const auto* cause = event.cause.get();
        if (!cause || !cause->IsPlayerRef()) {
            return;
        }
        const bool channeling = _hitActive.load();
        if (channeling) {
            _hits.fetch_add(1);
        }
        auto* target = event.target.get();
        // 범위 타격이 지금 치고 있는 대상의 적중이면 표시해 둔다(AreaHit이 타격이 들어갔는지 판단하는 근거).
        if (target && target->GetFormID() == _probeTarget.load()) {
            _probeHit.store(true);
        }
        if (!_logHits.load()) {
            return;
        }
        const char* name = target ? target->GetDisplayFullName() : nullptr;
        SKSE::log::info("[타격] 대상 '{}'({:08X}) 수단 {:08X}{}{} / {}", name ? name : "?",
            target ? target->GetFormID() : 0, event.source,
            event.flags.any(RE::TESHitEvent::Flag::kPowerAttack) ? " 강공격" : "",
            event.flags.any(RE::TESHitEvent::Flag::kHitBlocked) ? " 막힘" : "",
            channeling ? fmt::format("채널링 중(클립 {}번)", _hitVariant.load()) : std::string("채널링 아님"));
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
        _hitActive.store(false);
        _channelKey = Key::kNone;
        _pendingStart = Key::kNone;
        _needRelease = false;
        _right = {};
        _left = {};
        _extra = {};

        if (!playerReady) {
            return;
        }
        // 회전 중에 저장된 세이브는 Variable09가 클립 번호로 남아 있다. 불러온 직후에는 항상 꺼 둔다.
        auto* owner = player ? player->AsActorValueOwner() : nullptr;
        if (owner && owner->GetActorValue(kOarValue) != 0.0f) {
            owner->SetActorValue(kOarValue, 0.0f);
            SKSE::log::info("[채널링] 불러온 세이브의 Variable09가 0이 아니어서 0으로 되돌림");
        }
    }
}
