#include "PCH.h"
#include "runtime/SkillExecution.h"
#include "runtime/FormIds.h"

namespace SkyrimPoE::Runtime
{
    namespace
    {
        Core::KillTracker* g_killTracker = nullptr;
        thread_local std::uint32_t t_pluginDirectDamageDepth = 0;

        struct DirectDamageScope
        {
            DirectDamageScope() noexcept { ++t_pluginDirectDamageDepth; }
            ~DirectDamageScope() noexcept
            {
                if (t_pluginDirectDamageDepth > 0) {
                    --t_pluginDirectDamageDepth;
                }
            }
            DirectDamageScope(const DirectDamageScope&) = delete;
            DirectDamageScope& operator=(const DirectDamageScope&) = delete;
        };

        void SpawnHarmlessExplosionAt(RE::PlayerCharacter* player, const RE::NiPoint3& origin, RE::FormID vfxLocalId)
        {
            if (vfxLocalId == 0 || !player) {
                return;
            }
            auto* expl = FormIds::Lookup<RE::BGSExplosion>(vfxLocalId);
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            if (!expl || !dataHandler) {
                return;
            }
            // 고정된 좌표 origin에 직접 무해 폭발 참조를 생성해 대상 이동·소실 시에도 정확한 위치에서 연출이 터지게 한다.
            (void)dataHandler->CreateReferenceAtLocation(
                expl,
                origin,
                RE::NiPoint3{},
                player->GetParentCell(),
                player->GetWorldspace(),
                nullptr,
                nullptr,
                RE::ObjectRefHandle{},
                false,
                true);
        }
    }

    void SkillExecution::Initialize(Core::KillTracker* killTracker)
    {
        g_killTracker = killTracker;
    }

    bool SkillExecution::IsInPluginDirectDamage() noexcept
    {
        return t_pluginDirectDamageDepth > 0;
    }

    bool SkillExecution::IsAllyOfPlayer(const RE::PlayerCharacter* player, const RE::Actor* actor) noexcept
    {
        if (!actor || !player) {
            return true;
        }
        if (actor == player) {
            return true;
        }
        if (actor->IsPlayerTeammate()) {
            return true;
        }
        if (actor->IsSummonedByPlayer()) {
            return true;
        }
        if (actor->IsCommandedActor() && actor->GetCommandingActor().get() == player) {
            return true;
        }
        return false;
    }

    bool SkillExecution::CanDamageTarget(RE::PlayerCharacter* player, RE::Actor* target, bool isDirectHit)
    {
        if (!player || !target) {
            return false;
        }
        if (target->IsDead() || !target->Is3DLoaded() || target->IsDeleted() || target->IsDisabled()) {
            return false;
        }
        if (target->IsGhost() || target->IsInKillMove()) {
            return false;
        }
        if (IsAllyOfPlayer(player, target)) {
            return false;
        }
        // 결정 109, 133, 139, 148: 번지는 피해는 현재 적대 필수, 직격은 예외.
        if (!isDirectHit && !target->IsHostileToActor(player)) {
            return false;
        }
        return true;
    }

    std::vector<RE::NiPointer<RE::Actor>> SkillExecution::CollectHostileTargets(
        RE::PlayerCharacter* player,
        const RE::NiPoint3& origin,
        float radius)
    {
        std::vector<RE::NiPointer<RE::Actor>> out;
        if (!player || radius <= 0.0f) {
            return out;
        }
        auto* lists = RE::ProcessLists::GetSingleton();
        if (!lists) {
            return out;
        }
        const float radiusSq = radius * radius;
        for (const auto& handle : lists->highActorHandles) {
            auto actor = handle.get();
            if (!actor) {
                continue;
            }
            const auto delta = actor->GetPosition() - origin;
            if (std::abs(delta.z) > kVerticalHeightLimit ||
                (delta.x * delta.x + delta.y * delta.y) > radiusSq) {
                continue;
            }
            if (!CanDamageTarget(player, actor.get(), false)) {
                continue;
            }
            out.push_back(std::move(actor));
        }
        return out;
    }

    double SkillExecution::GetDifficultyMultiplier(bool toPlayer) noexcept
    {
        constexpr std::array<const char*, 6> kSuffix{ "VE", "E", "N", "H", "VH", "L" };
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* settings = RE::GameSettingCollection::GetSingleton();
        if (!player || !settings) {
            return 1.0;
        }
        const auto diff = player->GetGameStatsData().difficulty;
        if (diff < 0 || diff >= static_cast<std::int32_t>(kSuffix.size())) {
            return 1.0;
        }
        const auto name = std::format("{}{}", toPlayer ? "fDiffMultHPToPC" : "fDiffMultHPByPC", kSuffix[static_cast<std::size_t>(diff)]);
        auto* setting = settings->GetSetting(name.c_str());
        if (!setting) {
            return 1.0;
        }
        const double val = static_cast<double>(setting->GetFloat());
        return (std::isfinite(val) && val > 0.0) ? val : 1.0;
    }

    std::size_t SkillExecution::ExecuteAoESpell(
        const RE::NiPoint3& origin,
        float radius,
        double skyrimDamage,
        RE::FormID vfxExplosionLocalId)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded() || player->IsDead()) {
            return 0;
        }

        // 무해 시각·음향 EXPL 연출은 피해가 0이어도(예: Brutality) 시각 피드백으로 띄운다.
        SpawnHarmlessExplosionAt(player, origin, vfxExplosionLocalId);

        if (!std::isfinite(skyrimDamage) || skyrimDamage <= 0.0 || radius <= 0.0f) {
            return 0;
        }

        const auto targets = CollectHostileTargets(player, origin, radius);
        if (targets.empty()) {
            return 0;
        }

        const float dmg = static_cast<float>(skyrimDamage);
        std::size_t hitCount = 0;

        const DirectDamageScope scope;
        for (const auto& victim : targets) {
            // 결정 109, 133, 139: 번지는 피해 적용 직전에 대상 유효성과 현재 적대 여부를 다시 확인한다.
            if (!CanDamageTarget(player, victim.get(), false)) {
                continue;
            }
            // 결정 134, 140: 피해 적용 직전에 이번 삶 첫 피격 적대 여부를 선기록한다.
            if (g_killTracker) {
                g_killTracker->RecordFirstHitHostility(victim->GetFormID(), victim->IsHostileToActor(player));
            }
            // 결정 53, 55: 플레이어를 공격자로 넘겨 처치·어그로·난이도 배율이 정상 반영되도록 한다.
            (void)victim->DoDamage(dmg, player, true);
            ++hitCount;
        }
        return hitCount;
    }
}
