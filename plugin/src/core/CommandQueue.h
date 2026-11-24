#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace SkyrimPoE::Core
{
    // 결정 119, 137(나): 주 갱신 한 스레드에서 FIFO로 예약을 처리한다.
    // - 프레임 시작 시 꺼낸 예약만 실행하고, 실행 중 새로 들어온 예약은 다음 프레임으로 넘긴다.
    // - 현재 세대(generation)와 다른 예약은 실행 직전에 버린다.
    // - 잠금은 큐에서 꺼내기/넣기만 짧게 잡고 실행은 잠금 밖에서 한다(결정 118, 137(다)③).
    class CommandQueue
    {
    public:
        using Clock = std::chrono::steady_clock;
        using TaskFn = std::function<void()>;

        enum class Kind { StateReapply, Tracking };

        struct Stats
        {
            std::size_t executed = 0;
            std::size_t droppedStale = 0;
            std::size_t deferredByBudget = 0;
            std::size_t failed = 0;
        };

        [[nodiscard]] std::uint64_t CurrentGeneration(Kind kind = Kind::StateReapply) const noexcept
        {
            return (kind == Kind::Tracking ? trackingGeneration_ : stateGeneration_).load(std::memory_order_acquire);
        }

        // 안전 제거는 상태 재적용만 무효화한다. 추적 장부는 같은 세이브에서 계속 이어진다.
        std::uint64_t InvalidatePending();

        // 결정 137(라)①: 되돌리기(kRevert) 때 세대를 +1하고 대기 예약을 전부 비운다.
        std::uint64_t BumpGenerationAndClear();

        // 현재 세대로 예약한다.
        void Enqueue(TaskFn fn, Kind kind = Kind::StateReapply);

        // 명시된 세대로 예약한다(외부 스레드에서 캡처한 세대 보존용).
        void EnqueueForGeneration(std::uint64_t generation, TaskFn fn)
        {
            EnqueueForGeneration(generation, Kind::StateReapply, std::move(fn));
        }
        void EnqueueForGeneration(std::uint64_t generation, Kind kind, TaskFn fn);

        // 주 갱신 스레드에서 프레임당 1회 호출한다.
        // budgetMs <= 0이면 프레임 시작 시점 스냅샷을 모두 실행한다.
        // budgetMs > 0이면 최소 1건 실행 후 예산을 넘기면 남은 스냅샷을 다음 프레임 맨 앞으로 되돌린다.
        Stats DrainFrame(double budgetMs);

        [[nodiscard]] std::size_t PendingCount() const;

    private:
        struct Entry
        {
            std::uint64_t generation = 0;
            Kind kind = Kind::StateReapply;
            TaskFn fn;
        };

        std::atomic<std::uint64_t> stateGeneration_{ 1 };
        std::atomic<std::uint64_t> trackingGeneration_{ 1 };
        mutable std::mutex mutex_;
        std::deque<Entry> queue_;
    };
}
