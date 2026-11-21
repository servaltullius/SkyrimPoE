#include "PCH.h"
#include "runtime/CycloneRuntime.h"
#include "runtime/Addresses.h"
#include "runtime/AilmentRuntime.h"
#include "runtime/CombatHooks.h"
#include "runtime/FormIds.h"
#include "runtime/ProjectileRuntime.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"

#include <random>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr auto kOarActorValue = RE::ActorValue::kVariable09;
        constexpr auto kFrenzyPlugin = "BerserkerFrenzy.esp"sv;
        constexpr RE::FormID kFrenzyEffectLocalId = 0x800;
        constexpr auto kYieldCheckInterval = std::chrono::milliseconds(100);
        constexpr auto kInputTimeout = std::chrono::milliseconds(500);
        constexpr float kCycloneBaseRadius = 150.0f;
        constexpr float kNovaBaseRadius = 220.0f;

        using DealDamageFn = void*(RE::Actor* attacker, RE::Actor* target, RE::Projectile* projectile, bool leftHand);
        // DllMain 로더 락 안에서 Address Library를 열지 않도록 함수 내부 static으로 지연 해석한다.
        void CallDealDamage(RE::Actor* attacker, RE::Actor* target, RE::Projectile* projectile, bool leftHand)
        {
            static const REL::Relocation<DealDamageFn> fn{ REL::RelocationID(37673, 38627) };
            (void)fn(attacker, target, projectile, leftHand);
        }

        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::SkillSystem* g_skillSystem = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::Config* g_config = nullptr;
        bool g_installed = false;

        std::mutex g_mutex;
        Core::CycloneStateMachine g_machine;

        // 결정 54: 오른손/왼손 공격 버튼 상태를 분리 추적해 다른 입력(예: 우클릭 강타)을 오가로채지 않는다.
        enum class HeldHand : std::uint8_t
        {
            None = 0,
            Right = 1,
            Left = 2
        };
        bool g_rightHeld = false;
        bool g_leftHeld = false;
        bool g_autoTestHeld = false;
        HeldHand g_lastPressedHand = HeldHand::None;
        Clock::time_point g_lastRightPressedAt{};
        Clock::time_point g_lastLeftPressedAt{};

        // 경량 텔레메트리 카운터(자동 시험 및 진단용, relaxed 원자 증가)
        std::atomic<std::uint64_t> g_telAreaHitTicks{ 0 };
        std::atomic<std::uint64_t> g_telDealDamageHits{ 0 };
        std::atomic<std::uint64_t> g_telCritHits{ 0 };
        std::atomic<std::uint64_t> g_telCocTriggers{ 0 };
        std::atomic<std::uint64_t> g_telCocExecutions{ 0 };
        std::atomic<std::uint64_t> g_telCocTargetsHit{ 0 };
        std::atomic<std::uint64_t> g_telStaggersSwallowed{ 0 };
        std::atomic<std::uint64_t> g_telStaggersAborted{ 0 };

        // 프렌지 양보 캐시(결정 63): 주 갱신 스레드에서만 갱신, NotifyAnimationGraphHook은 lock-free atomic 읽기만 수행
        RE::EffectSetting* g_frenzyEffect = nullptr;
        std::atomic_bool g_yieldingToFrenzy{ false };
        Clock::time_point g_lastYieldCheck{};

        // 시각 정리 지연 플래그(비활성 상태일 때만 주 갱신 스레드에서 OAR 0 + NPC 회전 복원을 안전하게 처리)
        std::atomic_bool g_needVisualCleanup{ false };

        // NPC 스켈레톤 노드 회전 상태(프로토타입 2 검증 방식, 재시도 포함)
        bool g_spinActive = false;
        bool g_spinRestorePending = false;
        float g_spinAngle = 0.0f;
        RE::NiMatrix3 g_spinBase{};
        RE::NiMatrix3 g_spinApplied{};
        // C-13: 채널링 시작 시점 오른손 무기 FormID 기억(같은 종류 다른 무기로 교체 시 즉시 중단)
        RE::FormID g_activeRightWeaponId = 0;

        thread_local bool t_sendingGraphEvent = false;
        thread_local std::mt19937 t_rng{ std::random_device{}() };

        struct SendingScope
        {
            SendingScope() noexcept { t_sendingGraphEvent = true; }
            ~SendingScope() noexcept { t_sendingGraphEvent = false; }
        };

        void SetOarGate(RE::PlayerCharacter* player, float value)
        {
            if (!player) {
                return;
            }
            if (auto* av = player->AsActorValueOwner()) {
                av->SetActorValue(kOarActorValue, value);
            }
        }

        RE::NiAVObject* FindSpinNode(RE::PlayerCharacter* player)
        {
            static const RE::BSFixedString kNpcNode{ "NPC" };
            auto* root = player ? player->Get3D(false) : nullptr;
            return root ? root->GetObjectByName(kNpcNode) : nullptr;
        }

        RE::NiMatrix3 RotationAboutZ(float angle) noexcept
        {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            RE::NiMatrix3 m{};
            m.entry[0][0] = c;
            m.entry[0][1] = -s;
            m.entry[1][0] = s;
            m.entry[1][1] = c;
            m.entry[2][2] = 1.0f;
            return m;
        }

        RE::NiMatrix3 MultiplyMat3(const RE::NiMatrix3& a, const RE::NiMatrix3& b) noexcept
        {
            RE::NiMatrix3 out{};
            for (int r = 0; r < 3; ++r) {
                for (int col = 0; col < 3; ++col) {
                    out.entry[r][col] =
                        a.entry[r][0] * b.entry[0][col] +
                        a.entry[r][1] * b.entry[1][col] +
                        a.entry[r][2] * b.entry[2][col];
                }
            }
            return out;
        }

        float MaxMatrixDiff(const RE::NiMatrix3& a, const RE::NiMatrix3& b) noexcept
        {
            float worst = 0.0f;
            for (int r = 0; r < 3; ++r) {
                for (int col = 0; col < 3; ++col) {
                    worst = std::max(worst, std::abs(a.entry[r][col] - b.entry[r][col]));
                }
            }
            return worst;
        }

        // 프로토타입 2 검증: 1인칭 전환·3D 재로드 직후 FindSpinNode가 null이면 g_spinRestorePending을 남겨 다음 틱에 재시도한다.
        void TryRestoreSpinNode(RE::PlayerCharacter* player)
        {
            if (!g_spinActive && !g_spinRestorePending) {
                return;
            }
            g_spinActive = false;
            g_spinAngle = 0.0f;
            auto* node = FindSpinNode(player);
            if (!node) {
                g_spinRestorePending = true;
                return;
            }
            if (MaxMatrixDiff(node->local.rotate, g_spinApplied) <= 1e-3f) {
                node->local.rotate = g_spinBase;
            }
            g_spinRestorePending = false;
        }

        [[nodiscard]] bool IsPlayerAllowedForCyclone(RE::PlayerCharacter* player)
        {
            if (!player || !player->Is3DLoaded() || player->IsDead()) {
                return false;
            }
            // 결정 99: 막기 중 금지
            if (player->IsBlocking()) {
                return false;
            }
            const auto* state = player->AsActorState();
            if (!state) {
                return false;
            }
            if (!state->IsWeaponDrawn() || state->IsSwimming() || state->IsBleedingOut() || state->IsStaggered()) {
                return false;
            }
            if (state->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
                return false;
            }
            if (player->IsOnMount() || player->IsInKillMove() || player->IsInRagdollState()) {
                return false;
            }
            const auto* controls = RE::ControlMap::GetSingleton();
            if (!controls || !controls->IsFightingControlsEnabled() || !controls->IsMovementControlsEnabled()) {
                return false;
            }
            const auto* form = player->GetEquippedObject(false);
            const auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr;
            if (!weapon || !weapon->IsMelee() || weapon->IsHandToHandMelee()) {
                return false;
            }
            return true;
        }

        // 주 갱신 스레드 전용: 100ms 주기로 BerserkerFrenzy 효과 유무를 갱신해 atomic으로 게시한다.
        void UpdateYieldToFrenzyOnMainThread(RE::PlayerCharacter* player, Clock::time_point now)
        {
            if (!g_config || !g_config->yieldToFrenzy || !g_frenzyEffect || !player) {
                g_yieldingToFrenzy.store(false, std::memory_order_release);
                return;
            }
            if (now - g_lastYieldCheck < kYieldCheckInterval) {
                return;
            }
            g_lastYieldCheck = now;
            auto* mt = player->AsMagicTarget();
            g_yieldingToFrenzy.store(mt && mt->HasMagicEffect(g_frenzyEffect), std::memory_order_release);
        }

        [[nodiscard]] bool PowerInputAcceptsHandLocked(Clock::time_point now) noexcept
        {
            if (g_autoTestHeld) {
                return true;
            }
            if (!g_config) {
                return false;
            }
            const bool rightFresh = g_rightHeld && (now - g_lastRightPressedAt <= kInputTimeout);
            const bool leftFresh = g_leftHeld && (now - g_lastLeftPressedAt <= kInputTimeout);

            if (g_config->powerAttackInput == "both") {
                return rightFresh || leftFresh;
            }
            if (g_config->powerAttackInput == "leftHand") {
                return leftFresh && !rightFresh && g_lastPressedHand == HeldHand::Left;
            }
            // 기본 "rightHand": 왼손과 동시 강공격(쌍수 동시 강타, C-13)은 가로채지 않고 오른손 단독 길게 누르기만 허용
            return rightFresh && !leftFresh && g_lastPressedHand == HeldHand::Right;
        }

        [[nodiscard]] bool IsChannelButtonHeldLocked(Clock::time_point now) noexcept
        {
            if (g_autoTestHeld) {
                return true;
            }
            if (g_rightHeld && (now - g_lastRightPressedAt > kInputTimeout)) {
                g_rightHeld = false;
            }
            if (g_leftHeld && (now - g_lastLeftPressedAt > kInputTimeout)) {
                g_leftHeld = false;
            }
            if (!g_config) {
                return false;
            }
            if (g_config->powerAttackInput == "both") {
                return g_rightHeld || g_leftHeld;
            }
            if (g_config->powerAttackInput == "leftHand") {
                return g_leftHeld;
            }
            return g_rightHeld;
        }

        [[nodiscard]] std::optional<std::uint8_t> FindEquippedCycloneSlot(const PersistentState& state) noexcept
        {
            for (std::size_t i = 0; i < state.slots.size(); ++i) {
                if (state.slots[i].occupied && state.slots[i].gem.gemId == "Cyclone") {
                    return static_cast<std::uint8_t>(i);
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] double ComputeCycleResourceCost(
            RE::PlayerCharacter* player,
            const Core::StatSnapshot& snap,
            std::uint8_t cycloneSlotIdx)
        {
            if (!g_dataPack || !g_config || cycloneSlotIdx >= Core::kSkillSlotCount) {
                return 0.0;
            }
            const auto& stats = snap.slotStats[cycloneSlotIdx];
            if (snap.bloodMagic) {
                auto* avOwner = player ? player->AsActorValueOwner() : nullptr;
                const double maxHealth = avOwner
                    ? std::max(1.0, static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kHealth)))
                    : 100.0;
                return Core::ComputeBloodMagicLifeCost(maxHealth, snap.poeMaxLife, stats.poeManaCost);
            }
            const double baseMaxMagicka = CombatHooks::GetBaseSkyrimMaxMagicka(player);
            const auto scaled = Core::ComputeMagickaScaling(
                baseMaxMagicka, snap.poeMaxMana, stats.poeManaCost, snap.exileLevel, *g_dataPack, *g_config);
            return scaled.skyrimSkillCost;
        }

        [[nodiscard]] double ReadAvailableCycleResource(
            RE::PlayerCharacter* player,
            const Core::StatSnapshot& snap) noexcept
        {
            auto* avOwner = player ? player->AsActorValueOwner() : nullptr;
            if (!avOwner) {
                return 0.0;
            }
            if (snap.bloodMagic) {
                // 3단계 Blood Magic 자살 방지 가드: curHealth - cost >= 1.0 보장
                const double curHealth = static_cast<double>(avOwner->GetActorValue(RE::ActorValue::kHealth));
                return std::max(0.0, curHealth - 1.0);
            }
            return std::max(0.0, static_cast<double>(avOwner->GetActorValue(RE::ActorValue::kMagicka)));
        }

        [[nodiscard]] float GetSkyrimCritWithPerks(RE::PlayerCharacter* player, RE::Actor* target)
        {
            if (!player || !target) {
                return 0.0f;
            }
            auto* avOwner = player->AsActorValueOwner();
            if (!avOwner) {
                return 0.0f;
            }
            float critPct = avOwner->GetActorValue(RE::ActorValue::kCriticalChance);
            auto* form = player->GetEquippedObject(false);
            if (auto* weapon = form ? form->As<RE::TESObjectWEAP>() : nullptr) {
                RE::BGSEntryPoint::HandleEntryPoint(
                    RE::BGSEntryPoint::ENTRY_POINT::kCalculateMyCriticalHitChance,
                    player, weapon, target, &critPct);
            }
            return critPct;
        }

        [[nodiscard]] double RollPoePacketDamage(const Core::DamagePacket& pkt)
        {
            double total = 0.0;
            for (const auto& r : pkt.byType) {
                if (r.max <= 0.0 && r.min <= 0.0) {
                    continue;
                }
                const double lo = std::max(0.0, std::min(r.min, r.max));
                const double hi = std::max(lo, std::max(r.min, r.max));
                if (hi - lo <= 1e-6) {
                    total += lo;
                } else {
                    std::uniform_real_distribution<double> dist(lo, hi);
                    total += dist(t_rng);
                }
            }
            return total;
        }

        [[nodiscard]] RE::FormID VfxLocalIdForGem(std::string_view gemId) noexcept
        {
            if (gemId == "IceNova") return FormIds::kVfxIceNova;
            if (gemId == "ShockNova") return FormIds::kVfxShockNova;
            return 0;
        }

        void ExecuteCycloneAreaHit(
            RE::PlayerCharacter* player,
            std::uint8_t cycloneSlotIdx,
            const PersistentState& state,
            const Core::StatSnapshot& snap)
        {
            if (!player || !g_dataPack || !g_config || cycloneSlotIdx >= Core::kSkillSlotCount) {
                return;
            }
            const auto& cycStats = snap.slotStats[cycloneSlotIdx];
            const float radius = static_cast<float>(kCycloneBaseRadius * std::max(0.25, cycStats.areaMult));
            const auto targets = SkillExecution::CollectHostileTargets(player, player->GetPosition(), radius);
            if (targets.empty()) {
                return;
            }

            std::uniform_real_distribution<double> unitDist(0.0, 1.0);
            auto* playerAv = player->AsActorValueOwner();
            const double baseMaxMagicka = CombatHooks::GetBaseSkyrimMaxMagicka(player);

            for (const auto& target : targets) {
                if (!target || target->IsDead()) {
                    continue;
                }
                auto* targetAv = target->AsActorValueOwner();
                if (!targetAv) {
                    continue;
                }

                // 결정 51 + 3단계 Resolute Technique: RT 할당 시 스카이림 치명타 퍽 포함 모든 치명타를 차단
                bool isCrit = false;
                if (!snap.resoluteTechnique) {
                    const double skyrimCritPct = static_cast<double>(GetSkyrimCritWithPerks(player, target.get()));
                    const double totalCritPct = std::clamp(cycStats.EffectiveCritChancePct() + skyrimCritPct, 0.0, 100.0);
                    isCrit = (unitDist(t_rng) * 100.0) < totalCritPct;
                }

                double poeHit = RollPoePacketDamage(cycStats.baseHitDamage);
                if (isCrit) {
                    poeHit *= std::max(1.0, cycStats.critMultiplierPct / 100.0);
                }
                const double convertedSkyrimDmg = Core::ConvertOffenseHit(poeHit, snap.exileLevel, *g_dataPack, *g_config);

                const float healthBefore = targetAv->GetActorValue(RE::ActorValue::kHealth);
                bool absorbedByMonsterEs = false;
                {
                    const ScopedCycloneHitContext hitCtx(convertedSkyrimDmg, isCrit, target->GetFormID(), &absorbedByMonsterEs);
                    CallDealDamage(player, target.get(), nullptr, false);
                }
                const float healthAfter = targetAv->GetActorValue(RE::ActorValue::kHealth);
                const bool dealtDamage = target->IsDead() || (healthAfter < healthBefore - 1e-4f) || absorbedByMonsterEs;
                if (dealtDamage) {
                    g_telDealDamageHits.fetch_add(1, std::memory_order_relaxed);
                    if (isCrit) {
                        g_telCritHits.fetch_add(1, std::memory_order_relaxed);
                    }
                    // 5단계(결정 163): 사이클론 타격 시 원소·출혈·중독 상태이상 적용
                    (void)AilmentRuntime::ApplyPlayerHitAilments(
                        target.get(), cycStats.baseHitDamage, isCrit, AilmentRuntime::HitKind::Attack,
                        cycStats.igniteChancePct, snap.starforgeShock);
                }

                // M7 치명타 시 시전(CoC): 실제 체력 감소 > 0이고 치명타였으면(치명타 처치 포함, 결정 129) 즉시 발동 판정
                if (isCrit && dealtDamage && g_skillSystem && g_queue && playerAv) {
                    const double curMagicka = static_cast<double>(playerAv->GetActorValue(RE::ActorValue::kMagicka));
                    const double curHealth = static_cast<double>(playerAv->GetActorValue(RE::ActorValue::kHealth));
                    const double maxHealth = std::max(1.0, static_cast<double>(playerAv->GetPermanentActorValue(RE::ActorValue::kHealth)));
                    auto cast = g_skillSystem->TryTriggerOnCrit(
                        cycloneSlotIdx, snap, state.slots, curMagicka, baseMaxMagicka, *g_dataPack, *g_config,
                        curHealth, maxHealth);
                    if (cast) {
                        g_telCocTriggers.fetch_add(1, std::memory_order_relaxed);
                        if (cast->usesHealthCost && cast->skyrimHealthCost > 0.0) {
                            const double safeDrain = std::min(cast->skyrimHealthCost, std::max(0.0, curHealth - 1.0));
                            if (safeDrain > 0.0) {
                                playerAv->ModActorValue(
                                    RE::ACTOR_VALUE_MODIFIER::kDamage,
                                    RE::ActorValue::kHealth,
                                    -static_cast<float>(safeDrain));
                            }
                        } else if (cast->skyrimMagickaCost > 0.0) {
                            playerAv->ModActorValue(
                                RE::ACTOR_VALUE_MODIFIER::kDamage,
                                RE::ActorValue::kMagicka,
                                -static_cast<float>(cast->skyrimMagickaCost));
                        }

                        const bool isNovaSpell = (cast->gemId == "IceNova" || cast->gemId == "ShockNova");
                        if (!isNovaSpell) {
                            // 5단계(결정 163): CoC 발동 투사체·연쇄 주문(Fireball/FreezingPulse/Arc/Spark)은
                            // 플레이어→대상 방향으로 ProjectileRuntime으로 발사한다.
                            auto castCopy = *cast;
                            const auto targetPos = target->GetPosition();
                            g_queue->Enqueue([castCopy = std::move(castCopy), targetPos]() {
                                auto* pl = RE::PlayerCharacter::GetSingleton();
                                if (!pl || pl->IsDead()) return;
                                RE::NiPoint3 aimOrigin{};
                                RE::NiPoint3 aimDir{};
                                ProjectileRuntime::ComputePlayerAim(pl, aimOrigin, aimDir);
                                const RE::NiPoint3 toTarget{
                                    targetPos.x - aimOrigin.x,
                                    targetPos.y - aimOrigin.y,
                                    (targetPos.z + 75.0f) - aimOrigin.z
                                };
                                const float len = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y + toTarget.z * toTarget.z);
                                if (len > 1e-3f) {
                                    aimDir = RE::NiPoint3{ toTarget.x / len, toTarget.y / len, toTarget.z / len };
                                }
                                if (g_skillSystem) g_skillSystem->EnterTriggerDepth();
                                try {
                                    if (ProjectileRuntime::LaunchSpellDelivery(castCopy, aimOrigin, aimDir)) {
                                        g_telCocExecutions.fetch_add(1, std::memory_order_relaxed);
                                        g_telCocTargetsHit.fetch_add(1, std::memory_order_relaxed);
                                    }
                                } catch (...) {
                                }
                                if (g_skillSystem) g_skillSystem->ExitTriggerDepth();
                            });
                        } else {
                            // 발동 노바 주문(IceNova/ShockNova): 발동 시점에 피해 롤·치명타를 고정하고 대상 좌표에서 폭발+상태이상 적용
                            bool spellCrit = false;
                            const double spellDmg = Core::SkillSystem::RollTriggeredSpellSkyrimDamage(
                                *cast, *g_dataPack, *g_config, unitDist(t_rng), unitDist(t_rng), spellCrit);
                            const auto& pkt = cast->poeBaseHitDamage.byType;
                            const bool hasElemDamage =
                                pkt[static_cast<std::size_t>(Core::DamageType::Fire)].max > 0.0 ||
                                pkt[static_cast<std::size_t>(Core::DamageType::Cold)].max > 0.0 ||
                                pkt[static_cast<std::size_t>(Core::DamageType::Lightning)].max > 0.0;
                            const bool eoEligibleCrit = spellCrit && hasElemDamage;

                            const RE::NiPoint3 origin = target->GetPosition();
                            const float spellRadius = static_cast<float>(kNovaBaseRadius * std::max(0.25, cast->areaMult));
                            const RE::FormID vfxId = VfxLocalIdForGem(cast->gemId);
                            auto castCopy = *cast;

                            g_queue->Enqueue([origin, spellRadius, spellDmg, vfxId, eoEligibleCrit, spellCrit, castCopy = std::move(castCopy)]() {
                                if (g_skillSystem) {
                                    g_skillSystem->EnterTriggerDepth();
                                }
                                try {
                                    const auto domType = (castCopy.gemId == "ShockNova")
                                        ? Core::DamageType::Lightning
                                        : Core::DamageType::Cold;
                                    const auto hits = SkillExecution::ExecuteAoESpell(origin, spellRadius, spellDmg, vfxId, domType);
                                    if (auto* pl = RE::PlayerCharacter::GetSingleton(); pl && hits > 0) {
                                        const auto targets = SkillExecution::CollectHostileTargets(pl, origin, spellRadius);
                                        for (const auto& t : targets) {
                                            if (t && !t->IsDead()) {
                                                (void)AilmentRuntime::ApplyPlayerHitAilments(
                                                    t.get(), castCopy.poeBaseHitDamage, spellCrit,
                                                    AilmentRuntime::HitKind::Spell,
                                                    castCopy.igniteChancePct, castCopy.starforgeShock);
                                            }
                                        }
                                    }
                                    g_telCocExecutions.fetch_add(1, std::memory_order_relaxed);
                                    g_telCocTargetsHit.fetch_add(hits, std::memory_order_relaxed);
                                    // 검수 M3: Elemental Overload는 원소 피해 주문이 실제로 1명 이상 적중했을 때만 발동
                                    if (eoEligibleCrit && hits > 0) {
                                        CombatHooks::NotifyPlayerElementalCrit();
                                    }
                                } catch (...) {
                                }
                                if (g_skillSystem) {
                                    g_skillSystem->ExitTriggerDepth();
                                }
                            });
                        }
                    }
                }
            }
        }

        // ---- 훅 & 이벤트 싱크 ----
        struct NotifyAnimationGraphHook
        {
            static bool Thunk(RE::IAnimationGraphManagerHolder* a_this, const RE::BSFixedString& a_event)
            {
                const auto callOrig = [&]() -> bool {
                    const auto orig = originalAddr.load(std::memory_order_acquire);
                    if (orig == 0) {
                        return false;
                    }
                    using Fn = bool (*)(RE::IAnimationGraphManagerHolder*, const RE::BSFixedString&);
                    return reinterpret_cast<Fn>(orig)(a_this, a_event);
                };

                if (t_sendingGraphEvent || !g_config || !g_statEngine || !g_dataPack || !g_dataPack->IsLoaded()) {
                    return callOrig();
                }

                const std::string_view evName = a_event.empty() ? std::string_view{} : std::string_view(a_event.c_str());
                if (evName.empty()) {
                    return callOrig();
                }

                // 결정 63: 프렌지 양보 중에는 경직·공격 이벤트를 일절 가로채지 않고 통과시킨다(lock-free atomic 읽기).
                const bool yielding = g_yieldingToFrenzy.load(std::memory_order_acquire);
                if (yielding) {
                    return callOrig();
                }

                // 1. 경직 시작 이벤트 처리(결정 56, 150 ③ + 3단계 Unwavering Stance)
                // (결정 63 프렌지 양보 검사 뒤에 위치하므로 프렌지 양보 우선권 유지)
                if (Core::CycloneStateMachine::IsStaggerStartEvent(evName)) {
                    const auto snap = g_statEngine->GetSnapshot();
                    if (snap && snap->unwaveringStance) {
                        g_telStaggersSwallowed.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    float mag = 1.0f;
                    const bool known = player && player->GetGraphVariableFloat("staggerMagnitude", mag);
                    Core::CycloneStaggerDecision dec;
                    {
                        std::scoped_lock lock(g_mutex);
                        dec = g_machine.OnStaggerStart(known ? static_cast<double>(mag) : 1.0, g_config->channelIgnoreStagger);
                    }
                    if (dec.swallowEvent) {
                        g_telStaggersSwallowed.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                    if (dec.abortedChannel) {
                        g_telStaggersAborted.fetch_add(1, std::memory_order_relaxed);
                        g_needVisualCleanup.store(true, std::memory_order_release);
                    }
                    return callOrig();
                }

                // 2. 채널링 중에는 엔진이 계속 보내는 일반/강공격 시작 이벤트를 모두 삼켜 바닐라 공격 끼어듦을 차단한다.
                if (Core::CycloneStateMachine::IsAttackStartEvent(evName)) {
                    bool alreadyActive = false;
                    {
                        std::scoped_lock lock(g_mutex);
                        alreadyActive = g_machine.IsActive();
                    }
                    if (alreadyActive) {
                        return false;
                    }
                }

                // 3. 비활성 상태에서 강공격 시작 이벤트만 선택적으로 가로챈다(결정 54, 150 ①).
                if (Core::CycloneStateMachine::IsPowerAttackStartEvent(evName)) {
                    auto* player = RE::PlayerCharacter::GetSingleton();
                    const auto state = SaveLoad::GetStateCopy();
                    const auto cycSlot = FindEquippedCycloneSlot(state);
                    const auto snap = g_statEngine->GetSnapshot();

                    bool matchedInput = false;
                    {
                        std::scoped_lock lock(g_mutex);
                        matchedInput = PowerInputAcceptsHandLocked(Clock::now());
                    }

                    if (player && cycSlot && snap && matchedInput) {
                        const bool gemReqOk = !(g_config && g_config->enforceGemRequirements) ||
                            snap->slotStats[*cycSlot].requirementMet;
                        const bool allowed = gemReqOk && IsPlayerAllowedForCyclone(player);
                        const double curResource = ReadAvailableCycleResource(player, *snap);
                        const double firstCost = ComputeCycleResourceCost(player, *snap, *cycSlot);

                        bool accepted = false;
                        {
                            std::scoped_lock lock(g_mutex);
                            accepted = g_machine.TryBeginFromPowerAttack(true, false, allowed, curResource, firstCost);
                        }
                        if (accepted) {
                            return false;  // 강공격 모션 대신 첫 주 갱신에서 사이클론 시작(결정 128)
                        }
                    }
                }

                return callOrig();
            }

            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        class AttackButtonSink final : public RE::BSTEventSink<RE::InputEvent*>
        {
        public:
            static AttackButtonSink& Get()
            {
                static AttackButtonSink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(
                RE::InputEvent* const* a_events,
                RE::BSTEventSource<RE::InputEvent*>*) override
            {
                if (!a_events || !g_config) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* ui = RE::UI::GetSingleton();
                const bool paused = ui && ui->GameIsPaused();
                auto* controls = RE::ControlMap::GetSingleton();
                auto* userEvents = RE::UserEvents::GetSingleton();

                for (auto* ev = *a_events; ev; ev = ev->next) {
                    const auto* btn = ev->AsButtonEvent();
                    if (!btn) {
                        continue;
                    }
                    const auto dev = btn->GetDevice();
                    const auto code = btn->GetIDCode();

                    std::uint32_t rightKey = (dev == RE::INPUT_DEVICE::kGamepad) ? 0x0Au : 0u;
                    std::uint32_t leftKey = (dev == RE::INPUT_DEVICE::kGamepad) ? 0x09u : 1u;
                    if (controls && userEvents) {
                        const auto mappedR = controls->GetMappedKey(userEvents->rightAttack, dev);
                        const auto mappedL = controls->GetMappedKey(userEvents->leftAttack, dev);
                        if (mappedR != RE::ControlMap::kInvalid) rightKey = mappedR;
                        if (mappedL != RE::ControlMap::kInvalid) leftKey = mappedL;
                    }

                    const bool isRight = (code == rightKey);
                    const bool isLeft = (code == leftKey);
                    if (!isRight && !isLeft) {
                        continue;
                    }

                    // CommonLibSSE-NG ButtonEvent: 1프레임 짧은 뗌에서 IsUp()은 HeldDuration==0이라 false이므로
                    // 프로토타입 2와 동일하게 !IsPressed()로 뗌을 판정한다.
                    const bool pressed = btn->IsPressed();
                    const auto now = Clock::now();

                    std::scoped_lock lock(g_mutex);
                    if (pressed && !paused) {
                        if (isRight) {
                            g_rightHeld = true;
                            g_lastRightPressedAt = now;
                            if (btn->IsDown()) {
                                g_lastPressedHand = HeldHand::Right;
                            }
                        }
                        if (isLeft) {
                            g_leftHeld = true;
                            g_lastLeftPressedAt = now;
                            if (btn->IsDown()) {
                                g_lastPressedHand = HeldHand::Left;
                            }
                        }
                    } else if (!pressed) {
                        if (isRight) {
                            g_rightHeld = false;
                        }
                        if (isLeft) {
                            g_leftHeld = false;
                        }
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            AttackButtonSink() = default;
        };

        class MenuPauseSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            static MenuPauseSink& Get()
            {
                static MenuPauseSink instance;
                return instance;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!event || !event->opening) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto* ui = RE::UI::GetSingleton();
                if (!ui || !ui->GameIsPaused()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                // 결정 150 ④: 일시정지 메뉴 진입 시 즉시 채널링 종료 및 주 갱신 시각 정리 플래그 설정
                bool wasActive = false;
                {
                    std::scoped_lock lock(g_mutex);
                    wasActive = g_machine.ForceStop();
                    g_rightHeld = false;
                    g_leftHeld = false;
                    g_autoTestHeld = false;
                }
                if (wasActive) {
                    g_needVisualCleanup.store(true, std::memory_order_release);
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            MenuPauseSink() = default;
        };
    }

    bool CycloneRuntime::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        Core::SkillSystem* skillSystem,
        const Core::DataPack* dataPack,
        const Core::Config* config)
    {
        if (g_installed) {
            return true;
        }
        g_queue = queue;
        g_statEngine = statEngine;
        g_skillSystem = skillSystem;
        g_dataPack = dataPack;
        g_config = config;

        if (g_config && g_config->yieldToFrenzy) {
            if (auto* handler = RE::TESDataHandler::GetSingleton()) {
                g_frenzyEffect = handler->LookupForm<RE::EffectSetting>(kFrenzyEffectLocalId, kFrenzyPlugin);
            }
        }

        // PlayerCharacter IAnimationGraphManagerHolder::NotifyAnimationGraph (VTABLE[3], slot 0x1)
        REL::Relocation<std::uintptr_t> graphVtable{ RE::VTABLE_PlayerCharacter[3] };
        const auto slotAddr = graphVtable.address() + Addresses::kNotifyAnimationGraphVtableSlot * sizeof(std::uintptr_t);
        const auto current = *reinterpret_cast<const std::uintptr_t*>(slotAddr);
        NotifyAnimationGraphHook::originalAddr.store(current, std::memory_order_release);
        const auto prev = graphVtable.write_vfunc(Addresses::kNotifyAnimationGraphVtableSlot, NotifyAnimationGraphHook::Thunk);
        NotifyAnimationGraphHook::originalAddr.store(prev, std::memory_order_release);

        if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
            input->AddEventSink(&AttackButtonSink::Get());
        }
        if (auto* ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&MenuPauseSink::Get());
        }

        SaveLoad::AddRevertCallback([]() {
            {
                std::scoped_lock lock(g_mutex);
                g_machine.ForceStop();
                g_rightHeld = false;
                g_leftHeld = false;
                g_autoTestHeld = false;
                g_lastPressedHand = HeldHand::None;
            }
            if (g_skillSystem) {
                g_skillSystem->ResetOnRevert();
            }
            g_yieldingToFrenzy.store(false, std::memory_order_release);
            g_needVisualCleanup.store(true, std::memory_order_release);
            g_spinRestorePending = g_spinActive;
            g_spinActive = false;
            g_spinAngle = 0.0f;
            g_activeRightWeaponId = 0;
        });

        g_installed = true;
        SKSE::log::info("CycloneRuntime: NotifyAnimationGraph hook, input/menu sinks installed (yieldToFrenzy={}).",
            g_frenzyEffect != nullptr);
        return true;
    }

    void CycloneRuntime::Tick(double deltaSec, bool gamePaused)
    {
        if (g_skillSystem) {
            g_skillSystem->TickCooldowns(deltaSec, gamePaused);
        }

        if (!g_statEngine || !g_dataPack || !g_config || !g_dataPack->IsLoaded()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        const auto now = Clock::now();
        // 결정 63: 비활성 시에도 주 갱신 스레드에서 100ms 주기로 프렌지 상태를 갱신한다.
        UpdateYieldToFrenzyOnMainThread(player, now);

        bool isActive = false;
        bool held = false;
        {
            std::scoped_lock lock(g_mutex);
            isActive = g_machine.IsActive();
            held = IsChannelButtonHeldLocked(now);
        }

        if (!isActive) {
            // 비활성 상태일 때만 지연된 OAR 0 복원 및 NPC 회전 복원을 안전하게 처리(새 채널링 덮어쓰기 레이스 방지)
            if (g_needVisualCleanup.exchange(false, std::memory_order_acq_rel)) {
                SetOarGate(player, 0.0f);
                TryRestoreSpinNode(player);
            } else if (g_spinRestorePending) {
                TryRestoreSpinNode(player);
            }
            return;
        }

        const auto state = SaveLoad::GetStateCopy();
        const auto cycSlot = FindEquippedCycloneSlot(state);
        const auto snap = g_statEngine->GetSnapshot();
        if (!cycSlot || !snap) {
            StopNowOnMainThread(player);
            return;
        }

        const bool yielding = g_yieldingToFrenzy.load(std::memory_order_acquire);
        const bool gemReqOk = !(g_config && g_config->enforceGemRequirements) ||
            snap->slotStats[*cycSlot].requirementMet;

        double weaponSpeed = 1.0;
        RE::FormID currentRightWeaponId = 0;
        if (auto* form = player->GetEquippedObject(false)) {
            if (auto* weap = form->As<RE::TESObjectWEAP>()) {
                currentRightWeaponId = weap->GetFormID();
                const float spd = weap->GetSpeed();
                if (std::isfinite(spd) && spd > 0.0f) {
                    weaponSpeed = static_cast<double>(spd);
                }
            }
        }
        // C-13: 채널링 도중 오른손 무기가 다른 FormID로 바뀌면 즉시 종료
        const bool weaponSwitched = (g_activeRightWeaponId != 0 && currentRightWeaponId != g_activeRightWeaponId);
        const bool allowed = !yielding && !weaponSwitched && gemReqOk && IsPlayerAllowedForCyclone(player);

        auto* avOwner = player->AsActorValueOwner();
        const double curResource = ReadAvailableCycleResource(player, *snap);
        const double cycleCost = ComputeCycleResourceCost(player, *snap, *cycSlot);

        // C-13: 마무리 시간은 0.15초 고정이 아니라 현재 회전 주기의 남은 바퀴(최대 약 1/3초)로 산출
        const double effectiveRate = std::max(0.25, weaponSpeed * snap->slotStats[*cycSlot].attackSpeedMult);
        const double cycleSec = 1.0 / effectiveRate;
        const double computedOutroSec = std::clamp(cycleSec * 0.5, 0.06, 0.33);

        Core::CycloneTickInput in;
        in.deltaSec = deltaSec;
        in.gamePaused = gamePaused;
        in.buttonHeld = held;
        in.playerAllowed = allowed;
        in.weaponSpeed = weaponSpeed;
        in.poeAttackSpeedMult = snap->slotStats[*cycSlot].attackSpeedMult;
        in.currentMagicka = curResource;
        in.cycleMagickaCost = cycleCost;
        in.minChannelSec = g_config->minChannelSeconds;
        in.outroSec = computedOutroSec;

        Core::CycloneTickOutput out;
        {
            std::scoped_lock lock(g_mutex);
            out = g_machine.Tick(in);
        }

        if (out.startOar) {
            g_activeRightWeaponId = currentRightWeaponId;
            g_needVisualCleanup.store(false, std::memory_order_release);
            SetOarGate(player, 1.0f);
            // 프로토타입 2 검증: 길게 누르기 직전 먼저 시작된 일반 공격을 즉시 끊어 OAR 이동(사이클론) 자세로 전환한다.
            {
                const SendingScope scope;
                (void)player->NotifyAnimationGraph("attackStop");
            }
            if (auto* node = FindSpinNode(player)) {
                g_spinBase = node->local.rotate;
                g_spinApplied = g_spinBase;
                g_spinAngle = 0.0f;
                g_spinActive = true;
                g_spinRestorePending = false;
            }
        }

        if (out.consumeMagicka && out.magickaToConsume > 0.0 && avOwner) {
            if (snap->bloodMagic) {
                const double curHp = static_cast<double>(avOwner->GetActorValue(RE::ActorValue::kHealth));
                const double safeDrain = std::min(out.magickaToConsume, std::max(0.0, curHp - 1.0));
                if (safeDrain > 0.0) {
                    avOwner->ModActorValue(
                        RE::ACTOR_VALUE_MODIFIER::kDamage,
                        RE::ActorValue::kHealth,
                        -static_cast<float>(safeDrain));
                }
            } else {
                avOwner->ModActorValue(
                    RE::ACTOR_VALUE_MODIFIER::kDamage,
                    RE::ActorValue::kMagicka,
                    -static_cast<float>(out.magickaToConsume));
            }
        }

        if (out.spinRadiansDelta > 0.0 && g_spinActive) {
            if (auto* node = FindSpinNode(player)) {
                g_spinAngle = std::fmod(g_spinAngle + static_cast<float>(out.spinRadiansDelta),
                    static_cast<float>(Core::CycloneStateMachine::kTwoPi));
                // 프로토타입 2 검증 곱셈 순서: RotationAboutZ * base (월드 수직축 기준 회전)
                const auto rotated = MultiplyMat3(RotationAboutZ(g_spinAngle), g_spinBase);
                node->local.rotate = rotated;
                g_spinApplied = rotated;
            }
        }

        if (out.performAreaHit) {
            g_telAreaHitTicks.fetch_add(1, std::memory_order_relaxed);
            ExecuteCycloneAreaHit(player, *cycSlot, state, *snap);
        }

        if (out.stopOar || out.ended) {
            g_activeRightWeaponId = 0;
            SetOarGate(player, 0.0f);
            TryRestoreSpinNode(player);
            if (out.stopOar) {
                const SendingScope scope;
                (void)player->NotifyAnimationGraph("attackStop");
            }
        }
    }

    void CycloneRuntime::StopNowOnMainThread(RE::PlayerCharacter* player)
    {
        bool wasActive = false;
        {
            std::scoped_lock lock(g_mutex);
            wasActive = g_machine.ForceStop();
            g_rightHeld = false;
            g_leftHeld = false;
            g_autoTestHeld = false;
        }
        g_activeRightWeaponId = 0;
        if (wasActive && player) {
            SetOarGate(player, 0.0f);
            TryRestoreSpinNode(player);
            const SendingScope scope;
            (void)player->NotifyAnimationGraph("attackStop");
        }
    }

    bool CycloneRuntime::IsActive() noexcept
    {
        std::scoped_lock lock(g_mutex);
        return g_machine.IsActive();
    }

    bool CycloneRuntime::IsYieldingToFrenzy() noexcept
    {
        return g_yieldingToFrenzy.load(std::memory_order_acquire);
    }

    void CycloneRuntime::ForceRefreshYieldCheckOnMainThread(RE::PlayerCharacter* player)
    {
        if (!g_config || !g_config->yieldToFrenzy || !g_frenzyEffect || !player) {
            g_yieldingToFrenzy.store(false, std::memory_order_release);
            return;
        }
        g_lastYieldCheck = Clock::now();
        auto* mt = player->AsMagicTarget();
        g_yieldingToFrenzy.store(mt && mt->HasMagicEffect(g_frenzyEffect), std::memory_order_release);
    }

    CycloneTelemetry CycloneRuntime::GetTelemetry() noexcept
    {
        CycloneTelemetry t;
        t.areaHitTicks = g_telAreaHitTicks.load(std::memory_order_relaxed);
        t.dealDamageHits = g_telDealDamageHits.load(std::memory_order_relaxed);
        t.critHits = g_telCritHits.load(std::memory_order_relaxed);
        t.cocTriggers = g_telCocTriggers.load(std::memory_order_relaxed);
        t.cocExecutions = g_telCocExecutions.load(std::memory_order_relaxed);
        t.cocTargetsHit = g_telCocTargetsHit.load(std::memory_order_relaxed);
        t.staggersSwallowed = g_telStaggersSwallowed.load(std::memory_order_relaxed);
        t.staggersAborted = g_telStaggersAborted.load(std::memory_order_relaxed);
        return t;
    }

    bool CycloneRuntime::TryStartForAutoTest(RE::PlayerCharacter* player)
    {
        if (!player) {
            return false;
        }
        {
            std::scoped_lock lock(g_mutex);
            g_autoTestHeld = true;
            g_rightHeld = true;
            g_lastRightPressedAt = Clock::now();
            g_lastPressedHand = HeldHand::Right;
        }
        (void)player->NotifyAnimationGraph("attackPowerStartInPlace");
        const bool active = IsActive();
        if (!active) {
            std::scoped_lock lock(g_mutex);
            g_autoTestHeld = false;
            g_rightHeld = false;
        }
        return active;
    }

    void CycloneRuntime::SetAutoTestHeld(bool held)
    {
        std::scoped_lock lock(g_mutex);
        g_autoTestHeld = held;
        if (held) {
            g_rightHeld = true;
            g_lastRightPressedAt = Clock::now();
            g_lastPressedHand = HeldHand::Right;
        } else {
            g_rightHeld = false;
            g_leftHeld = false;
        }
    }
}
