#include "PCH.h"
#include "runtime/TrueHudBridge.h"

#include "core/EnemyEsHud.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/SkillExecution.h"
#include "runtime/TrueHUDAPI.h"

#include <memory>
#include <unordered_map>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        // 액터 FormID → 특수 바에 넘길 현재·최대 ES. 주 스레드가 만들어 통째로 바꿔 끼우고, 값 함수는 읽기만 한다.
        using EsBarTable = std::unordered_map<RE::FormID, Core::EnemyEsBarValues>;

        constexpr auto kTableRebuildInterval = std::chrono::milliseconds(100);

        std::atomic_bool g_ownsSpecialBar{ false };
        TRUEHUD_API::IVTrueHUD1* g_api = nullptr;  // 주 스레드에서만 씀
        // SKSE의 잘못된 플러그인 핸들 값(-1)
        constexpr SKSE::PluginHandle kNoPluginHandle = static_cast<SKSE::PluginHandle>(-1);
        SKSE::PluginHandle g_handle = kNoPluginHandle;
        std::atomic<std::shared_ptr<const EsBarTable>> g_table;
        std::chrono::steady_clock::time_point g_nextTableBuild{};

        const char* ResultName(TRUEHUD_API::APIResult result) noexcept
        {
            switch (result) {
            case TRUEHUD_API::APIResult::OK: return "OK";
            case TRUEHUD_API::APIResult::NotOwner: return "NotOwner";
            case TRUEHUD_API::APIResult::MustKeep: return "MustKeep";
            case TRUEHUD_API::APIResult::AlreadyGiven: return "AlreadyGiven";
            case TRUEHUD_API::APIResult::AlreadyTaken: return "AlreadyTaken";
            default: return "Unknown";
            }
        }

        // TrueHUD가 정보 바·보스 바를 갱신할 때 액터마다 부른다(TrueHUD 메뉴 갱신 쪽). 잠금 없이 표만 읽는다.
        // 표에 없는 액터(플레이어, 아군, ES 없음, 죽음)는 0/0이다.
        float EsBarValue(RE::Actor* actor, bool wantMax) noexcept
        {
            try {
                if (!actor) {
                    return 0.0f;
                }
                const auto table = g_table.load(std::memory_order_acquire);
                if (!table) {
                    return 0.0f;
                }
                const auto it = table->find(actor->GetFormID());
                if (it == table->end()) {
                    return 0.0f;
                }
                return wantMax ? it->second.max : it->second.current;
            } catch (...) {
                return 0.0f;
            }
        }

        void StopUsing(const char* reason)
        {
            g_ownsSpecialBar.store(false, std::memory_order_release);
            g_table.store(nullptr, std::memory_order_release);
            SKSE::log::warn("TrueHudBridge: {}; using the own enemy ES display.", reason);
        }

        void ReleaseSpecialBar(const char* success, const char* failure)
        {
            const auto released = g_api->ReleaseSpecialResourceBarControl(g_handle);
            StopUsing(released == TRUEHUD_API::APIResult::OK ? success : failure);
        }

        // 결정 185와 같은 기준의 적(아군 아님, 처치 보상 기준 적대)이고 ES가 있는 살아 있는 액터만 표에 넣는다.
        void RebuildTable()
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto table = std::make_shared<EsBarTable>();
            if (player) {
                for (const auto& entry : MonsterRuntime::ListEnergyShieldProfiles()) {
                    RE::NiPointer<RE::Actor> actor{ RE::TESForm::LookupByID<RE::Actor>(entry.actorId) };
                    if (!actor || actor->IsDead() || actor->IsDisabled() || actor->IsDeleted() ||
                        SkillExecution::IsAllyOfPlayer(player, actor.get()) ||
                        !SkillExecution::IsHostileForFirstHitReward(actor.get(), player)) {
                        continue;
                    }
                    (*table)[entry.actorId] = Core::MakeEnemyEsBarValues(entry.currentEs, entry.maxEs);
                }
            }
            g_table.store(std::shared_ptr<const EsBarTable>(std::move(table)), std::memory_order_release);
        }
    }

    void TrueHudBridge::Initialize(const Core::Config* config)
    {
        g_ownsSpecialBar.store(false, std::memory_order_release);
        g_table.store(nullptr, std::memory_order_release);
        if (!config || !config->trueHudSpecialBar) {
            SKSE::log::info("TrueHudBridge: TrueHudSpecialBar is off; using the own enemy ES display.");
            return;
        }
        // 특수 바 함수는 모두 IVTrueHUD1에 있으므로 V1로 요청한다(설치본 1.1.9.0은 V1~V4를 줌).
        g_api = static_cast<TRUEHUD_API::IVTrueHUD1*>(TRUEHUD_API::RequestPluginAPI(TRUEHUD_API::InterfaceVersion::V1));
        if (!g_api) {
            SKSE::log::warn("TrueHudBridge: TrueHUD.dll or its interface is unavailable; using the own enemy ES display.");
            return;
        }
        g_handle = SKSE::GetPluginHandle();
        const auto request = g_api->RequestSpecialResourceBarsControl(g_handle);
        if (request != TRUEHUD_API::APIResult::OK && request != TRUEHUD_API::APIResult::AlreadyGiven) {
            SKSE::log::warn("TrueHudBridge: special bar request returned {} (owner handle {}, ours {}); using the own enemy ES display.",
                ResultName(request), g_api->GetSpecialResourceBarControlOwner(), g_handle);
            return;
        }
        RebuildTable();
        g_nextTableBuild = std::chrono::steady_clock::now() + kTableRebuildInterval;
        const auto registered = g_api->RegisterSpecialResourceFunctions(
            g_handle,
            [](RE::Actor* actor) { return EsBarValue(actor, false); },
            [](RE::Actor* actor) { return EsBarValue(actor, true); },
            true,    // 최대값이 기본, 0이 깜빡임 문턱(바가 보이는 조건은 TrueHUD MCM의 특수 바 표시 설정)
            false);  // 플레이어에게는 표시하지 않음(플레이어 ES는 자체 HUD)
        if (registered != TRUEHUD_API::APIResult::OK) {
            const auto released = g_api->ReleaseSpecialResourceBarControl(g_handle);
            g_table.store(nullptr, std::memory_order_release);
            SKSE::log::warn("TrueHudBridge: registering the ES functions returned {} (release {}); using the own enemy ES display.",
                ResultName(registered), ResultName(released));
            return;
        }
        g_ownsSpecialBar.store(true, std::memory_order_release);
        SKSE::log::info("TrueHudBridge: took the TrueHUD special bars (request {}, handle {}); the own enemy ES display is hidden.",
            ResultName(request), g_handle);
    }

    void TrueHudBridge::TickMainThread()
    {
        if (!g_ownsSpecialBar.load(std::memory_order_acquire)) {
            return;
        }
        if (!g_api) {
            StopUsing("lost the TrueHUD interface");
            return;
        }
        // 안전 제거 정리 뒤에는 넘길 ES가 없으므로 다른 모드가 쓸 수 있게 바를 놓는다.
        if (MapDeviceRuntime::IsUninstallCleanupArmed()) {
            ReleaseSpecialBar("released the TrueHUD special bars for the safe uninstall",
                "could not release the TrueHUD special bars for the safe uninstall");
            return;
        }
        if (g_api->GetSpecialResourceBarControlOwner() != g_handle) {
            StopUsing("the TrueHUD special bars are no longer ours");
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= g_nextTableBuild) {
            g_nextTableBuild = now + kTableRebuildInterval;
            RebuildTable();
        }
    }

    void TrueHudBridge::SuspendSpecialBarOnMainThread()
    {
        if (!OwnsSpecialBar()) {
            return;
        }
        if (g_api) {
            ReleaseSpecialBar("released the TrueHUD special bars during preservation",
                "could not release the TrueHUD special bars during preservation");
        } else {
            StopUsing("lost the TrueHUD interface");
        }
        SKSE::log::info("TrueHudBridge: special bar control will be requested again in the next game session.");
    }

    bool TrueHudBridge::OwnsSpecialBar() noexcept
    {
        return g_ownsSpecialBar.load(std::memory_order_acquire);
    }
}
