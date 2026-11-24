#include "core/CommandQueue.h"

namespace SkyrimPoE::Core
{
    std::uint64_t CommandQueue::InvalidatePending()
    {
        std::scoped_lock lock(mutex_);
        const auto next = stateGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
        std::erase_if(queue_, [](const Entry& entry) { return entry.kind == Kind::StateReapply; });
        return next;
    }

    std::uint64_t CommandQueue::BumpGenerationAndClear()
    {
        std::scoped_lock lock(mutex_);
        trackingGeneration_.fetch_add(1, std::memory_order_acq_rel);
        const auto next = stateGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
        queue_.clear();
        return next;
    }

    void CommandQueue::Enqueue(TaskFn fn, Kind kind)
    {
        if (!fn) {
            return;
        }
        std::scoped_lock lock(mutex_);
        queue_.push_back(Entry{ CurrentGeneration(kind), kind, std::move(fn) });
    }

    void CommandQueue::EnqueueForGeneration(std::uint64_t generation, Kind kind, TaskFn fn)
    {
        if (!fn) {
            return;
        }
        std::scoped_lock lock(mutex_);
        queue_.push_back(Entry{ generation, kind, std::move(fn) });
    }

    CommandQueue::Stats CommandQueue::DrainFrame(double budgetMs)
    {
        Stats stats;
        std::deque<Entry> batch;
        {
            // 프레임 시작 시점에 들어 있는 예약만 통째로 꺼낸다.
            // 실행 도중 새로 Enqueue된 예약은 queue_에 쌓여 다음 프레임으로 넘어간다(결정 137(나)①).
            std::scoped_lock lock(mutex_);
            batch.swap(queue_);
        }

        const bool useBudget = budgetMs > 0.0;
        const auto start = Clock::now();
        std::size_t index = 0;

        for (; index < batch.size(); ++index) {
            auto& entry = batch[index];
            if (entry.generation != CurrentGeneration(entry.kind)) {
                ++stats.droppedStale;
                continue;
            }

            if (useBudget && stats.executed > 0) {
                const auto elapsedMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                if (elapsedMs >= budgetMs) {
                    break;
                }
            }

            if (entry.fn) {
                try {
                    entry.fn();
                    ++stats.executed;
                } catch (...) {
                    // 작업 1건의 예외가 같은 프레임 배치의 나머지 예약을 유실시키지 않도록 격리한다.
                    ++stats.failed;
                }
            }
        }

        if (index < batch.size()) {
            std::scoped_lock lock(mutex_);
            // 시간 예산 초과로 남은 현재 세대 예약은 FIFO 순서를 지키기 위해 새 예약 앞에 되돌린다.
            for (std::size_t rev = batch.size(); rev > index; --rev) {
                auto& leftover = batch[rev - 1];
                if (leftover.generation == CurrentGeneration(leftover.kind)) {
                    queue_.push_front(std::move(leftover));
                    ++stats.deferredByBudget;
                } else {
                    ++stats.droppedStale;
                }
            }
        }

        return stats;
    }

    std::size_t CommandQueue::PendingCount() const
    {
        std::scoped_lock lock(mutex_);
        return queue_.size();
    }
}
