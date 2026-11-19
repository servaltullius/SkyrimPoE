#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

namespace SkyrimPoE::Core
{
    // 결정 119, 137(나): 주 갱신 한 스레드에서 FIFO로 예약을 처리한다.
    // - 프레임 시작 시 꺼낸 예약만 실행하고, 실행 중 새로 들어온 예약은 다음 프레임으로 넘긴다.
    // - 로드 세대(generation)가 다른 예약은 실행 직전에 버린다.
    // - 잠금은 큐에서 꺼내기/넣기만 짧게 잡고 실행은 잠금 밖에서 한다(결정 118, 137(다)③).
    class CommandQueue
    {
    public:
        using Clock = std::chrono::steady_clock;
        using TaskFn = std::function<void()>;

        struct Stats
        {
            std::size_t executed = 0;
            std::size_t droppedStale = 0;
            std::size_t deferredByBudget = 0;
            std::size_t failed = 0;
        };

        [[nodiscard]] std::uint64_t CurrentGeneration() const noexcept
        {
            return generation_.load(std::memory_order_acquire);
        }

        // 결정 137(라)①: 되돌리기(kRevert) 때 로드 세대를 +1하고 대기 예약을 전부 비운다.
        std::uint64_t BumpGenerationAndClear();

        // 현재 세대로 예약한다.
        void Enqueue(TaskFn fn);

        // 명시된 세대로 예약한다(외부 스레드에서 캡처한 세대 보존용).
        void EnqueueForGeneration(std::uint64_t generation, TaskFn fn);

        // 주 갱신 스레드에서 프레임당 1회 호출한다.
        // budgetMs <= 0이면 프레임 시작 시점 스냅샷을 모두 실행한다.
        // budgetMs > 0이면 최소 1건 실행 후 예산을 넘기면 남은 스냅샷을 다음 프레임 맨 앞으로 되돌린다.
        Stats DrainFrame(double budgetMs);

        [[nodiscard]] std::size_t PendingCount() const;

    private:
        struct Entry
        {
            std::uint64_t generation = 0;
            TaskFn fn;
        };

        std::atomic<std::uint64_t> generation_{ 1 };
        mutable std::mutex mutex_;
        std::deque<Entry> queue_;
    };
}
