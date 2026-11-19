#include "PCH.h"
#include "runtime/CombatHooks.h"
#include "runtime/Addresses.h"
#include "runtime/SaveLoad.h"
#include "runtime/SkillExecution.h"

#include <random>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        Core::CommandQueue* g_queue = nullptr;
        Core::StatEngine* g_statEngine = nullptr;
        Core::EnergyShieldPool* g_esPool = nullptr;
        Core::KillTracker* g_killTracker = nullptr;
        const Core::DataPack* g_dataPack = nullptr;
        const Core::Config* g_config = nullptr;
        bool g_installed = false;

        thread_local CycloneHitContext t_cycloneCtx{};
        thread_local std::mt19937 t_rng{ std::random_device{}() };

        // 폭발 비율(44004+0xC1 -> 43849+0x412는 동일 스레드 동기 호출이므로 thread_local로 관리해 스택 재사용 오염을 방지)
        thread_local const RE::HitData* t_lastExplHit = nullptr;
        thread_local float t_lastExplRatio = 1.0f;

        // 마법 효과별 스카이림 저항 배율 및 꼬리 판정 캐시(AddTarget::SetEffectiveness -> 35086).
        // ActiveEffect 풀 주소 재사용으로 인한 오적용을 막기 위해 (usUniqueID, spell, effect) 동일성 키를 함께 검증한다.
        std::mutex g_effectCacheMutex;
        struct EffectCacheEntry
        {
            std::uint16_t uniqueId = 0;
            const RE::MagicItem* spell = nullptr;
            const RE::Effect* effect = nullptr;
            bool hasSkyrimResist = false;
            float skyrimResistMult = 1.0f;
            bool hasHitRoll = false;
            bool spellBlocked = false;
            bool spellSuppressed = false;
            double suppressPreventPct = 50.0;

            [[nodiscard]] bool Matches(const RE::ActiveEffect* ae) const noexcept
            {
                return ae && uniqueId == ae->usUniqueID && spell == ae->spell && effect == ae->effect;
            }
        };
        std::unordered_map<const RE::ActiveEffect*, EffectCacheEntry> g_effectCache;

        // 주 갱신 스레드에서만 읽고 쓰는 동기화 기준값(스카이림 레벨업·장비 강화 변화도 감지)
        std::uint64_t g_lastSyncedSnapVersion = 0;
        double g_lastObservedPermHealth = -1.0;
        double g_lastObservedPermMagicka = -1.0;

        // 진단·자동 시험용: 플레이어->동료 실제 체력 피해 누적 횟수
        std::atomic<std::uint64_t> g_playerToTeammateHits{ 0 };

        [[nodiscard]] double RollUnit()
        {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            return dist(t_rng);
        }

        void PostHudNotice(const char* text)
        {
            if (!text || !g_queue) {
                return;
            }
            std::string msg(text);
            g_queue->Enqueue([msg = std::move(msg)]() {
                RE::SendHUDMessage::ShowHUDMessage(msg.c_str());
            });
        }

        bool ContainsAddress(const REL::Segment& seg, std::uintptr_t addr, std::size_t size) noexcept
        {
            const auto begin = seg.address();
            const auto end = begin + seg.size();
            return addr >= begin && addr <= end && size <= (end - addr);
        }

        bool ReadRel32Target(std::uintptr_t site, std::uintptr_t& outTarget) noexcept
        {
            const auto textSeg = REL::Module::get().segment(REL::Segment::textx);
            if (!ContainsAddress(textSeg, site, 5)) {
                return false;
            }
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(site);
            if (bytes[0] != 0xE8) {
                return false;
            }
            std::int32_t rel32 = 0;
            std::memcpy(&rel32, bytes + 1, sizeof(rel32));
            outTarget = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(site + 5) + static_cast<std::intptr_t>(rel32));
            return true;
        }

        // ---- 1. HitData 적용(AE 38586) 8개 호출 자리 ----
        void ProcessApplyHit(std::size_t siteIdx, RE::Actor* target, RE::HitData* hit)
        {
            if (!target || !hit || !g_statEngine || !g_dataPack || !g_config || !g_dataPack->IsLoaded()) {
                return;
            }
            if (SkillExecution::IsInPluginDirectDamage()) {
                return;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            const auto aggressor = hit->aggressor.get();

            // (A) 플레이어가 적을 공격하는 경로
            if (aggressor && aggressor.get() == player && !target->IsPlayerRef()) {
                // M8: 모든 타격(사이클론·바닐라 근접·활·작업 큐 지연)에서 피해 전 첫 적대 스냅샷 선기록(결정 134, 140)
                if (g_killTracker && !hit->flags.any(RE::HitData::Flag::kPredictDamage)) {
                    g_killTracker->RecordFirstHitHostility(target->GetFormID(), target->IsHostileToActor(player));
                }

                // M6: 사이클론 근접 타격 문맥 활성 시(38627+0x4A8) PoE 피해로 치환(결정 65, 66, 93)
                if (siteIdx == 0 && t_cycloneCtx.active &&
                    (t_cycloneCtx.expectedTargetId == 0 || t_cycloneCtx.expectedTargetId == target->GetFormID())) {
                    // 결정 66: physicalDamage가 0이면 (1 - percentBlocked)를 폴백 비율로 사용
                    const double engineRatio = (hit->physicalDamage > 1e-4f)
                        ? std::max(0.0, static_cast<double>(hit->totalDamage) / static_cast<double>(hit->physicalDamage))
                        : std::clamp(1.0 - static_cast<double>(hit->percentBlocked), 0.0, 1.0);
                    const double sneakMult = (hit->bonusHealthDamageMult > 0.0f)
                        ? static_cast<double>(hit->bonusHealthDamageMult)
                        : 1.0;
                    const double replaced = std::max(0.0, t_cycloneCtx.convertedSkyrimDamage * engineRatio * sneakMult);
                    hit->totalDamage = static_cast<float>(replaced);
                    hit->flags.set(RE::HitData::Flag::kIgnoreCritical);
                }
                return;
            }

            // (B) 플레이어가 타격을 받는 경로(siteIdx 0..6; 7=task_queue는 앞선 자리에서 이미 환산됐으므로 제외)
            if (target->IsPlayerRef() && siteIdx < 7) {
                if (target->IsDead() || target->IsInKillMove() || hit->flags.any(RE::HitData::Flag::kPredictDamage)) {
                    return;
                }

                float explRatio = 1.0f;
                if (siteIdx == 1) {  // explosion
                    if (t_lastExplHit == hit) {
                        explRatio = t_lastExplRatio;
                    }
                    t_lastExplHit = nullptr;
                    t_lastExplRatio = 1.0f;
                }

                const double preBlock = (hit->physicalDamage > 1e-4f)
                    ? static_cast<double>(hit->physicalDamage) * static_cast<double>(explRatio) *
                          std::clamp(1.0 - static_cast<double>(hit->percentBlocked), 0.0, 1.0)
                    : static_cast<double>(hit->totalDamage);

                if (!std::isfinite(preBlock) || preBlock <= 0.0) {
                    return;
                }

                const auto snap = g_statEngine->GetSnapshot();
                if (!snap) {
                    return;
                }

                // 1) PoE 회피 판정(결정 72)
                if (snap->evasion > 0.0) {
                    const double acc = g_dataPack->MonsterAccuracy(snap->exileLevel);
                    const double hitChance = Core::EvasionHitChance(snap->evasion, acc, g_dataPack->Constants().minHitChance);
                    if (RollUnit() >= hitChance) {
                        hit->totalDamage = 0.0f;
                        PostHudNotice("회피");
                        return;
                    }
                }

                // 2) PoE 공격 막기 판정(결정 73)
                if (snap->attackBlockChance > 0.0 && RollUnit() * 100.0 < snap->attackBlockChance) {
                    hit->totalDamage = 0.0f;
                    PostHudNotice("막음");
                    return;
                }

                // 3) PoE 물리 방어 + 기대 방어 되돌림 환산(결정 68, 69, 94, 142)
                const double postDefense = Core::ConvertDefensePhysicalHit(
                    preBlock, snap->exileLevel, *snap, *g_dataPack, *g_config);

                // 4) 난이도 배율 보정 에너지 실드 선차감(검수 C2 해법)
                if (g_esPool) {
                    const double diffMult = SkillExecution::GetDifficultyMultiplier(true);
                    const auto absorb = g_esPool->AbsorbDamage(postDefense, diffMult, false);
                    hit->totalDamage = static_cast<float>(absorb.remainingRawDamage);
                } else {
                    hit->totalDamage = static_cast<float>(postDefense);
                }
            }
        }

        template <std::size_t SiteIdx>
        struct ApplyHitHook
        {
            static void Thunk(RE::Actor* target, RE::HitData* hit)
            {
                try {
                    ProcessApplyHit(SiteIdx, target, hit);
                } catch (...) {
                }
                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::Actor*, RE::HitData*);
                    reinterpret_cast<Fn>(orig)(target, hit);
                }
            }

            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        template <std::size_t SiteIdx>
        bool InstallApplyHitSite()
        {
            const auto& spec = Addresses::kAeApplyHitSites[SiteIdx];
            const auto site = REL::ID(spec.id).address() + spec.offset;
            std::uintptr_t preTarget = 0;
            if (!ReadRel32Target(site, preTarget)) {
                SKSE::log::warn("CombatHooks: ApplyHit site '{}' (ID {}+0x{:X}) is not a valid E8 call; skipped.",
                    spec.name, spec.id, spec.offset);
                return false;
            }
            auto& trampoline = SKSE::GetTrampoline();
            if (trampoline.free_size() < 14) {
                SKSE::log::error("CombatHooks: insufficient trampoline space for ApplyHit '{}'.", spec.name);
                return false;
            }
            ApplyHitHook<SiteIdx>::originalAddr.store(preTarget, std::memory_order_release);
            const auto previous = trampoline.write_call<5>(site, ApplyHitHook<SiteIdx>::Thunk);
            ApplyHitHook<SiteIdx>::originalAddr.store(previous, std::memory_order_release);
            return true;
        }

        // ---- 2. 폭발 비율 훅 (44004+0xC1 -> 44016) ----
        struct ExplosionStepsHook
        {
            static void Thunk(RE::HitData* hit)
            {
                const float totalBefore = hit ? hit->totalDamage : 0.0f;
                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::HitData*);
                    reinterpret_cast<Fn>(orig)(hit);
                }
                if (!hit) {
                    t_lastExplHit = nullptr;
                    t_lastExplRatio = 1.0f;
                    return;
                }
                const auto target = hit->target.get();
                if (target && target->IsPlayerRef()) {
                    t_lastExplHit = hit;
                    t_lastExplRatio = (totalBefore > 1e-4f)
                        ? std::clamp(hit->totalDamage / totalBefore, 0.0f, 10.0f)
                        : 1.0f;
                }
            }
            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        // ---- 3. AddTarget::SetEffectiveness (34547+0x656 -> 34052) ----
        struct SetEffectivenessHook
        {
            static void Thunk(RE::ActiveEffect* self, float effectiveness, bool flag)
            {
                try {
                    if (self && self->spell && self->effect && self->effect->baseEffect &&
                        self->effect->baseEffect->IsDetrimental()) {
                        if (auto* target = self->GetTargetActor(); target && target->IsPlayerRef()) {
                            float resMult = 1.0f;
                            if (self->spell->GetSpellType() != RE::MagicSystem::SpellType::kAbility &&
                                !self->spell->IgnoresResistance()) {
                                if (auto* mt = target->AsMagicTarget()) {
                                    resMult = mt->CheckResistance(self->spell, self->effect, self->source);
                                }
                            }
                            std::scoped_lock lock(g_effectCacheMutex);
                            if (g_effectCache.size() > 2048) {
                                g_effectCache.clear();
                            }
                            // 풀 주소 재사용 시 이전 효과의 판정이 남지 않도록 엔트리 전체를 새로 덮어쓴다.
                            EffectCacheEntry fresh;
                            fresh.uniqueId = self->usUniqueID;
                            fresh.spell = self->spell;
                            fresh.effect = self->effect;
                            fresh.hasSkyrimResist = true;
                            fresh.skyrimResistMult = resMult;
                            g_effectCache[self] = fresh;
                        }
                    }
                } catch (...) {
                }

                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::ActiveEffect*, float, bool);
                    reinterpret_cast<Fn>(orig)(self, effectiveness, flag);
                }
            }
            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        // ---- 4. 받는 마법 피해 (ValueModifierEffect::ModifyActorValue, ID 35086) ----
        struct MagicDamageMapping
        {
            Core::DamageType type = Core::DamageType::Fire;
            bool useAverageElemental = false;
        };

        MagicDamageMapping ClassifyMagicDamage(const RE::EffectSetting* baseEffect) noexcept
        {
            if (!baseEffect) {
                return { Core::DamageType::Fire, true };
            }
            switch (baseEffect->data.resistVariable) {
                case RE::ActorValue::kResistFire:
                    return { Core::DamageType::Fire, false };
                case RE::ActorValue::kResistFrost:
                    return { Core::DamageType::Cold, false };
                case RE::ActorValue::kResistShock:
                    return { Core::DamageType::Lightning, false };
                case RE::ActorValue::kPoisonResist:
                case RE::ActorValue::kResistDisease:
                    return { Core::DamageType::Chaos, false };
                default:
                    return { Core::DamageType::Fire, true };
            }
        }

        float ProcessMagicModifyActorValue(
            RE::ValueModifierEffect* self,
            RE::Actor* actor,
            float value,
            RE::ActorValue av)
        {
            if (!self || !actor || !(value < 0.0f)) {
                return value;
            }
            const auto actualAv = (av == RE::ActorValue::kNone) ? self->actorValue : av;
            // 결정 83: 냉기·전격의 지구력·매지카 몫은 건드리지 않고 체력 몫만 PoE 방어 적용
            if (actualAv != RE::ActorValue::kHealth) {
                return value;
            }

            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return value;
            }

            // (A) 플레이어가 적에게 거는 바닐라 주문·마법부여·독: 피해 수정 없이 사전 적대만 선기록(결정 134, 140, 142)
            if (!actor->IsPlayerRef()) {
                if (g_killTracker) {
                    if (const auto caster = self->GetCasterActor(); caster && caster.get() == player) {
                        g_killTracker->RecordFirstHitHostility(actor->GetFormID(), actor->IsHostileToActor(player));
                    }
                }
                return value;
            }

            // (B) 플레이어가 받는 마법 피해
            if (actor->IsDead() || actor->IsInKillMove() ||
                !g_statEngine || !g_dataPack || !g_config || !g_dataPack->IsLoaded()) {
                return value;
            }
            if (!self->spell || !self->effect || !self->effect->baseEffect) {
                // 결정 77: 주문/효과 정보 없는 환경 피해는 엔진 그대로 둔다
                return value;
            }
            const auto* baseEffect = self->effect->baseEffect;
            // 결정 81: kRecover(일시 체력 증가 만료 되돌림)은 피해로 보지 않는다
            if (baseEffect->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kRecover)) {
                return value;
            }
            if (!baseEffect->IsDetrimental()) {
                return value;
            }

            const auto snap = g_statEngine->GetSnapshot();
            if (!snap) {
                return value;
            }

            // 1) AddTarget::SetEffectiveness에서 기록한 스카이림 저항 배율 r 복원(동일성 키 일치 시에만 신뢰, 결정 71·78)
            float skyrimRes = 1.0f;
            bool hasCachedRes = false;
            {
                std::scoped_lock lock(g_effectCacheMutex);
                if (const auto it = g_effectCache.find(self); it != g_effectCache.end()) {
                    if (it->second.Matches(self)) {
                        if (it->second.hasSkyrimResist) {
                            skyrimRes = it->second.skyrimResistMult;
                            hasCachedRes = true;
                        }
                    } else {
                        // 풀 주소가 새 효과에 재사용됐으므로 낡은 엔트리를 즉시 폐기한다.
                        g_effectCache.erase(it);
                    }
                }
            }
            if (!hasCachedRes) {
                if (self->spell->GetSpellType() != RE::MagicSystem::SpellType::kAbility &&
                    !self->spell->IgnoresResistance()) {
                    if (auto* mt = actor->AsMagicTarget()) {
                        skyrimRes = mt->CheckResistance(self->spell, self->effect, self->source);
                    }
                }
            }

            double preResistD = static_cast<double>(-value);
            if (std::isfinite(skyrimRes) && skyrimRes > 1e-4f && skyrimRes < 10.0f) {
                preResistD /= static_cast<double>(skyrimRes);
            }

            // 2) 타격 vs 지속 피해 분류 및 주문 막기·주문 억제(결정 82, 87)
            const bool isConcentration =
                (self->spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration);
            const bool isHit = (self->effect->effectItem.duration == 0) || isConcentration;

            if (isHit) {
                if (isConcentration) {
                    const double blockPass = 1.0 - std::clamp(snap->spellBlockChance, 0.0, 75.0) / 100.0;
                    const double supChance = std::clamp(snap->spellSuppressionChance, 0.0, 100.0) / 100.0;
                    const double supPrevent = std::clamp(snap->spellSuppressionPrevent, 0.0, 100.0) / 100.0;
                    preResistD *= blockPass * (1.0 - supChance * supPrevent);
                } else {
                    bool blocked = false;
                    bool suppressed = false;
                    double preventPct = snap->spellSuppressionPrevent;
                    bool firstRoll = false;
                    {
                        std::scoped_lock lock(g_effectCacheMutex);
                        if (g_effectCache.size() > 2048) {
                            g_effectCache.clear();
                        }
                        auto& entry = g_effectCache[self];
                        if (!entry.Matches(self)) {
                            entry = EffectCacheEntry{};
                            entry.uniqueId = self->usUniqueID;
                            entry.spell = self->spell;
                            entry.effect = self->effect;
                        }
                        if (!entry.hasHitRoll) {
                            entry.hasHitRoll = true;
                            entry.spellBlocked = (snap->spellBlockChance > 0.0 && RollUnit() * 100.0 < snap->spellBlockChance);
                            entry.spellSuppressed = (!entry.spellBlocked && snap->spellSuppressionChance > 0.0 &&
                                                     RollUnit() * 100.0 < snap->spellSuppressionChance);
                            entry.suppressPreventPct = snap->spellSuppressionPrevent;
                            firstRoll = true;
                        }
                        blocked = entry.spellBlocked;
                        suppressed = entry.spellSuppressed;
                        preventPct = entry.suppressPreventPct;
                    }
                    if (blocked) {
                        if (firstRoll) {
                            PostHudNotice("막음");
                        }
                        return 0.0f;
                    }
                    if (suppressed) {
                        preResistD *= (1.0 - std::clamp(preventPct, 0.0, 100.0) / 100.0);
                    }
                }
            }

            // 3) PoE 저항 + PoE 3.30 인내 원소 피해 감소 + 기대 저항 되돌림(결정 71, 74, 142)
            const auto mapping = ClassifyMagicDamage(baseEffect);
            double postDefense = 0.0;
            if (mapping.useAverageElemental) {
                Core::StatSnapshot avgSnap = *snap;
                const double avgElem = (snap->fireResist + snap->coldResist + snap->lightningResist) / 3.0;
                avgSnap.fireResist = avgElem;
                postDefense = Core::ConvertDefenseResistDamage(
                    preResistD, Core::DamageType::Fire, snap->exileLevel, avgSnap, *g_dataPack, *g_config);
            } else {
                postDefense = Core::ConvertDefenseResistDamage(
                    preResistD, mapping.type, snap->exileLevel, *snap, *g_dataPack, *g_config);
            }

            // 4) 난이도 배율 보정 에너지 실드 선차감(카오스는 ES 우회)
            const bool isChaos = (mapping.type == Core::DamageType::Chaos);
            double finalRaw = postDefense;
            if (g_esPool) {
                const double diffMult = SkillExecution::GetDifficultyMultiplier(true);
                const auto absorb = g_esPool->AbsorbDamage(postDefense, diffMult, isChaos);
                finalRaw = absorb.remainingRawDamage;
            }
            return -static_cast<float>(std::max(0.0, finalRaw));
        }

        template <std::size_t Index>
        struct MagicModifyHook
        {
            static void Thunk(RE::ValueModifierEffect* self, RE::Actor* actor, float value, RE::ActorValue av)
            {
                float modified = value;
                try {
                    modified = ProcessMagicModifyActorValue(self, actor, value, av);
                } catch (...) {
                    modified = value;
                }
                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::ValueModifierEffect*, RE::Actor*, float, RE::ActorValue);
                    reinterpret_cast<Fn>(orig)(self, actor, modified, av);
                }
            }
            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        struct MagicVtableSpec
        {
            std::string_view name;
            REL::VariantID id;
        };

        constexpr std::array<MagicVtableSpec, 15> kMagicVtables = {{
            { "ValueModifierEffect",             RE::VTABLE_ValueModifierEffect[0] },
            { "AccumulatingValueModifierEffect", RE::VTABLE_AccumulatingValueModifierEffect[0] },
            { "PeakValueModifierEffect",         RE::VTABLE_PeakValueModifierEffect[0] },
            { "AbsorbEffect",                    RE::VTABLE_AbsorbEffect[0] },
            { "TargetValueModifierEffect",       RE::VTABLE_TargetValueModifierEffect[0] },
            { "DarknessEffect",                  RE::VTABLE_DarknessEffect[0] },
            { "InvisibilityEffect",              RE::VTABLE_InvisibilityEffect[0] },
            { "NightEyeEffect",                  RE::VTABLE_NightEyeEffect[0] },
            { "ParalysisEffect",                 RE::VTABLE_ParalysisEffect[0] },
            { "RallyEffect",                     RE::VTABLE_RallyEffect[0] },
            { "DemoralizeEffect",                RE::VTABLE_DemoralizeEffect[0] },
            { "TurnUndeadEffect",                RE::VTABLE_TurnUndeadEffect[0] },
            { "BanishEffect",                    RE::VTABLE_BanishEffect[0] },
            { "CalmEffect",                      RE::VTABLE_CalmEffect[0] },
            { "FrenzyEffect",                    RE::VTABLE_FrenzyEffect[0] },
        }};

        template <std::size_t I>
        bool InstallMagicVtable()
        {
            REL::Relocation<std::uintptr_t> vtable{ kMagicVtables[I].id };
            const auto slotAddr = vtable.address() + Addresses::kModifyActorValueVtableSlot * sizeof(std::uintptr_t);
            const auto currentTarget = *reinterpret_cast<const std::uintptr_t*>(slotAddr);
            if (currentTarget != REL::ID(Addresses::kAeModifyActorValueId).address()) {
                SKSE::log::warn("CombatHooks: magic vtable '{}' slot 0x20 points to 0x{:X} (expected 35086); chaining anyway.",
                    kMagicVtables[I].name, currentTarget);
            }
            MagicModifyHook<I>::originalAddr.store(currentTarget, std::memory_order_release);
            const auto prev = vtable.write_vfunc(Addresses::kModifyActorValueVtableSlot, MagicModifyHook<I>::Thunk);
            MagicModifyHook<I>::originalAddr.store(prev, std::memory_order_release);
            return true;
        }

        template <std::size_t I>
        bool InstallMagicDirectCall()
        {
            constexpr std::size_t HookIdx = kMagicVtables.size() + I;
            const auto& spec = Addresses::kAeMagicDirectCalls[I];
            const auto site = REL::ID(spec.id).address() + spec.offset;
            std::uintptr_t preTarget = 0;
            if (!ReadRel32Target(site, preTarget)) {
                SKSE::log::warn("CombatHooks: magic direct call '{}' (ID {}+0x{:X}) invalid; skipped.",
                    spec.name, spec.id, spec.offset);
                return false;
            }
            auto& trampoline = SKSE::GetTrampoline();
            if (trampoline.free_size() < 14) {
                SKSE::log::error("CombatHooks: insufficient trampoline space for magic call '{}'.", spec.name);
                return false;
            }
            MagicModifyHook<HookIdx>::originalAddr.store(preTarget, std::memory_order_release);
            const auto prev = trampoline.write_call<5>(site, MagicModifyHook<HookIdx>::Thunk);
            MagicModifyHook<HookIdx>::originalAddr.store(prev, std::memory_order_release);
            return true;
        }

        // ---- 5. 실제 체력 감소 기여 확정 (HandleHealthDamage, vtable 0x104) ----
        template <std::size_t Which>
        struct HandleHealthDamageHook
        {
            static void Thunk(RE::Actor* self, RE::Actor* attacker, float damage)
            {
                // 결정 140: HandleHealthDamage는 이미 체력이 실제로 깎인 뒤 음수 damage로 불리며,
                // orig 안에서 KillImpl/TESDeathEvent가 동기 발송될 수 있으므로 반드시 orig 호출 전에 기여를 확정한다.
                try {
                    if (self && !self->IsPlayerRef() && attacker && attacker->IsPlayerRef() && damage < 0.0f) {
                        if (self->IsPlayerTeammate()) {
                            g_playerToTeammateHits.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (g_killTracker) {
                            g_killTracker->ConfirmDamageContribution(self->GetFormID(), static_cast<double>(-damage));
                        }
                    }
                } catch (...) {
                }

                const auto orig = originalAddr.load(std::memory_order_acquire);
                if (orig != 0) {
                    using Fn = void (*)(RE::Actor*, RE::Actor*, float);
                    reinterpret_cast<Fn>(orig)(self, attacker, damage);
                }
            }
            static inline std::atomic<std::uintptr_t> originalAddr{ 0 };
        };

        template <std::size_t Which>
        void InstallHandleHealthDamage(const REL::VariantID& vtableId)
        {
            REL::Relocation<std::uintptr_t> vtable{ vtableId };
            const auto slotAddr = vtable.address() + Addresses::kHandleHealthDamageVtableSlot * sizeof(std::uintptr_t);
            const auto current = *reinterpret_cast<const std::uintptr_t*>(slotAddr);
            HandleHealthDamageHook<Which>::originalAddr.store(current, std::memory_order_release);
            const auto prev = vtable.write_vfunc(Addresses::kHandleHealthDamageVtableSlot, HandleHealthDamageHook<Which>::Thunk);
            HandleHealthDamageHook<Which>::originalAddr.store(prev, std::memory_order_release);
        }

        template <std::size_t... Is>
        std::size_t InstallAllApplyHit(std::index_sequence<Is...>)
        {
            return (static_cast<std::size_t>(InstallApplyHitSite<Is>()) + ...);
        }

        template <std::size_t... Is>
        std::size_t InstallAllMagicVtables(std::index_sequence<Is...>)
        {
            return (static_cast<std::size_t>(InstallMagicVtable<Is>()) + ...);
        }

        template <std::size_t... Is>
        std::size_t InstallAllMagicCalls(std::index_sequence<Is...>)
        {
            return (static_cast<std::size_t>(InstallMagicDirectCall<Is>()) + ...);
        }
    }

    ScopedCycloneHitContext::ScopedCycloneHitContext(double convertedSkyrimDamage, bool isCrit, RE::FormID targetId) noexcept
        : previous_(t_cycloneCtx)
    {
        t_cycloneCtx.active = true;
        t_cycloneCtx.convertedSkyrimDamage = convertedSkyrimDamage;
        t_cycloneCtx.isCrit = isCrit;
        t_cycloneCtx.expectedTargetId = targetId;
    }

    ScopedCycloneHitContext::~ScopedCycloneHitContext() noexcept
    {
        t_cycloneCtx = previous_;
    }

    bool CombatHooks::Install(
        Core::CommandQueue* queue,
        Core::StatEngine* statEngine,
        Core::EnergyShieldPool* esPool,
        Core::KillTracker* killTracker,
        const Core::DataPack* dataPack,
        const Core::Config* config)
    {
        if (g_installed) {
            return true;
        }
        g_queue = queue;
        g_statEngine = statEngine;
        g_esPool = esPool;
        g_killTracker = killTracker;
        g_dataPack = dataPack;
        g_config = config;

        const auto ver = REL::Module::get().version();
        if (!Addresses::IsVerifiedRuntime(ver)) {
            SKSE::log::warn("CombatHooks: skipped on unverified runtime {}.", ver.string("."));
            return false;
        }

        if (g_esPool && g_dataPack && g_dataPack->IsLoaded()) {
            g_esPool->Configure(g_dataPack->Constants().esRechargeDelay, g_dataPack->Constants().esRechargeRate);
        }

        const auto hitInstalled = InstallAllApplyHit(std::make_index_sequence<Addresses::kAeApplyHitSites.size()>{});

        bool explInstalled = false;
        {
            const auto site = REL::ID(Addresses::kAeExplosionStepsSite.id).address() + Addresses::kAeExplosionStepsSite.offset;
            std::uintptr_t preTarget = 0;
            if (ReadRel32Target(site, preTarget) && SKSE::GetTrampoline().free_size() >= 14) {
                ExplosionStepsHook::originalAddr.store(preTarget, std::memory_order_release);
                const auto prev = SKSE::GetTrampoline().write_call<5>(site, ExplosionStepsHook::Thunk);
                ExplosionStepsHook::originalAddr.store(prev, std::memory_order_release);
                explInstalled = true;
            }
        }

        bool effInstalled = false;
        {
            const auto site = REL::ID(Addresses::kAeSetEffectivenessSite.id).address() + Addresses::kAeSetEffectivenessSite.offset;
            std::uintptr_t preTarget = 0;
            if (ReadRel32Target(site, preTarget) && SKSE::GetTrampoline().free_size() >= 14) {
                SetEffectivenessHook::originalAddr.store(preTarget, std::memory_order_release);
                const auto prev = SKSE::GetTrampoline().write_call<5>(site, SetEffectivenessHook::Thunk);
                SetEffectivenessHook::originalAddr.store(prev, std::memory_order_release);
                effInstalled = true;
            }
        }

        const auto magicVtInstalled = InstallAllMagicVtables(std::make_index_sequence<kMagicVtables.size()>{});
        const auto magicCallInstalled = InstallAllMagicCalls(std::make_index_sequence<Addresses::kAeMagicDirectCalls.size()>{});

        InstallHandleHealthDamage<0>(RE::VTABLE_Character[0]);
        InstallHandleHealthDamage<1>(RE::VTABLE_PlayerCharacter[0]);

        SaveLoad::AddRevertCallback([]() {
            {
                std::scoped_lock lock(g_effectCacheMutex);
                g_effectCache.clear();
            }
            if (g_esPool) {
                g_esPool->Clear();
            }
            g_lastSyncedSnapVersion = 0;
            g_lastObservedPermHealth = -1.0;
            g_lastObservedPermMagicka = -1.0;
        });

        g_installed = true;
        SKSE::log::info(
            "CombatHooks: installed ApplyHit={}/{}, ExplosionSteps={}, SetEffectiveness={}, MagicVtable={}/{}, MagicCalls={}/{}, HandleHealthDamage=2/2.",
            hitInstalled, Addresses::kAeApplyHitSites.size(),
            explInstalled, effInstalled,
            magicVtInstalled, kMagicVtables.size(),
            magicCallInstalled, Addresses::kAeMagicDirectCalls.size());
        return true;
    }

    void CombatHooks::TickPlayerPools(double deltaSec, bool gamePaused)
    {
        if (g_esPool) {
            g_esPool->Tick(deltaSec, gamePaused);
        }

        if (gamePaused || !g_statEngine || !g_dataPack || !g_config || !g_dataPack->IsLoaded()) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded() || player->IsDead()) {
            return;
        }
        auto* avOwner = player->AsActorValueOwner();
        if (!avOwner) {
            return;
        }

        const auto snap = g_statEngine->GetSnapshot();
        if (!snap) {
            return;
        }

        const double currentPermHealth = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kHealth));
        const double currentPermMagicka = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kMagicka));

        // 스탯 스냅샷 변경뿐 아니라 스카이림 레벨업·장비 강화로 영구 체력/매지카가 바뀐 경우도 즉시 재동기화한다(결정 70, 125).
        const bool snapChanged = (snap->version != g_lastSyncedSnapVersion);
        const bool permChanged = (g_lastObservedPermHealth < 0.0) ||
            (std::abs(currentPermHealth - g_lastObservedPermHealth) > 0.25) ||
            (std::abs(currentPermMagicka - g_lastObservedPermMagicka) > 0.25);

        if (!snapChanged && !permChanged) {
            return;
        }

        const auto stateCopy = SaveLoad::GetStateCopy();
        double appliedHealth = stateCopy.appliedHealthDelta;
        double appliedMagicka = stateCopy.appliedMagickaDelta;
        bool deltasChanged = false;

        // 결정 67, 70: 기대 생명력 대비 배수로 최대 체력 보정
        const double baseSkyrimHealth = std::max(1.0, currentPermHealth - appliedHealth);
        const double targetHealth = Core::ComputeSkyrimMaxHealth(
            baseSkyrimHealth, snap->poeMaxLife, snap->exileLevel, *g_dataPack, *g_config);
        const double desiredHealthDelta = targetHealth - baseSkyrimHealth;
        if (std::abs(desiredHealthDelta - appliedHealth) > 0.25) {
            const float diff = static_cast<float>(desiredHealthDelta - appliedHealth);
            // 음수 보정(예: 패시브 미배분 레벨업)이 저체력 플레이어를 즉사시키지 않도록 최소 체력 1.0 보장
            if (diff < 0.0f) {
                const float curHealth = avOwner->GetActorValue(RE::ActorValue::kHealth);
                if (curHealth > 0.0f && (curHealth + diff) < 1.0f) {
                    const float compensate = std::min(-diff, 1.0f - (curHealth + diff));
                    if (compensate > 0.0f) {
                        avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, compensate);
                    }
                }
            }
            avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kHealth, diff);
            appliedHealth += static_cast<double>(diff);
            deltasChanged = true;
        }

        // 결정 70, 85: 에너지 실드 최대치 동기화
        if (g_esPool) {
            const double targetMaxEs = Core::ComputeSkyrimMaxEnergyShield(
                baseSkyrimHealth, snap->poeMaxEnergyShield, snap->exileLevel, *g_dataPack, *g_config);
            g_esPool->SetMax(targetMaxEs, false);
        }

        // 결정 125, 144: 기대 마나 대비 배수로 최대 매지카 보정(Blood Magic/마나 0이면 원본 유지)
        const double baseSkyrimMagicka = std::max(1.0, currentPermMagicka - appliedMagicka);
        const auto manaRes = Core::ComputeMagickaScaling(
            baseSkyrimMagicka, snap->poeMaxMana, 0.0, snap->exileLevel, *g_dataPack, *g_config);
        const double desiredMagickaDelta = manaRes.skyrimMaxMagicka - baseSkyrimMagicka;
        if (std::abs(desiredMagickaDelta - appliedMagicka) > 0.25) {
            const float diff = static_cast<float>(desiredMagickaDelta - appliedMagicka);
            avOwner->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kMagicka, diff);
            appliedMagicka += static_cast<double>(diff);
            deltasChanged = true;
        }

        if (deltasChanged) {
            SaveLoad::UpdateState([appliedHealth, appliedMagicka](PersistentState& st) {
                st.appliedHealthDelta = appliedHealth;
                st.appliedMagickaDelta = appliedMagicka;
            });
        }

        g_lastSyncedSnapVersion = snap->version;
        g_lastObservedPermHealth = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kHealth));
        g_lastObservedPermMagicka = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kMagicka));
    }

    double CombatHooks::GetBaseSkyrimMaxMagicka(RE::PlayerCharacter* player) noexcept
    {
        if (!player) {
            return 100.0;
        }
        auto* avOwner = player->AsActorValueOwner();
        if (!avOwner) {
            return 100.0;
        }
        const double perm = static_cast<double>(avOwner->GetPermanentActorValue(RE::ActorValue::kMagicka));
        const double appliedMagicka = SaveLoad::GetStateCopy().appliedMagickaDelta;
        return std::max(1.0, perm - appliedMagicka);
    }

    std::uint64_t CombatHooks::GetPlayerToTeammateHitCount() noexcept
    {
        return g_playerToTeammateHits.load(std::memory_order_relaxed);
    }
}
