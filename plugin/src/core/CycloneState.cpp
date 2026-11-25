#include "core/CycloneState.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace SkyrimPoE::Core
{
    namespace
    {
        char LowerAscii(char c) noexcept
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        bool ContainsNoCase(std::string_view text, std::string_view needle) noexcept
        {
            const auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                [](char a, char b) { return LowerAscii(a) == LowerAscii(b); });
            return it != text.end();
        }
    }

    double CycloneStateMachine::ComputeCyclePeriod(double weaponSpeed, double poeAttackSpeedMult) noexcept
    {
        const double w = (std::isfinite(weaponSpeed) && weaponSpeed > 0.0) ? weaponSpeed : 1.0;
        const double m = (std::isfinite(poeAttackSpeedMult) && poeAttackSpeedMult > 0.0) ? poeAttackSpeedMult : 3.0;
        const double hps = std::clamp(w * m, kMinHitsPerSec, kMaxHitsPerSec);
        return 1.0 / hps;
    }

    bool CycloneStateMachine::TryBeginFromPowerAttack(
        bool hasCycloneEquipped,
        bool yieldingToFrenzy,
        bool playerAllowed,
        double currentMagicka,
        double firstCycleCost,
        double currentHealth,
        double firstCycleHealthCost)
    {
        if (phase_ != CyclonePhase::Idle) {
            return false;
        }
        if (!hasCycloneEquipped || yieldingToFrenzy || !playerAllowed) {
            return false;
        }
        const double cost = (std::isfinite(firstCycleCost) && firstCycleCost > 0.0) ? firstCycleCost : 0.0;
        // 결정 97, 122: 시작 이벤트에서는 두 자원 잔액만 읽고, 부족하면 원래 강공격 통과.
        if (currentMagicka + 1e-6 < cost ||
            (firstCycleHealthCost > 0.0 && !(currentHealth - firstCycleHealthCost >= 1.0))) {
            return false;
        }

        phase_ = CyclonePhase::PendingStart;
        elapsedSec_ = 0.0;
        cycleElapsedSec_ = 0.0;
        hitFiredInCycle_ = false;
        outroRemainingSec_ = 0.0;
        return true;
    }

    CycloneStaggerDecision CycloneStateMachine::OnStaggerStart(double magnitude, double ignoreThreshold)
    {
        CycloneStaggerDecision dec;
        if (phase_ == CyclonePhase::Idle) {
            return dec;
        }
        const double mag = std::isfinite(magnitude) ? magnitude : 1.0;
        const double thresh = std::isfinite(ignoreThreshold) ? ignoreThreshold : 0.5;
        if (mag <= thresh) {
            // 결정 56: 한도 이하 경직은 삼키고 회전을 유지한다.
            dec.swallowEvent = true;
            return dec;
        }
        // 결정 150 ③: 한도 초과 경직은 통과시키면서 채널링을 즉시 종료한다.
        ForceStop();
        dec.swallowEvent = false;
        dec.abortedChannel = true;
        return dec;
    }

    bool CycloneStateMachine::ForceStop()
    {
        const bool wasActive = (phase_ != CyclonePhase::Idle);
        phase_ = CyclonePhase::Idle;
        elapsedSec_ = 0.0;
        cycleElapsedSec_ = 0.0;
        hitFiredInCycle_ = false;
        outroRemainingSec_ = 0.0;
        return wasActive;
    }

    CycloneTickOutput CycloneStateMachine::Tick(const CycloneTickInput& in)
    {
        CycloneTickOutput out;
        if (phase_ == CyclonePhase::Idle) {
            return out;
        }

        // 결정 150 ④: 메뉴/일시정지 진입 시 즉시 채널링 종료.
        if (in.gamePaused) {
            const bool hadOar = (phase_ == CyclonePhase::Channeling || phase_ == CyclonePhase::Outro);
            ForceStop();
            out.stopOar = hadOar;
            out.ended = true;
            out.endReason = "menu_or_paused";
            return out;
        }

        if (!in.playerAllowed) {
            const bool hadOar = (phase_ == CyclonePhase::Channeling || phase_ == CyclonePhase::Outro);
            ForceStop();
            out.stopOar = hadOar;
            out.ended = true;
            out.endReason = "player_disallowed";
            return out;
        }

        const double dt = (std::isfinite(in.deltaSec) && in.deltaSec > 0.0) ? std::min(in.deltaSec, 0.1) : 0.0;
        const double period = ComputeCyclePeriod(in.weaponSpeed, in.poeAttackSpeedMult);
        const double cost = (std::isfinite(in.cycleMagickaCost) && in.cycleMagickaCost > 0.0) ? in.cycleMagickaCost : 0.0;
        const double healthCost = (std::isfinite(in.cycleHealthCost) && in.cycleHealthCost > 0.0) ? in.cycleHealthCost : 0.0;
        const bool sufficientHealth = healthCost == 0.0 || in.currentHealth - healthCost >= 1.0;
        const double turnsPerSec = std::clamp(1.0 / period, 0.5, 5.0);

        // 1. 결정 128: 첫 주 갱신에서 첫 주기 비용 차감 성공 시에만 OAR 켜고 채널링 시작.
        if (phase_ == CyclonePhase::PendingStart) {
            if (in.currentMagicka + 1e-6 < cost || !sufficientHealth) {
                ForceStop();
                out.ended = true;
                out.endReason = sufficientHealth ? "insufficient_magicka_on_start" : "insufficient_health_on_start";
                return out;
            }
            phase_ = CyclonePhase::Channeling;
            elapsedSec_ = 0.0;
            // 단일 주기 경과 시간으로 반 주기 타격·주기 종료 비용의 1:1 위상을 고정한다.
            cycleElapsedSec_ = 0.0;
            hitFiredInCycle_ = false;
            out.startOar = true;
            if (cost > 0.0) {
                out.consumeMagicka = true;
                out.magickaToConsume = cost;
            }
            out.healthToConsume = healthCost;
            out.spinRadiansDelta = kTwoPi * turnsPerSec * dt;
            return out;
        }

        // 2. 결정 150 ②: 마무리 구간(Outro)은 무비용·무타격으로 회전만 정리 후 종료.
        if (phase_ == CyclonePhase::Outro) {
            outroRemainingSec_ -= dt;
            out.spinRadiansDelta = kTwoPi * turnsPerSec * dt;
            if (outroRemainingSec_ <= 0.0) {
                ForceStop();
                out.stopOar = true;
                out.ended = true;
                out.endReason = "outro_complete";
            }
            return out;
        }

        // 3. 채널링 진행 중
        elapsedSec_ += dt;
        out.spinRadiansDelta = kTwoPi * turnsPerSec * dt;

        // 버튼이 떼졌고 최소 채널링 시간을 채웠으면 마무리 구간으로 전환(추가 비용·타격 없음).
        if (!in.buttonHeld && elapsedSec_ >= std::max(0.0, in.minChannelSec)) {
            phase_ = CyclonePhase::Outro;
            outroRemainingSec_ = std::max(0.0, in.outroSec);
            if (outroRemainingSec_ <= 0.0) {
                ForceStop();
                out.stopOar = true;
                out.ended = true;
                out.endReason = "released";
            }
            return out;
        }

        // 단일 주기 모델: 비용이 지불된 주기당 반 주기(0.5*period)에 타격 정확히 1회, 주기 말(period)에 다음 비용 판정.
        cycleElapsedSec_ += dt;
        if (!hitFiredInCycle_ && cycleElapsedSec_ >= period * 0.5) {
            out.performAreaHit = true;
            hitFiredInCycle_ = true;
        }

        if (cycleElapsedSec_ >= period) {
            if (in.currentMagicka + 1e-6 < cost || !sufficientHealth) {
                ForceStop();
                out.stopOar = true;
                out.ended = true;
                out.endReason = sufficientHealth ? "out_of_magicka" : "out_of_health";
                return out;
            }
            if (cost > 0.0) {
                out.consumeMagicka = true;
                out.magickaToConsume = cost;
            }
            out.healthToConsume = healthCost;
            // 프레임 스파이크(dt >= period)에서도 반 주기 위상이 깨지지 않도록 잔여 시간을 [0, 0.5*period)로 제한한다.
            cycleElapsedSec_ = std::fmod(cycleElapsedSec_, period);
            if (cycleElapsedSec_ >= period * 0.5) {
                cycleElapsedSec_ = 0.0;
            }
            hitFiredInCycle_ = false;
        }

        return out;
    }

    bool CycloneStateMachine::IsAttackStartEvent(std::string_view eventName)
    {
        if (!ContainsNoCase(eventName, "attack")) {
            return false;
        }
        if (ContainsNoCase(eventName, "stop") || ContainsNoCase(eventName, "_end") || ContainsNoCase(eventName, "win")) {
            return false;
        }
        return ContainsNoCase(eventName, "start") || ContainsNoCase(eventName, "initiate");
    }

    bool CycloneStateMachine::IsPowerAttackStartEvent(std::string_view eventName)
    {
        return IsAttackStartEvent(eventName) && ContainsNoCase(eventName, "power");
    }

    bool CycloneStateMachine::IsStaggerStartEvent(std::string_view eventName)
    {
        constexpr std::string_view kTarget = "staggerstart";
        return eventName.size() == kTarget.size() &&
               std::equal(eventName.begin(), eventName.end(), kTarget.begin(),
                   [](char a, char b) { return LowerAscii(a) == b; });
    }
}
