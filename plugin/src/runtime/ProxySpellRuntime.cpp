#include "PCH.h"
#include "runtime/ProxySpellRuntime.h"
#include "runtime/CombatHooks.h"
#include "runtime/FormIds.h"
#include "runtime/MapDeviceRuntime.h"
#include "runtime/ProjectileRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"
#include "runtime/UiBridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct ProxySlotBinding
        {
            RE::SpellItem* proxySpell = nullptr;
            RE::SpellItem* proxyPower = nullptr;
            std::atomic<std::uint64_t> lastToastMs{ 0 };
            std::atomic<bool> isSpellActive{ false };
            std::atomic<bool> isPowerActive{ false };
        };

        // 검수 M2: 캐스터(왼손/오른손/포효) 단위로 CheckCast 승인을 분리해 양손 동시 시전 경합과 2초 조준 타임아웃 불발을 방지
        struct PendingCastAuth
        {
            std::uint8_t slotIdx = 0;
            bool isPower = false;
            bool dualCast = false;
            std::uint64_t timestampMs = 0;
            std::uint64_t generation = 0;
        };

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::SkillSystem* g_skillSystem = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::Config* g_config = nullptr;

        std::array<ProxySlotBinding, Core::kSkillSlotCount> g_bindings{};
        std::mutex g_authMutex;
        std::unordered_map<const RE::MagicCaster*, PendingCastAuth> g_pendingAuthByCaster;
        std::atomic<std::uint64_t> g_handCastCount{ 0 };
        std::atomic<std::uint64_t> g_guardActivationCount{ 0 };
        std::atomic<std::uint64_t> g_dllMagickaChargeCount{ 0 };
        std::atomic<double> g_lastDllMagickaCharge{ 0.0 };
        std::atomic<std::uint8_t> g_lastDllMagickaChargeSlot{ 0 };
        bool g_hooksInstalled = false;
        double g_lastSyncedGuardArmour = 0.0;
        std::uint64_t g_lastSyncedSnapVersion = 0;

        std::uint64_t NowMs() noexcept
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
        }

        std::string ResolveGemDisplayName(std::string_view gemId, std::string_view fallbackName)
        {
            // C-25: 하드코딩 대신 lang/*.json(UiBridge) 우선 조회, 없으면 DataPack 이름/ID 폴백
            const std::string localized = UiBridge::GetGemDisplayName(gemId);
            if (!localized.empty() && localized != gemId) {
                return localized;
            }
            if (!fallbackName.empty()) {
                return std::string(fallbackName);
            }
            return std::string(gemId);
        }

        // 주어진 MagicItem이 우리 18개 대리 레코드 중 하나인지 판별
        bool MatchProxyForm(const RE::MagicItem* item, std::uint8_t& outSlot, bool& outIsPower) noexcept
        {
            if (!item) return false;
            for (std::size_t i = 0; i < Core::kSkillSlotCount; ++i) {
                if (g_bindings[i].proxySpell == item) {
                    outSlot = static_cast<std::uint8_t>(i);
                    outIsPower = false;
                    return true;
                }
                if (g_bindings[i].proxyPower == item) {
                    outSlot = static_cast<std::uint8_t>(i);
                    outIsPower = true;
                    return true;
                }
            }
            return false;
        }

        bool IsPlayerCaster(RE::MagicCaster* caster) noexcept
        {
            if (!caster) return false;
            auto* player = RE::PlayerCharacter::GetSingleton();
            return player && caster->GetCasterAsActor() == player;
        }

        void MaybeQueueCooldownNotice(std::uint64_t generation, std::uint8_t slotIdx, std::string msg)
        {
            // 메뉴 질의(MagicMenu/FavoritesMenu 등) 중에는 토스트 완전 억제 + 슬롯별 1초 스로틀
            auto* ui = RE::UI::GetSingleton();
            if (!ui || ui->GameIsPaused() || !g_queue) {
                return;
            }
            const auto now = NowMs();
            const auto prev = g_bindings[slotIdx].lastToastMs.load(std::memory_order_relaxed);
            if (now < prev + 1000ULL) {
                return;
            }
            g_bindings[slotIdx].lastToastMs.store(now, std::memory_order_relaxed);
            g_queue->EnqueueForGeneration(generation, [m = std::move(msg)]() {
                RE::SendHUDMessage::ShowHUDMessage(m.c_str());
            });
        }

        void SyncGuardArmourToLayer2(double bonusArmour)
        {
            if (std::abs(bonusArmour - g_lastSyncedGuardArmour) < 1e-4) {
                return;
            }
            g_lastSyncedGuardArmour = bonusArmour;
            if (g_statEngine) {
                g_statEngine->UpdateLayer2([bonusArmour](Core::Layer2Input& l2) {
                    l2.moltenShellBonusArmour = bonusArmour;
                });
            }
        }

        double ReadDualCastMagickaMultiplier(bool dualCast)
        {
            if (!dualCast) return 1.0;
            auto* settings = RE::GameSettingCollection::GetSingleton();
            const auto* value = settings ? settings->GetSetting("fMagicDualCastingCostMult") : nullptr;
            return value && value->GetType() == RE::Setting::Type::kFloat ? value->GetFloat() : 1.0;
        }

        // 시험 진단용 시전 추적. AutoTest가 켜고 끈다(일반 플레이에서는 꺼져 있어 기록하지 않음).
        std::atomic<bool> g_castTrace{ false };

        struct CheckCastHook
        {
            static constexpr std::size_t kIndex = 0x0A;

            static bool Thunk(
                RE::MagicCaster* caster,
                RE::MagicItem* spell,
                bool dualCast,
                float* alchStrength,
                RE::MagicSystem::CannotCastReason* reason,
                bool useBaseValueForCost)
            {
                const bool result = Decide(caster, spell, dualCast, alchStrength, reason, useBaseValueForCost);
                if (g_castTrace.load(std::memory_order_relaxed) && IsPlayerCaster(caster)) {
                    SKSE::log::info("ProxySpellRuntime trace: CheckCast spell=0x{:08X} dual={} result={} reason={}",
                        spell ? spell->GetFormID() : 0u, dualCast, result,
                        reason ? static_cast<std::uint32_t>(*reason) : 0xFFFFFFFFu);
                }
                return result;
            }

            static bool Decide(
                RE::MagicCaster* caster,
                RE::MagicItem* spell,
                bool dualCast,
                float* alchStrength,
                RE::MagicSystem::CannotCastReason* reason,
                bool useBaseValueForCost)
            {
                const auto generation = g_queue ? g_queue->CurrentGeneration() : 0;
                const bool origOk = original(caster, spell, dualCast, alchStrength, reason, useBaseValueForCost);
                if (!origOk || MapDeviceRuntime::IsUninstallCleanupActive() || !IsPlayerCaster(caster)) {
                    return origOk;
                }

                std::uint8_t slotIdx = 0;
                bool isPower = false;
                if (!MatchProxyForm(spell, slotIdx, isPower)) {
                    return origOk;
                }

                if (!g_statEngine || !g_skillSystem || !g_config) {
                    return origOk;
                }
                const auto snap = g_statEngine->GetSnapshot();
                if (!snap) {
                    return origOk;
                }
                const auto& slotStat = snap->slotStats[slotIdx];
                const auto gemGeneration = SaveLoad::WorldGemGeneration();
                if (snap->gemGeneration != gemGeneration) {
                    if (reason) *reason = RE::MagicSystem::CannotCastReason::kPowerUsed;
                    return false;
                }
                if (gemGeneration != 0) {
                    const auto use = Core::GemForms::ClassifyUse(gemGeneration, slotStat.gemId);
                    if (!use.execute || (!isPower && !ProjectileRuntime::HasSpellDelivery(gemGeneration, slotStat.gemId))) {
                        if (reason) *reason = RE::MagicSystem::CannotCastReason::kPowerUsed;
                        MaybeQueueCooldownNotice(generation, slotIdx, use.execute
                            ? "이 젬의 실행기가 연결되지 않았습니다."
                            : std::string(Core::GemForms::UseReasonText(use.reason)));
                        return false;
                    }
                }
                const bool slotActive = isPower
                    ? g_bindings[slotIdx].isPowerActive.load(std::memory_order_acquire)
                    : g_bindings[slotIdx].isSpellActive.load(std::memory_order_acquire);
                if (!slotActive || slotStat.gemId.empty()) {
                    if (reason) *reason = RE::MagicSystem::CannotCastReason::kPowerUsed;
                    return false;
                }

                // 요구치 미달 차단
                if (g_config->enforceGemRequirements && !slotStat.requirementMet) {
                    if (reason) *reason = RE::MagicSystem::CannotCastReason::kPowerUsed;
                    MaybeQueueCooldownNotice(generation, slotIdx, "SkyrimPoE: 스킬 젬 요구치를 충족하지 못했습니다.");
                    return false;
                }

                auto* player = RE::PlayerCharacter::GetSingleton();
                auto* av = player ? player->AsActorValueOwner() : nullptr;

                if (isPower) {
                    const auto gs = g_skillSystem->GuardSnapshot();
                    if (gs.active) {
                        if (reason) *reason = RE::MagicSystem::CannotCastReason::kPowerUsed;
                        MaybeQueueCooldownNotice(generation, slotIdx, "SkyrimPoE: 용암 방패가 이미 활성 중입니다.");
                        return false;
                    }
                    if (gs.cooldownRemainingSec > 0.0) {
                        if (reason) *reason = RE::MagicSystem::CannotCastReason::kPowerUsed;
                        MaybeQueueCooldownNotice(generation, slotIdx,
                            std::format("SkyrimPoE: 용암 방패 재사용 대기 ({:.1f}초)", gs.cooldownRemainingSec));
                        return false;
                    }
                } else {
                    const double remCd = g_skillSystem->RemainingCooldown(slotStat.gemInstanceId);
                    if (remCd > 0.0) {
                        if (reason) *reason = RE::MagicSystem::CannotCastReason::kPowerUsed;
                        MaybeQueueCooldownNotice(generation, slotIdx,
                            std::format("SkyrimPoE: 스킬 재사용 대기 ({:.1f}초)", remCd));
                        return false;
                    }
                }

                // Guard powers and PoE2 passive builds defer the cost to the queued cast.
                if (av && (isPower || snap->poe2Passives || snap->bloodMagic)) {
                    const double maxHp = av->GetPermanentActorValue(RE::ActorValue::kHealth);
                    const double baseMaxMag = CombatHooks::GetBaseSkyrimMaxMagicka(player);
                    const auto costs = Core::ComputeSkillResourceCost(slotStat, *snap, baseMaxMag, maxHp,
                        ReadDualCastMagickaMultiplier(dualCast && !isPower));
                    if (!costs.CanPayHealth(av->GetActorValue(RE::ActorValue::kHealth))) {
                        if (reason) *reason = RE::MagicSystem::CannotCastReason::kMagicka;
                        MaybeQueueCooldownNotice(generation, slotIdx, "SkyrimPoE: 생명력이 부족합니다.");
                        return false;
                    }
                    if (!costs.CanPayMagicka(av->GetActorValue(RE::ActorValue::kMagicka))) {
                        if (reason) *reason = RE::MagicSystem::CannotCastReason::kMagicka;
                        MaybeQueueCooldownNotice(generation, slotIdx, "SkyrimPoE: 매지카가 부족합니다.");
                        return false;
                    }
                }

                // 결정 90 + 검수 M2 + C-24: 메뉴 일시정지 질의에서는 승인을 기록하지 않고, 실제 게임 중 캐스터별 승인·이중시전 여부 기록
                auto* ui = RE::UI::GetSingleton();
                if (!ui || !ui->GameIsPaused()) {
                    std::scoped_lock lk(g_authMutex);
                    if (g_queue && generation == g_queue->CurrentGeneration() &&
                        !MapDeviceRuntime::IsUninstallCleanupActive()) {
                        g_pendingAuthByCaster[caster] = PendingCastAuth{ slotIdx, isPower, dualCast, NowMs(), generation };
                    }
                }
                return true;
            }

            static inline REL::Relocation<decltype(Thunk)> original;
        };

        struct SpellCastHook
        {
            static constexpr std::size_t kIndex = 0x09;

            static void Thunk(RE::MagicCaster* caster, bool doCast, std::uint32_t arg2, RE::MagicItem* spell)
            {
                const auto generation = g_queue ? g_queue->CurrentGeneration() : 0;
                original(caster, doCast, arg2, spell);
                if (g_castTrace.load(std::memory_order_relaxed) && IsPlayerCaster(caster)) {
                    SKSE::log::info("ProxySpellRuntime trace: SpellCast spell=0x{:08X} doCast={} arg2={}",
                        spell ? spell->GetFormID() : 0u, doCast, arg2);
                }
                if (!doCast || !g_queue || MapDeviceRuntime::IsUninstallCleanupActive() || !IsPlayerCaster(caster)) {
                    return;
                }

                std::uint8_t slotIdx = 0;
                bool isPower = false;
                if (!MatchProxyForm(spell, slotIdx, isPower)) {
                    return;
                }

                // 결정 90 + 검수 M2 + C-24: 동일 캐스터의 CheckCast 통과 승인·이중시전 소비(최대 30초 차징 유지 허용)
                constexpr std::uint64_t kMaxChargeHoldMs = 30000ULL;
                bool authorized = false;
                bool dualCast = false;
                {
                    std::scoped_lock lk(g_authMutex);
                    const auto it = g_pendingAuthByCaster.find(caster);
                    if (it != g_pendingAuthByCaster.end() && it->second.generation == generation) {
                        const auto now = NowMs();
                        if (it->second.slotIdx == slotIdx &&
                            it->second.isPower == isPower &&
                            now <= it->second.timestampMs + kMaxChargeHoldMs) {
                            authorized = true;
                            dualCast = it->second.dualCast;
                        }
                        g_pendingAuthByCaster.erase(it);
                    }
                }
                if (g_castTrace.load(std::memory_order_relaxed)) {
                    SKSE::log::info("ProxySpellRuntime trace: SpellCast slot={} power={} authorized={}", slotIdx + 1, isPower, authorized);
                }
                if (!authorized) {
                    return;
                }

                // C-24: 시전 직후 시점의 조준 원점·방향·이중 시전 여부를 캡처해 주 갱신 이연 실행으로 전달
                RE::NiPoint3 capturedOrigin{};
                RE::NiPoint3 capturedDir{};
                bool hasAim = false;
                if (!isPower) {
                    if (auto* pl = RE::PlayerCharacter::GetSingleton()) {
                        ProjectileRuntime::ComputePlayerAim(pl, capturedOrigin, capturedDir);
                        hasAim = true;
                    }
                }

                g_queue->EnqueueForGeneration(generation, [slotIdx, isPower, capturedOrigin, capturedDir, hasAim, dualCast]() {
                    if (isPower) {
                        (void)ProxySpellRuntime::ExecuteGuardPowerSlot(slotIdx);
                    } else {
                        // The queued cast checks the snapshot to select the passive tree's cost path.
                        (void)ProxySpellRuntime::ExecuteHandCastSlot(
                            slotIdx,
                            false,
                            hasAim ? &capturedOrigin : nullptr,
                            hasAim ? &capturedDir : nullptr,
                            dualCast);
                    }
                });
            }

            static inline REL::Relocation<decltype(Thunk)> original;
        };

        template <class Base, class Result, class... Args>
        auto MemberFnToThunk(Result (Base::*)(Args...)) -> Result (*)(Base*, Args...);
        static_assert(std::is_same_v<decltype(&CheckCastHook::Thunk), decltype(MemberFnToThunk(&RE::MagicCaster::CheckCast))>);
        static_assert(std::is_same_v<decltype(&SpellCastHook::Thunk), decltype(MemberFnToThunk(&RE::MagicCaster::SpellCast))>);
    }

    // 대리 주문·파워에는 효과가 하나 있어야 한다. 2026-11-25까지의 ESP는 효과 항목(EFID)에 데이터(EFIT)가 없어 엔진이 그 효과를
    // 버렸고, 그래서 MagicItem::GetCostliestEffectItem()이 null이었다(AE 1.6.1170 디컴파일: 효과 목록이 비면 null).
    // 이 값을 확인하지 않고 읽는 다른 모드(DynamicMagicModificationFramework의 RequestCast 훅)는 손 시전 때 튕겼다.
    // ESP에 EFIT(크기·범위·지속 0)를 넣어 고쳤고, 여기서는 그것이 실제로 불러와졌는지만 확인한다.
    void ProxySpellRuntime::SetCastTraceForTest(bool enabled)
    {
        g_castTrace.store(enabled, std::memory_order_relaxed);
        SKSE::log::info("ProxySpellRuntime trace: {}.", enabled ? "on" : "off");
    }

    void ProxySpellRuntime::EnsureProxyCostliestEffects()
    {
        std::size_t missing = 0;
        for (std::size_t i = 0; i < Core::kSkillSlotCount; ++i) {
            for (auto* spell : { g_bindings[i].proxySpell, g_bindings[i].proxyPower }) {
                if (spell && !spell->GetCostliestEffectItem()) {
                    ++missing;
                }
            }
        }
        if (missing > 0) {
            SKSE::log::warn("ProxySpellRuntime: {} proxy spells/powers have no costliest effect (old ESP without EFIT?); "
                "mods that read it without a null check (e.g. DynamicMagicModificationFramework) may crash on hand casts.", missing);
        } else {
            SKSE::log::info("ProxySpellRuntime: every proxy spell/power has a costliest effect.");
        }
    }
    bool ProxySpellRuntime::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        Core::SkillSystem* skillSystem,
        const Core::DataPack* dataPack,
        const Core::Config* config)
    {
        g_queue = queue;
        g_statEngine = statEngine;
        g_skillSystem = skillSystem;
        g_dataPack = dataPack;
        g_config = config;

        for (std::size_t i = 0; i < Core::kSkillSlotCount; ++i) {
            g_bindings[i].proxySpell = FormIds::Lookup<RE::SpellItem>(FormIds::kProxySpells[i]);
            g_bindings[i].proxyPower = FormIds::Lookup<RE::SpellItem>(FormIds::kProxyPowers[i]);
        }
        EnsureProxyCostliestEffects();

        if (!g_hooksInstalled) {
            REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_ActorMagicCaster[0] };
            CheckCastHook::original = vtable.write_vfunc(CheckCastHook::kIndex, CheckCastHook::Thunk);
            SpellCastHook::original = vtable.write_vfunc(SpellCastHook::kIndex, SpellCastHook::Thunk);
            g_hooksInstalled = true;
            SKSE::log::info("ProxySpellRuntime: installed ActorMagicCaster CheckCast(0x0A) + SpellCast(0x09) hooks.");
        }
        return true;
    }

    void ProxySpellRuntime::SyncProxySpellsFromState()
    {
        if (MapDeviceRuntime::IsUninstallCleanupActive()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !g_dataPack || !g_config || !g_statEngine) {
            return;
        }

        const auto st = SaveLoad::GetStateCopy();
        const auto snap = g_statEngine->GetSnapshot();
        if (!snap || snap->gemGeneration != SaveLoad::WorldGemGeneration()) {
            return;
        }
        const double baseSkyrimMaxMagicka = CombatHooks::GetBaseSkyrimMaxMagicka(player);

        // 결정 91(C-25): 같은 이름의 대리 주문이 2개 이상이면 2번째부터 "[2]", "[3]" 접미사 부착
        std::unordered_map<std::string, int> spellNameCounts;
        for (std::size_t i = 0; i < Core::kSkillSlotCount; ++i) {
            if (!st.slots[i].occupied) continue;
            const auto& id = st.slots[i].gem.gemId;
            if (!Core::GemForms::AllowsExecution(snap->gemGeneration, id)) continue;
            const auto* def = g_dataPack->FindGem(id, snap->gemGeneration);
            if (!def) continue;
            if (def->kind == Core::GemKind::Spell || def->kind == Core::GemKind::Guard) {
                ++spellNameCounts[def->id];
            }
        }
        std::unordered_map<std::string, int> spellNameOrdinal;

        for (std::size_t i = 0; i < Core::kSkillSlotCount; ++i) {
            auto* sp = g_bindings[i].proxySpell;
            auto* pw = g_bindings[i].proxyPower;
            const auto& slot = st.slots[i];
            const auto* def = slot.occupied && Core::GemForms::AllowsExecution(snap->gemGeneration, slot.gem.gemId)
                ? g_dataPack->FindGem(slot.gem.gemId, snap->gemGeneration) : nullptr;
            const auto& sstat = snap->slotStats[i];

            const bool wantSpell = def && (def->kind == Core::GemKind::Spell);
            const bool wantPower = def && (def->kind == Core::GemKind::Guard);

            if (sp) {
                if (wantSpell) {
                    const int ord = ++spellNameOrdinal[def->id];
                    std::string dispName = std::string("PoE: ") + ResolveGemDisplayName(def->id, def->name);
                    if (spellNameCounts[def->id] > 1 && ord >= 2) {
                        dispName += std::format(" [{}]", ord);
                    }
                    sp->fullName = dispName.c_str();

                    std::int32_t skyrimCost = 0;
                    // PoE2 passive builds pay together after rechecking both resources.
                    if (!snap->poe2Passives && !snap->bloodMagic && sstat.poeManaCost > 0.0) {
                        const auto ms = Core::ComputeMagickaScaling(
                            baseSkyrimMaxMagicka, snap->poeMaxMana, sstat.poeManaCost, snap->expectedPoeMana);
                        skyrimCost = static_cast<std::int32_t>(std::max<long long>(0LL, std::llround(ms.skyrimSkillCost)));
                    }
                    sp->data.costOverride = skyrimCost;
                    sp->data.flags.set(RE::SpellItem::SpellFlag::kCostOverride);
                    const double baseCast = (def->castTime > 0.0) ? def->castTime : 0.65;
                    sp->data.chargeTime = static_cast<float>(std::clamp(baseCast / std::max(0.25, sstat.castSpeedMult), 0.15, 1.5));

                    if (!player->HasSpell(sp)) {
                        player->AddSpell(sp);
                    }
                    g_bindings[i].isSpellActive.store(true, std::memory_order_release);
                } else {
                    sp->fullName = std::format("PoE 주문 칸 {}", i + 1).c_str();
                    sp->data.costOverride = 10;
                    sp->data.chargeTime = 0.5f;
                    if (player->HasSpell(sp)) {
                        player->RemoveSpell(sp);
                    }
                    g_bindings[i].isSpellActive.store(false, std::memory_order_release);
                }
            }

            if (pw) {
                if (wantPower) {
                    const int ord = ++spellNameOrdinal[def->id];
                    std::string dispName = std::string("PoE: ") + ResolveGemDisplayName(def->id, def->name);
                    if (spellNameCounts[def->id] > 1 && ord >= 2) {
                        dispName += std::format(" [{}]", ord);
                    }
                    pw->fullName = dispName.c_str();
                    pw->data.costOverride = 0;
                    pw->data.flags.set(RE::SpellItem::SpellFlag::kCostOverride);
                    if (!player->HasSpell(pw)) {
                        player->AddSpell(pw);
                    }
                    g_bindings[i].isPowerActive.store(true, std::memory_order_release);
                } else {
                    pw->fullName = std::format("PoE 파워 칸 {}", i + 1).c_str();
                    pw->data.costOverride = 0;
                    if (player->HasSpell(pw)) {
                        player->RemoveSpell(pw);
                    }
                    g_bindings[i].isPowerActive.store(false, std::memory_order_release);
                }
            }
        }
    }

    bool ProxySpellRuntime::ExecuteHandCastSlot(
        std::uint8_t slotIndex,
        bool chargeMagicka,
        const RE::NiPoint3* capturedOrigin,
        const RE::NiPoint3* capturedDir,
        bool dualCast)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (MapDeviceRuntime::IsUninstallCleanupActive() ||
            !player || player->IsDead() || !g_statEngine || !g_skillSystem || !g_dataPack || !g_config) {
            return false;
        }
        const auto st = SaveLoad::GetStateCopy();
        const auto snap = g_statEngine->GetSnapshot();
        if (!snap) return false;

        const auto gemGeneration = SaveLoad::WorldGemGeneration();
        if (snap->gemGeneration != gemGeneration) return false;
        if (gemGeneration != 0) {
            if (slotIndex >= st.slots.size()) return false;
            const auto& id = st.slots[slotIndex].gem.gemId;
            if (!Core::GemForms::AllowsExecution(gemGeneration, id) || !ProjectileRuntime::HasSpellDelivery(gemGeneration, id)) {
                return false;
            }
        }

        auto* av = player->AsActorValueOwner();
        if (!av) return false;
        const double curMag = static_cast<double>(av->GetActorValue(RE::ActorValue::kMagicka));
        const double baseMaxMag = CombatHooks::GetBaseSkyrimMaxMagicka(player);
        const double curHp = static_cast<double>(av->GetActorValue(RE::ActorValue::kHealth));
        const double maxHp = std::max(1.0, static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth)));

        // The legacy tree pays via engine charging. The PoE2 tree pays after validation.
        const bool payMagicka = chargeMagicka || snap->poe2Passives;
        const double checkMag = payMagicka ? curMag : std::numeric_limits<double>::max();
        const double manaMultiplier = ReadDualCastMagickaMultiplier(snap->poe2Passives && dualCast);
        auto castOpt = g_skillSystem->TryBuildHandCastSpell(
            slotIndex, *snap, st.slots, checkMag, baseMaxMag, *g_dataPack, *g_config, curHp, maxHp, manaMultiplier);
        if (!castOpt.has_value()) {
            return false;
        }

        // C-24: 이중 시전(양손 합동 시전) 시 엔진 매지카 이중 소모에 대응해 피해 1.5배·범위 1.15배 반영
        if (dualCast) {
            constexpr double kDualCastDamageMult = 1.5;
            Core::DamageTypeValues multipliers;
            multipliers.fill(kDualCastDamageMult);
            Core::ScaleDamagePacket(castOpt->poeBaseHitDamage, multipliers);
            Core::ScaleDamagePacket(castOpt->targetAilmentHitDamage.basis, multipliers);
            castOpt->areaMult *= 1.15;
        }

        if (castOpt->skyrimHealthCost > 0.0) {
            av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth,
                -static_cast<float>(castOpt->skyrimHealthCost));
        }
        if (payMagicka && castOpt->skyrimMagickaCost > 0.0) {
            const float charged = static_cast<float>(castOpt->skyrimMagickaCost);
            av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka, -charged);
            g_lastDllMagickaCharge.store(static_cast<double>(charged), std::memory_order_relaxed);
            g_lastDllMagickaChargeSlot.store(slotIndex, std::memory_order_relaxed);
            g_dllMagickaChargeCount.fetch_add(1, std::memory_order_release);
        }

        RE::NiPoint3 origin{}, dir{};
        if (capturedOrigin && capturedDir) {
            origin = *capturedOrigin;
            dir = *capturedDir;
        } else {
            ProjectileRuntime::ComputePlayerAim(player, origin, dir);
        }
        if (ProjectileRuntime::LaunchSpellDelivery(*castOpt, origin, dir)) {
            g_handCastCount.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    bool ProxySpellRuntime::ExecuteGuardPowerSlot(std::uint8_t slotIndex)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (MapDeviceRuntime::IsUninstallCleanupActive() ||
            !player || player->IsDead() || !g_statEngine || !g_skillSystem || !g_dataPack || !g_config) {
            return false;
        }
        const auto st = SaveLoad::GetStateCopy();
        const auto snap = g_statEngine->GetSnapshot();
        if (!snap) return false;

        const auto gemGeneration = SaveLoad::WorldGemGeneration();
        if (snap->gemGeneration != gemGeneration) return false;
        if (gemGeneration != 0 && (slotIndex >= st.slots.size() ||
            !Core::GemForms::AllowsExecution(gemGeneration, st.slots[slotIndex].gem.gemId))) {
            return false;
        }

        auto* av = player->AsActorValueOwner();
        if (!av) return false;
        const double curMag = static_cast<double>(av->GetActorValue(RE::ActorValue::kMagicka));
        const double baseMaxMag = CombatHooks::GetBaseSkyrimMaxMagicka(player);
        const double curHp = static_cast<double>(av->GetActorValue(RE::ActorValue::kHealth));
        const double maxHp = std::max(1.0, static_cast<double>(av->GetPermanentActorValue(RE::ActorValue::kHealth)));
        const double baseMaxHp = std::max(50.0, maxHp - st.appliedHealthDelta);

        const auto act = g_skillSystem->TryActivateMoltenShell(
            slotIndex, *snap, st.slots, curMag, baseMaxMag, baseMaxHp, *g_dataPack, *g_config, curHp, maxHp);
        if (!act.ok) {
            return false;
        }

        if (act.skyrimHealthCost > 0.0) {
            av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth,
                -static_cast<float>(act.skyrimHealthCost));
        }
        if (act.skyrimMagickaCost > 0.0) {
            av->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kMagicka,
                -static_cast<float>(act.skyrimMagickaCost));
        }

        SyncGuardArmourToLayer2(act.bonusFlatArmour);
        SkillExecution::SpawnHarmlessVfx(player->GetPosition(), FormIds::kVfxFireBurst);
        g_guardActivationCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void ProxySpellRuntime::Tick(double deltaSec, bool gamePaused)
    {
        // 검수 M1: StatSnapshot::version 변화 감지 시 대리 주문 18종(습득/제거/비용/시전시간/[칸 N]) 자동 동기화
        if (!gamePaused && g_statEngine) {
            if (const auto snap = g_statEngine->GetSnapshot(); snap && snap->version != g_lastSyncedSnapVersion) {
                g_lastSyncedSnapVersion = snap->version;
                SyncProxySpellsFromState();
            }
        }

        if (!g_skillSystem) return;
        const auto tickRes = g_skillSystem->TickGuardShield(deltaSec, gamePaused);
        const auto gs = g_skillSystem->GuardSnapshot();
        SyncGuardArmourToLayer2(gs.active ? gs.bonusFlatArmour : 0.0);

        if (tickRes.expiredNow && tickRes.reflectedFireSkyrim > 0.0) {
            if (auto* player = RE::PlayerCharacter::GetSingleton(); player && !player->IsDead()) {
                (void)SkillExecution::ExecuteAoESpell(
                    player->GetPosition(), 320.0f, tickRes.reflectedFireSkyrim,
                    FormIds::kVfxFireBurst, Core::DamageType::Fire);
            }
        }
    }

    std::uint64_t ProxySpellRuntime::HandCastCount() noexcept
    {
        return g_handCastCount.load(std::memory_order_relaxed);
    }

    std::uint64_t ProxySpellRuntime::GuardActivationCount() noexcept
    {
        return g_guardActivationCount.load(std::memory_order_relaxed);
    }

    ProxySpellRuntime::MagickaChargeDiagnostics ProxySpellRuntime::GetMagickaChargeDiagnostics() noexcept
    {
        const auto count = g_dllMagickaChargeCount.load(std::memory_order_acquire);
        return {
            count,
            g_lastDllMagickaCharge.load(std::memory_order_relaxed),
            g_lastDllMagickaChargeSlot.load(std::memory_order_relaxed)
        };
    }

    void ProxySpellRuntime::ResetOnRevert()
    {
        {
            std::scoped_lock lk(g_authMutex);
            g_pendingAuthByCaster.clear();
        }
        for (auto& b : g_bindings) {
            b.lastToastMs.store(0, std::memory_order_relaxed);
            b.isSpellActive.store(false, std::memory_order_relaxed);
            b.isPowerActive.store(false, std::memory_order_relaxed);
        }
        g_lastSyncedSnapVersion = 0;
        SyncGuardArmourToLayer2(0.0);
    }

    void ProxySpellRuntime::RemoveAllProxySpells(RE::PlayerCharacter* player)
    {
        if (!player) {
            return;
        }
        for (std::size_t i = 0; i < Core::kSkillSlotCount; ++i) {
            if (auto* sp = FormIds::Lookup<RE::SpellItem>(FormIds::kProxySpells[i])) {
                if (player->HasSpell(sp)) {
                    player->RemoveSpell(sp);
                }
            }
            if (auto* pw = FormIds::Lookup<RE::SpellItem>(FormIds::kProxyPowers[i])) {
                if (player->HasSpell(pw)) {
                    player->RemoveSpell(pw);
                }
            }
            g_bindings[i].isSpellActive.store(false, std::memory_order_release);
            g_bindings[i].isPowerActive.store(false, std::memory_order_release);
        }
        SyncGuardArmourToLayer2(0.0);
    }
}
