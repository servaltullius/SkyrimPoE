#include "PCH.h"
#include "runtime/MigrationRuntime.h"

#include "runtime/AilmentRuntime.h"
#include "runtime/CycloneRuntime.h"
#include "runtime/FormIds.h"
#include "runtime/MonsterRuntime.h"
#include "runtime/ProxySpellRuntime.h"
#include "runtime/WorldDropTracker.h"

#include <REX/W32/KERNEL32.h>

namespace SkyrimPoE::Runtime
{
    namespace
    {
        using namespace Core;
        using enum CleanupStep;
        using enum ReturnKind;
        using enum LegacyRecordStatus;
        constexpr auto kPermanent = RE::ACTOR_VALUE_MODIFIER::kPermanent;
        constexpr auto kHealth = RE::ActorValue::kHealth;
        constexpr auto kMagicka = RE::ActorValue::kMagicka;
        constexpr auto kEscapeFailed = "지도 안에서 나오지 못해 정리하지 않았습니다. 이 세이브는 바뀌지 않았습니다";
        constexpr auto kUnreadableActiveRun = "진행 중이던 지도 회차 기록을 읽을 수 없어 정리하지 않았습니다";
        constexpr std::array<RE::FormID, 5> kLegacyMapsV1Cells{
            0x000B6BE6, 0x00043FAB, 0x00015224, 0x000152B1, 0x0001521C
        };
        constexpr RE::FormID kLegacyMapsV1ArenaCell = 0x00015211;

        RE::TESObjectMISC* LegacyGemReturnForm(std::string_view gemId)
        {
            const auto local = FormIds::FindLocalFormIdForGem(gemId);
            return local ? FormIds::Lookup<RE::TESObjectMISC>(local) : nullptr;
        }

        std::size_t DamagedCount(const LegacyReadReport& report)
        {
            const std::array records{ report.coreFront, report.gemKeys, report.tree, report.items, report.gear,
                report.spirit, report.atlas, report.maps, report.speedMods, report.worldDrops };
            return static_cast<std::size_t>(std::ranges::count(records, Damaged)) +
                (report.coreFront != Damaged && report.slotsDamaged ? 1 : 0);
        }

        std::string DescribeCandidate(const LegacyMigrationResult& candidate, std::uint32_t unresolvedMapForms) noexcept
        try
        {
            const auto& [state, report] = candidate;
            const auto describe = [](LegacyRecordStatus status, std::size_t count) -> std::string {
                if (status == Missing) return "없음";
                if (status == Damaged) return "손상(개수 모름)";
                return count == 0 ? "비어 있음" : std::to_string(count);
            };
            const auto count = [&](ReturnKind kind) {
                return static_cast<std::size_t>(std::ranges::count(state.returns, kind, &ReturnEntry::kind));
            };
            return std::format(
                "CORE 앞부분={}, 스킬 칸={}, GKEY={} (반환 보조 {}개), TREE={}, ITEM={}, GEAR={} (주얼·참·가상 슬롯), "
                "SPRT={}, ATLS={}, MAPS={}, SPDM={}, WDRP={}, FormID 해석 실패 {}개 (MAPS {}개), 중복 제거 {}개",
                describe(report.coreFront, 1),
                describe(report.slotsDamaged ? Damaged : report.coreFront, count(SlottedGem)),
                describe(report.gemKeys, report.gemKeyCount), count(BagSupport),
                describe(report.tree, report.treeNodeCount), describe(report.items, report.itemCount),
                describe(report.gear, count(Jewel) + count(Charm) + state.virtualRefs.size()),
                describe(report.spirit, report.auraCount), describe(report.atlas, report.atlasClears),
                describe(report.maps, state.mapRun.active ? 1 : 0),
                describe(report.speedMods, report.speedModCount), describe(report.worldDrops, state.worldDrops.size()),
                report.unresolvedCount + unresolvedMapForms, unresolvedMapForms, report.duplicateReturns);
        }
        catch (...) {
            return {};
        }

        void GivePoE2Starter(MigrationState& state)
        {
            SKSE::log::info("MigrationRuntime: 세대 {} 시작 물건 정의 없음(결정 200).", state.targetGeneration);
            state.starterGranted = true;
        }

        class RuntimeCleanupOps final : public CleanupOps
        {
        public:
            RuntimeCleanupOps(RE::PlayerCharacter* player, RE::ActorValueOwner& values,
                const ActiveMapState* run, RE::FormID cell, MigrationDiagnostics& diagnostics,
                const std::function<MigrationSpeedResult(const MigrationSpeedMod&)>& speed,
                const std::function<void()>& clearCosave)
                : player_(player), values_(values), run_(run), cell_(cell),
                  diagnostics_(diagnostics), speed_(speed), clearCosave_(clearCosave) {}

            StepOutcome CloseMapRun(MigrationState& state) override
            {
                if (run_) {
                    auto& result = diagnostics_.mapClose;
                    result = MapDeviceRuntime::CloseLegacyRun(player_, *run_, cell_, state.worldDrops);
                    using Refund = LegacyRunCloseResult::Refund;
                    const bool refundLost = result.refund == LegacyRunCloseResult::Refund::Lost;
                    const bool partial = diagnostics_.map == MigrationDiagnostics::MapResult::Partial ||
                        refundLost || result.refund == Refund::WithheldUnknown ||
                        result.spawnsFailed != 0 || result.dropsFailed != 0 || !result.cellResetQueued ||
                        run_->unresolvedBossRefFormId != 0;
                    diagnostics_.map = partial ? MigrationDiagnostics::MapResult::Partial : MigrationDiagnostics::MapResult::Closed;
                    diagnostics_.dropResults[static_cast<std::size_t>(WorldDropTracker::LegacyDropReturn::Lost)] += result.dropsFailed;
                    state.lostCount += static_cast<std::uint32_t>(result.dropsFailed) + refundLost;
                } else {
                    diagnostics_.map = MigrationDiagnostics::MapResult::Partial;
                }
                state.mapRunClosed = true;
                return StepOutcome::Done;
            }

            StepOutcome ReturnSlottedGems(MigrationState& state) override
            {
                ReturnItems(state, SlottedGem);
                return StepOutcome::Done;
            }

            StepOutcome DetachBagSupports(MigrationState& state) override
            {
                ReturnItems(state, BagSupport);
                return StepOutcome::Done;
            }

            StepOutcome ReturnJewelsAndCharms(MigrationState& state) override
            {
                ReturnItems(state, Jewel);
                ReturnItems(state, Charm);
                for (auto& ref : state.virtualRefs) {
                    ref.released = true;
                }
                return StepOutcome::Done;
            }

            StepOutcome RevertEngineState(MigrationState& state) override
            {
                auto& result = diagnostics_;
                result.healthBefore = player_->GetActorValueModifier(kPermanent, kHealth);
                result.magickaBefore = player_->GetActorValueModifier(kPermanent, kMagicka);
                if (std::abs(state.healthDelta) > 1e-6) {
                    const float revert = -static_cast<float>(state.healthDelta);
                    const float current = values_.GetActorValue(kHealth);
                    if (revert < 0.0f && current > 0.0f && current + revert < 1.0f) {
                        const float compensation = std::min(-revert, 1.0f - (current + revert));
                        values_.ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, kHealth, compensation);
                    }
                    values_.ModActorValue(kPermanent, kHealth, revert);
                }
                if (std::abs(state.magickaDelta) > 1e-6) {
                    values_.ModActorValue(kPermanent, kMagicka, -static_cast<float>(state.magickaDelta));
                }
                result.healthAfter = player_->GetActorValueModifier(kPermanent, kHealth);
                result.magickaAfter = player_->GetActorValueModifier(kPermanent, kMagicka);
                state.healthDelta = 0.0;
                state.magickaDelta = 0.0;
                state.deltasReverted = true;

                ProxySpellRuntime::RemoveAllProxySpells(player_);
                values_.SetActorValue(RE::ActorValue::kVariable09, 0.0f);
                AilmentRuntime::ClearPlayerAilments();
                AilmentRuntime::RevertAllTrackedSpeedsNow();
                MonsterRuntime::RevertAllHasteSpeedsNow();
                MapDeviceRuntime::ClearPlayerPoolsAndStats();
                CycloneRuntime::StopNowOnMainThread(player_);
                result.engineResidue = MigrationRuntime::ObserveEngineResidue(player_);
                const bool healthReverted = std::abs(result.healthAfter - result.healthBefore + result.healthDelta) <=
                    kLegacyDeltaTolerance;
                const bool magickaReverted = std::abs(result.magickaAfter - result.magickaBefore + result.magickaDelta) <=
                    kLegacyDeltaTolerance;
                result.engineRevertMismatch = !healthReverted || !magickaReverted || !result.engineResidue.Empty();
                return StepOutcome::Done;
            }

            StepOutcome SpeedModsAndWorldDrops(MigrationState& state) override
            {
                for (const auto& mod : state.speedMods) {
                    auto outcome = MigrationSpeedResult::Lost;
                    try {
                        outcome = speed_(mod);
                    } catch (...) {
                        // Report the loss with the other speed outcomes.
                    }
                    ++diagnostics_.speedResults[static_cast<std::size_t>(outcome)];
                    state.lostCount += outcome == MigrationSpeedResult::Lost;
                }
                using DropResult = WorldDropTracker::LegacyDropReturn;
                std::vector<SavedWorldDropEntry> deferred;
                deferred.reserve(state.worldDrops.size());
                for (auto& drop : state.worldDrops) {
                    if (drop.status == WorldDropStatus::Done) {
                        continue;
                    }
                    const SavedWorldDropEntry entry{ drop.dropRefFormId, drop.corpseRefFormId,
                        drop.beamRefFormId, drop.worldDropTier };
                    const bool alreadyLost = drop.status == WorldDropStatus::CorpseGone;
                    auto outcome = DropResult::Lost;
                    try {
                        outcome = WorldDropTracker::ReturnLegacyDrop(entry);
                    } catch (...) {
                        // A failed item is not retried after a possible partial engine change.
                    }
                    ++diagnostics_.dropResults[static_cast<std::size_t>(outcome)];
                    if (outcome == DropResult::Deferred) {
                        deferred.push_back(entry);
                    }
                    state.lostCount += outcome == DropResult::Lost && !alreadyLost;
                    drop.status = outcome == DropResult::Lost ? WorldDropStatus::CorpseGone : WorldDropStatus::Done;
                    drop.beamRefFormId = 0;
                }
                WorldDropTracker::RestoreFromSave(std::move(deferred));
                return StepOutcome::Done;
            }

            StepOutcome ClearCosave(MigrationState&) override
            {
                clearCosave_();
                return StepOutcome::Done;
            }

            StepOutcome GrantStarter(MigrationState& state) override
            {
                GivePoE2Starter(state);
                return StepOutcome::Done;
            }

            StepOutcome Finish(MigrationState& state) override
            {
                diagnostics_.lostCount = state.lostCount;
                diagnostics_.starterGranted = state.starterGranted;
                return StepOutcome::Done;
            }

        private:
            void ReturnItems(MigrationState& state, ReturnKind kind)
            {
                const auto index = static_cast<std::size_t>(kind);
                for (auto& entry : state.returns) {
                    if (entry.kind != kind || entry.status != ReturnStatus::Pending) {
                        continue;
                    }
                    RE::FormID formId = 0;
                    try {
                        if (auto* form = MigrationRuntime::ReturnForm(entry)) {
                            formId = form->GetFormID();
                            const auto before = player_->GetItemCount(form);
                            player_->AddObjectToContainer(form, nullptr, 1, nullptr);
                            if (player_->GetItemCount(form) > before) {
                                entry.status = ReturnStatus::Done;
                                ++diagnostics_.returned[index];
                                continue;
                            }
                        }
                    } catch (...) {
                        // Count this item as lost and continue through the ledger.
                    }
                    entry.status = ReturnStatus::Lost;
                    ++diagnostics_.lostReturns[index];
                    ++state.lostCount;
                    if (formId) {
                        ++diagnostics_.lostReturnsByForm[formId];
                    } else {
                        ++diagnostics_.lostReturnsWithoutForm;
                    }
                }
            }

            RE::PlayerCharacter* player_;
            RE::ActorValueOwner& values_;
            const ActiveMapState* run_;
            RE::FormID cell_;
            MigrationDiagnostics& diagnostics_;
            const std::function<MigrationSpeedResult(const MigrationSpeedMod&)>& speed_;
            const std::function<void()>& clearCosave_;
        };
    }

    std::string MigrationRuntime::Prepare(const Core::SaveRecordBatch& batch, const Core::SaveGeneration& target,
        const Core::FormIdResolver& resolve, std::uint64_t world)
    {
        Reset();
        candidate_ = Core::BuildLegacyMigration(batch, target.game, target.contentGeneration, resolve,
            Core::LegacyReturnDestination::Player);
        if (!candidate_.report.Succeeded()) {
            return std::string(candidate_.report.failureReason);
        }
        const auto& map = candidate_.state.mapRun;
        if (!map.bytes.empty()) {
            auto decoded = Core::ReadMapSave(map.version, map.bytes, [&](auto raw, auto& resolved) {
                const bool ok = resolve(raw, resolved) && resolved != 0;
                unresolvedMapForms_ += !ok;
                return ok;
            });
            if (!decoded.run || decoded.run->active != map.active) {
                candidate_.report.maps = Damaged;
            } else if (decoded.run->active) {
                if (map.version == Core::kLegacyIndexMapSaveVersion) {
                    legacyCell_ = decoded.run->isArena4 ? kLegacyMapsV1ArenaCell :
                        kLegacyMapsV1Cells[decoded.legacyDungeonIndex % kLegacyMapsV1Cells.size()];
                }
                run_ = std::move(decoded.run);
            }
        }
        phase_ = Phase::AwaitingLoad;
        world_ = world;
        SKSE::log::info("MigrationRuntime: candidate for world {}. {}", world, DescribeCandidate(candidate_, unresolvedMapForms_));
        return {};
    }

    void MigrationRuntime::Confirm(std::uint64_t world, RE::PlayerCharacter* player)
    {
        if (phase_ != Phase::AwaitingLoad || !IsForWorld(world)) {
            return;
        }
        if (run_) {
            legacyCell_ = MapDeviceRuntime::LegacyDungeonCell(*run_, legacyCell_);
        }
        const bool activeRunUnreadable = candidate_.state.mapRun.active && !run_;
        phase_ = !activeRunUnreadable && MapDeviceRuntime::ObserveLegacyEscape(player, legacyCell_).inPackCell
            ? Phase::Escaping : Phase::Ready;
    }

    void MigrationRuntime::Reset()
    {
        *this = {};
    }

    MigrationCandidateSnapshot MigrationRuntime::Snapshot(RE::PlayerCharacter* player) const
    {
        return { DescribeCandidate(candidate_, unresolvedMapForms_), candidate_.state.returns,
            candidate_.state.healthDelta, candidate_.state.magickaDelta, candidate_.state.mapRun.active,
            MapDeviceRuntime::ObserveLegacyEscape(player, legacyCell_).inPackCell };
    }

    bool MigrationRuntime::CanTick(RE::PlayerCharacter* player)
    {
        auto* ui = RE::UI::GetSingleton();
        return ui && !ui->GameIsPaused() && !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) &&
            !ui->IsMenuOpen(RE::FaderMenu::MENU_NAME) && player && player->Is3DLoaded() &&
            !player->IsDead() && player->AsActorValueOwner();
    }

    std::string_view MigrationRuntime::TickEscape(RE::PlayerCharacter* player, bool eligible)
    {
        if (phase_ != Phase::Escaping) {
            return {};
        }
        if (!eligible) {
            lastEligible_.reset();
            return {};
        }
        const auto now = std::chrono::steady_clock::now();
        if (lastEligible_) {
            escapeSeconds_ += std::min(0.25, std::chrono::duration<double>(now - *lastEligible_).count());
        }
        lastEligible_ = now;
        const auto observation = MapDeviceRuntime::ObserveLegacyEscape(player, legacyCell_);
        if (!observation.inPackCell ||
            (escape_ && Core::HasArrived(*escape_, observation))) {
            phase_ = Phase::Ready;
        } else if (escapeSeconds_ >= 60.0) {
            return kEscapeFailed;
        } else if (!escapeAttempted_) {
            escapeAttempted_ = true;
            escape_ = MapDeviceRuntime::RequestLegacyEscape(player, run_ ? &*run_ : nullptr, legacyCell_);
        }
        return {};
    }

    std::string_view MigrationRuntime::CheckCommitGate(RE::PlayerCharacter* player) const
    {
        if (candidate_.state.mapRun.active && !run_) {
            return kUnreadableActiveRun;
        }
        if (MapDeviceRuntime::ObserveLegacyEscape(player, legacyCell_).inPackCell) {
            return kEscapeFailed;
        }
        const auto& state = candidate_.state;
        if (Core::JudgeLegacyDelta(state.healthDelta, player->GetActorValueModifier(kPermanent, kHealth)) !=
                Core::DeltaTrust::Trusted ||
            Core::JudgeLegacyDelta(state.magickaDelta, player->GetActorValueModifier(kPermanent, kMagicka)) !=
                Core::DeltaTrust::Trusted) {
            return "옛 체력·매지카 보정값을 확인할 수 없어 정리하지 않았습니다.";
        }
        try {
            (void)Core::WriteMigrationRecord(state);
        } catch (...) {
            return "옛 세이브 정리 목록을 안전하게 처리할 수 없어 정리하지 않았습니다.";
        }
        return {};
    }

    MigrationDiagnostics MigrationRuntime::Execute(RE::PlayerCharacter* player, RE::ActorValueOwner& values,
        const std::function<MigrationSpeedResult(const Core::MigrationSpeedMod&)>& speed,
        const std::function<void()>& clearCosave)
    {
        auto& state = candidate_.state;
        MigrationDiagnostics diagnostics;
        diagnostics.candidateReport = DescribeCandidate(candidate_, unresolvedMapForms_);
        diagnostics.damagedRecords = DamagedCount(candidate_.report);
        if (candidate_.report.maps == Damaged || unresolvedMapForms_ != 0) {
            diagnostics.map = MigrationDiagnostics::MapResult::Partial;
        }
        diagnostics.healthDelta = state.healthDelta;
        diagnostics.magickaDelta = state.magickaDelta;
        diagnostics.speedInputCount = candidate_.report.speedModCount;
        diagnostics.speedModsFiltered = candidate_.report.speedModsFiltered;
        diagnostics.speedModsUnresolved = candidate_.report.speedModsUnresolved;
        diagnostics.tickThreadId = REX::W32::GetCurrentThreadId();
        RuntimeCleanupOps ops(player, values, run_ ? &*run_ : nullptr, legacyCell_, diagnostics, speed, clearCosave);
        if (Core::CompleteCleanup(state, ops) != Core::StepOutcome::Done) {
            throw std::logic_error("A migration operation did not finish in its commit tick.");
        }
        diagnostics.step = state.step;
        return diagnostics;
    }

    RE::TESObjectMISC* MigrationRuntime::ReturnForm(const Core::ReturnEntry& entry)
    {
        switch (entry.kind) {
        case SlottedGem:
        case BagSupport:
            return LegacyGemReturnForm(entry.gemId);
        case Jewel:
            if (auto* base = RE::TESForm::LookupByID<RE::TESObjectMISC>(entry.baseFormId);
                base && FormIds::JewelColorFromLocalFormId(FormIds::ResolveLocalFormIdIfOurs(base->GetFormID()))) {
                return base;
            }
            return FormIds::Lookup<RE::TESObjectMISC>(FormIds::JewelLocalFormId(entry.jewelColor));
        case Charm:
            if (const auto local = FormIds::CharmLocalFormId(entry.charmKind)) {
                return FormIds::Lookup<RE::TESObjectMISC>(local);
            }
            return nullptr;
        }
        return nullptr;
    }

    MigrationEngineResidue MigrationRuntime::ObserveEngineResidue(RE::PlayerCharacter* player)
    {
        const auto* values = player->AsActorValueOwner();
        const auto hasSpell = [player](RE::FormID local) {
            auto* spell = FormIds::Lookup<RE::SpellItem>(local);
            return spell && player->HasSpell(spell);
        };
        return {
            values ? values->GetActorValue(RE::ActorValue::kVariable09) : -1.0f,
            static_cast<std::size_t>(std::ranges::count_if(FormIds::kProxySpells, hasSpell)),
            static_cast<std::size_t>(std::ranges::count_if(FormIds::kProxyPowers, hasSpell))
        };
    }

    std::string_view MigrationDiagnostics::MapResultName() const noexcept
    {
        constexpr std::array mapResults{ "없음", "닫음", "일부 실패" };
        return mapResults[static_cast<std::size_t>(map)];
    }

    std::string MigrationDiagnostics::Describe() const
    {
        constexpr std::array refundResults{ "억제", "환불", "실패", "환불 보류(확인 불가)" };
        std::string lostForms;
        for (const auto& [form, count] : lostReturnsByForm) {
            lostForms += std::format(" {:08X}={}", form, count);
        }
        return std::format(
            "step={} (1~9 완료={}), 반환[젬/보조/주얼/참]={}/{}/{}/{}, 반환 실패={}/{}/{}/{} (폼{}, 미해석 {}), 손실 합계={}, 손상={} ({}), "
            "체력 원복={:.6f}->{:.6f} (델타 {:.6f}), 매지카 원복={:.6f}->{:.6f} (델타 {:.6f}), Variable09={}, 대리 주문={}, 파워={}, {}, "
            "지도={}, 회차 드랍 폐기={}, 폐기 실패={}, 스폰 정리 실패={}, 셀 리셋 예약={}, 입장 비용={}, "
            "SPDM 원본={}, [원복/이미 0/보류/포기/입력에서 걸러짐]={}/{}/{}/{}/{}, "
            "WDRP[회수/이미 회수됨/이관/포기]={}/{}/{}/{}, 시작 지급 완료={}, tick thread={}",
            static_cast<unsigned>(step), step == Complete,
            returned[0], returned[1], returned[2], returned[3], lostReturns[0], lostReturns[1], lostReturns[2], lostReturns[3],
            lostForms, lostReturnsWithoutForm, lostCount, damagedRecords, candidateReport, healthBefore, healthAfter, healthDelta,
            magickaBefore, magickaAfter, magickaDelta, engineResidue.variable09, engineResidue.proxySpells, engineResidue.proxyPowers,
            engineRevertMismatch ? "원복 불일치" : "원복 확인", MapResultName(),
            mapClose.dropsDiscarded, mapClose.dropsFailed, mapClose.spawnsFailed, mapClose.cellResetQueued,
            refundResults[static_cast<std::size_t>(mapClose.refund)],
            speedInputCount, speedResults[0], speedResults[1], speedResults[2], speedResults[3], speedModsFiltered,
            dropResults[0], dropResults[1], dropResults[2], dropResults[3], starterGranted, tickThreadId);
    }
}
