#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace SkyrimPoE::Core
{
    // 시험 세션 보호(C8·F7·결정 256) 순수 상태 기계.
    // - active_: 시험 세션이 열려 있는 동안 허용된 출력 슬롯의 명시 요청 1회 외 모든 코세이브 쓰기를 거부한다.
    // - aborted_: 한 번 중단되면 sticky로 유지되며 Begin/SetAllowedSlot/LockForQuit/OnLoadStarted로 풀리지 않는다.
    // - allowPreserveSave_: preserve 모드에서만 ModMode::Preserve 상태의 요청 저장(제품 replay 경로)을 허용한다.
    class TestSessionGate
    {
    public:
        void Begin(std::string allowedSaveSlot, bool allowPreserveSave = false)
        {
            active_ = true;
            saveAllowed_ = false;
            if (aborted_) {
                lockedForQuit_ = true;
                allowedSlot_.clear();
                allowPreserveSave_ = false;
            } else {
                lockedForQuit_ = false;
                allowedSlot_ = std::move(allowedSaveSlot);
                allowPreserveSave_ = allowPreserveSave;
            }
        }

        // preserve 모드처럼 한 기동에서 여러 출력 슬롯을 차례로 쓸 때 허용 슬롯을 바꾼다.
        // 세션이 비활성이거나 종료 대기 잠금(LockForQuit) 또는 중단 상태면 거부한다.
        [[nodiscard]] bool SetAllowedSlot(std::string allowedSaveSlot)
        {
            if (!active_ || aborted_ || lockedForQuit_) {
                return false;
            }
            allowedSlot_ = std::move(allowedSaveSlot);
            saveAllowed_ = false;
            return true;
        }

        void LockForQuit(bool aborted)
        {
            active_ = true;
            lockedForQuit_ = true;
            allowedSlot_.clear();
            saveAllowed_ = false;
            if (aborted) {
                aborted_ = true;
                allowPreserveSave_ = false;
            }
        }

        void OnLoadStarted() noexcept
        {
            saveAllowed_ = false;
        }

        template <class SameStemFn>
        bool AuthorizeSave(
            std::string_view stem,
            bool requested,
            bool isLoading,
            bool allowsGameplay,
            SameStemFn&& sameStem,
            bool isPreserveMode = false)
        {
            const bool modeOk = allowsGameplay || (allowPreserveSave_ && isPreserveMode);
            saveAllowed_ = active_ && !aborted_ && requested &&
                !allowedSlot_.empty() && !isLoading && modeOk &&
                sameStem(stem, std::string_view(allowedSlot_));
            return saveAllowed_;
        }

        // 코세이브 쓰기를 거부해야 하면 true. 허용 토큰은 1회만 소비한다.
        [[nodiscard]] bool ShouldRefuseCoSave() noexcept
        {
            if (aborted_) {
                saveAllowed_ = false;
                return true;
            }
            if (!active_) {
                return false;
            }
            return !std::exchange(saveAllowed_, false);
        }

        [[nodiscard]] bool IsActive() const noexcept { return active_; }
        [[nodiscard]] bool IsAborted() const noexcept { return aborted_; }
        [[nodiscard]] bool AllowsPreserveSave() const noexcept { return allowPreserveSave_; }
        [[nodiscard]] std::string_view AllowedSlot() const noexcept { return allowedSlot_; }
        [[nodiscard]] bool HasPendingSaveAllowance() const noexcept { return saveAllowed_; }

    private:
        bool active_ = false;
        bool aborted_ = false;
        bool lockedForQuit_ = false;
        bool allowPreserveSave_ = false;
        std::string allowedSlot_;
        bool saveAllowed_ = false;
    };
}
