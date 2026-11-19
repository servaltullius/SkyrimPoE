#include "Measurement.h"

#include <algorithm>
#include <format>
#include <thread>
#include <utility>

namespace Probe
{
    namespace
    {
        constexpr std::array kPhaseResults{ "-", "complete", "menu_not_opened", "skipped", "aborted" };
        constexpr std::array kWriteResults{ "-", "passed", "failed", "not_run" };
        constexpr std::array kRunResults{ "-", "done", "aborted" };

        void StoreMin(std::atomic<Tick>& field, Tick value, Tick empty) noexcept
        {
            auto current = field.load(std::memory_order_relaxed);
            while ((current == empty || value < current) &&
                   !field.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
            }
        }

        void StoreMax(std::atomic<Tick>& field, Tick value) noexcept
        {
            auto current = field.load(std::memory_order_relaxed);
            while (value > current && !field.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
            }
        }

        double Milliseconds(Tick ticks)
        {
            return std::chrono::duration<double, std::milli>(Clock::duration(ticks)).count();
        }

        std::string MenuNames(unsigned flags)
        {
            std::string names;
            for (std::size_t i = 0; i < kMenus.size(); ++i) {
                if ((flags & (1u << i)) != 0) {
                    if (!names.empty()) {
                        names += ',';
                    }
                    names += kMenus[i];
                }
            }
            return names.empty() ? "-" : names;
        }

        template <class Range>
        std::string ThreadNames(const Range& threads)
        {
            std::string names;
            for (const auto id : threads) {
                if (id != 0) {
                    if (!names.empty()) {
                        names += ',';
                    }
                    names += std::to_string(id);
                }
            }
            return names.empty() ? "-" : names;
        }
    }

    Tick Now() noexcept
    {
        return Clock::now().time_since_epoch().count();
    }

    std::uint64_t Measurement::AtomicStats::Add(Tick& time, std::uint32_t thread, std::uint32_t mainThread) noexcept
    {
        const auto index = calls.fetch_add(1, std::memory_order_relaxed);
        if (thread == mainThread) {
            mainCalls.fetch_add(1, std::memory_order_relaxed);
        } else if (mainThread == 0) {
            unknownMain.fetch_add(1, std::memory_order_relaxed);
        }
        // Take the timestamp again on contention so every observation contributes
        // a gap in the order in which its timestamp was published.
        auto previous = last.load(std::memory_order_relaxed);
        do {
            time = std::max(Now(), previous);
        } while (!last.compare_exchange_weak(previous, time, std::memory_order_relaxed));
        if (previous == 0) {
            first.store(time, std::memory_order_relaxed);
        } else {
            StoreMin(minGap, time - previous, kNoGap);
            StoreMax(maxGap, time - previous);
        }
        for (auto& slot : threads) {
            auto known = slot.load(std::memory_order_relaxed);
            if (known == thread || (known == 0 &&
                    (slot.compare_exchange_strong(known, thread, std::memory_order_relaxed) || known == thread))) {
                return index;
            }
        }
        overflow.store(true, std::memory_order_relaxed);
        return index;
    }

    Measurement::Stats Measurement::AtomicStats::Drain() noexcept
    {
        Stats result;
        result.calls = calls.exchange(0, std::memory_order_relaxed);
        result.mainCalls = mainCalls.exchange(0, std::memory_order_relaxed);
        result.unknownMain = unknownMain.exchange(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < threads.size(); ++i) {
            result.threads[i] = threads[i].exchange(0, std::memory_order_relaxed);
        }
        result.overflow = overflow.exchange(false, std::memory_order_relaxed);
        result.first = first.exchange(0, std::memory_order_relaxed);
        result.last = last.exchange(0, std::memory_order_relaxed);
        result.minGap = minGap.exchange(kNoGap, std::memory_order_relaxed);
        result.maxGap = maxGap.exchange(0, std::memory_order_relaxed);
        return result;
    }

    void Measurement::Record(Candidate candidate, std::uint32_t thread) noexcept
    {
        const auto index = static_cast<std::size_t>(candidate);
        while (const auto epoch = epoch_.load()) {
            auto& buffer = buffers_[epoch % buffers_.size()];
            buffer.writers.fetch_add(1);
            // A writer that saw the old epoch may enter late. Only writers still in the
            // published epoch can touch its data, so draining and recycling remain safe.
            if (epoch == epoch_.load()) {
                Tick time;
                const auto call = buffer.stats[index].Add(time, thread, mainThread_.load(std::memory_order_relaxed));
                if (candidate == Candidate::D && call < buffer.tasks.size()) {
                    buffer.tasks[call] = { time, thread };
                }
                buffer.writers.fetch_sub(1);
                return;
            }
            buffer.writers.fetch_sub(1);
        }
    }

    void Measurement::Configure(const std::uint32_t* frames, const std::uint32_t* updates,
        const std::array<bool, kCandidates>& installed, Candidate writer)
    {
        frames_ = frames;
        updates_ = updates;
        installed_ = installed;
        writer_ = writer;
    }

    void Measurement::SetMainThread(std::uint32_t id) noexcept
    {
        mainThread_.store(id, std::memory_order_relaxed);
    }

    void Measurement::Start(Context context)
    {
        const std::scoped_lock lock(mutex_);
        if (result_ != RunResult::Running) {
            return;
        }
        context_ = context;
        origin_ = start_ = Now();
        startFrames_ = frames_ ? *frames_ : 0;
        startUpdates_ = updates_ ? *updates_ : 0;
        epoch_.store(++generation_);
    }

    void Measurement::Cut(PhaseResult result, bool stop)
    {
        const auto nextFrames = frames_ ? *frames_ : 0;
        const auto nextUpdates = updates_ ? *updates_ : 0;
        const auto time = Now();
        auto& buffer = buffers_[generation_ % buffers_.size()];
        epoch_.store(stop ? 0 : ++generation_);
        while (buffer.writers.load() != 0) {
            std::this_thread::yield();
        }

        Interval interval;
        interval.phase = phase_;
        interval.context = context_;
        interval.start = start_;
        interval.end = time;
        interval.frames = frames_ ? static_cast<std::uint32_t>(nextFrames - startFrames_) : -1LL;
        interval.updates = updates_ ? static_cast<std::uint32_t>(nextUpdates - startUpdates_) : -1LL;
        interval.phaseResult = result;
        interval.writer = writer_;
        interval.writeResult = writeResult_;
        interval.runResult = result_;
        for (std::size_t i = 0; i < kCandidates; ++i) {
            interval.stats[i] = buffer.stats[i].Drain();
        }
        const auto tasks = std::min<std::uint64_t>(interval.stats[static_cast<std::size_t>(Candidate::D)].calls,
            buffer.tasks.size());
        interval.tasks.assign(buffer.tasks.begin(), buffer.tasks.begin() + tasks);
        pending_.push_back(std::move(interval));
        start_ = time;
        startFrames_ = nextFrames;
        startUpdates_ = nextUpdates;
    }

    void Measurement::SetContext(Context context)
    {
        const std::scoped_lock lock(mutex_);
        if (Active() && context != context_) {
            Cut();
            context_ = context;
        }
    }

    void Measurement::Next(Phase phase, Context context, PhaseResult previous)
    {
        const std::scoped_lock lock(mutex_);
        if (Active()) {
            Cut(previous);
            phase_ = phase;
            context_ = context;
        }
    }

    void Measurement::Finish(RunResult result)
    {
        const std::scoped_lock lock(mutex_);
        if (result_ == RunResult::Running) {
            result_ = result;
            if (Active()) {
                Cut(result == RunResult::Done ? PhaseResult::Complete : PhaseResult::Aborted, true);
            }
        }
    }

    void Measurement::Note(std::string message)
    {
        const std::scoped_lock lock(mutex_);
        notes_.push_back(std::move(message));
    }

    void Measurement::SetWriteResult(WriteResult result, Candidate writer)
    {
        const std::scoped_lock lock(mutex_);
        if (writer != Candidate::Count) {
            writer_ = writer;
        }
        writeResult_ = result;
    }

    RunResult Measurement::Result() const
    {
        const std::scoped_lock lock(mutex_);
        return result_;
    }

    void Measurement::Flush(std::ostream& log, std::ostream& tsv, bool sample)
    {
        std::vector<Interval> intervals;
        std::vector<std::string> notes;
        {
            const std::scoped_lock lock(mutex_);
            if (sample && Active()) {
                Cut();
            }
            intervals.swap(pending_);
            notes.swap(notes_);
        }
        for (const auto& note : notes) {
            log << note << '\n';
        }
        for (const auto& interval : intervals) {
            Write(interval, log, tsv);
        }
        log.flush();
        tsv.flush();
    }

    void Measurement::WriteHeader(std::ostream& tsv)
    {
        tsv << "interval\tstart_ms\tend_ms\tphase\tcandidate\tinstalled\tcalls\tthreads\tthread_overflow\tmain_calls"
               "\tunknown_main_calls\tmin_gap_ms\tmax_gap_ms\tfirst_call_ms\tlast_call_ms\tframe_delta\tupdate_delta"
               "\tpaused\tfreeze\tmenus\tload_active\tphase_result\tengine_write\trun_result\n";
    }

    void Measurement::Write(const Interval& interval, std::ostream& log, std::ostream& tsv)
    {
        const auto number = ++intervalNumber_;
        const auto phase = static_cast<std::size_t>(interval.phase);
        const auto menus = MenuNames(interval.context.menus);
        const auto start = Milliseconds(interval.start - origin_);
        const auto end = Milliseconds(interval.end - origin_);
        auto& contextUpdates = contextUpdates_[phase];
        const auto addUpdates = [&](std::optional<std::int64_t>& total) {
            const auto previous = total.value_or(0);
            total = previous >= 0 && interval.updates >= 0 ? previous + interval.updates : -1;
        };
        if (interval.context.paused == 1 && (interval.context.menus & (Menu::Journal | Menu::Inventory | Menu::Console))) {
            addUpdates(contextUpdates.menu);
        }
        if (interval.context.loadActive || (interval.context.menus & Menu::Loading) != 0) {
            addUpdates(contextUpdates.load);
        }
        for (std::size_t i = 0; i < kCandidates; ++i) {
            const auto& stats = interval.stats[i];
            const auto threads = ThreadNames(stats.threads);
            const auto write = interval.phase == Phase::EngineWrite && static_cast<Candidate>(i) == interval.writer ?
                interval.writeResult : WriteResult::NotRun;
            tsv << std::format("{}\t{:.6f}\t{:.6f}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.6f}\t{:.6f}"
                               "\t{:.6f}\t{:.6f}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n",
                number, start, end, kPhaseNames[phase], kCandidateNames[i], int(installed_[i]), stats.calls, threads,
                int(stats.overflow), stats.mainCalls, stats.unknownMain,
                stats.minGap != kNoGap ? Milliseconds(stats.minGap) : -1.0,
                stats.minGap != kNoGap ? Milliseconds(stats.maxGap) : -1.0,
                stats.first ? Milliseconds(stats.first - origin_) : -1.0,
                stats.last ? Milliseconds(stats.last - origin_) : -1.0, interval.frames, interval.updates,
                interval.context.paused, interval.context.freeze, menus, int(interval.context.loadActive),
                kPhaseResults[static_cast<std::size_t>(interval.phaseResult)], kWriteResults[static_cast<std::size_t>(write)],
                kRunResults[static_cast<std::size_t>(interval.runResult)]);

            auto& total = totals_[phase][i];
            total.seen = true;
            total.calls += stats.calls;
            total.mainCalls += stats.mainCalls;
            total.unknownMain += stats.unknownMain;
            total.overflow |= stats.overflow;
            total.frames = total.frames >= 0 && interval.frames >= 0 ? total.frames + interval.frames : -1;
            total.updates = total.updates >= 0 && interval.updates >= 0 ? total.updates + interval.updates : -1;
            for (auto id : stats.threads) {
                if (id != 0) {
                    total.threads.insert(id);
                }
            }
        }
        log << std::format("[{:.3f}–{:.3f} ms] {}: calls A={} B={} C={} D={}, updates={}, frames={}, "
                           "paused={}, freeze={}, menus={}, loading={}, phase={}.\n",
            start, end, kPhaseNames[phase], interval.stats[0].calls, interval.stats[1].calls,
            interval.stats[2].calls, interval.stats[3].calls, interval.updates, interval.frames,
            interval.context.paused, interval.context.freeze, menus, interval.context.loadActive,
            kPhaseResults[static_cast<std::size_t>(interval.phaseResult)]);
        for (const auto& task : interval.tasks) {
            log << std::format("  D t={:.6f} ms thread={}\n", Milliseconds(task.time - origin_), task.thread);
        }
        const auto omitted = interval.stats[static_cast<std::size_t>(Candidate::D)].calls - interval.tasks.size();
        if (omitted != 0) {
            log << std::format("  D: {} call timestamps exceeded the interval capacity.\n", omitted);
        }
    }

    void Measurement::Summarize(std::ostream& log) const
    {
        log << "\nSummary (calls / updates / frames / distinct threads / Main thread)\n";
        for (std::size_t phase = 0; phase < kPhases; ++phase) {
            for (std::size_t i = 0; i < kCandidates; ++i) {
                const auto& total = totals_[phase][i];
                const auto threadCount = std::max(total.threads.size(), total.overflow ? kThreads + 1 : 0);
                const auto ratio = total.calls != 0 && total.unknownMain == 0 ?
                    std::format("{:.2f}%", 100.0 * total.mainCalls / total.calls) : "-";
                log << std::format("{} {}: {}\n", kPhaseNames[phase], kCandidateNames[i], total.seen ?
                    std::format("{} / {} / {} / {}{} / {}", total.calls, total.updates, total.frames, threadCount,
                        total.overflow ? "+" : "", ratio) : "no intervals");
            }
        }
        const auto describeUpdates = [](const std::optional<std::int64_t>& updates) -> std::string {
            if (!updates) {
                return "not observed";
            }
            if (*updates < 0) {
                return "counter unavailable";
            }
            return *updates == 0 ? "no (+0)" : std::format("yes (+{})", *updates);
        };
        log << "\nMain::Update by phase (gUpdateCounter delta)\n";
        for (std::size_t phase = 0; phase < kPhases; ++phase) {
            const auto& updates = contextUpdates_[phase];
            log << std::format("{} | paused menu {} | load {}\n", kPhaseNames[phase],
                describeUpdates(updates.menu), describeUpdates(updates.load));
        }
        log << "Run result: " << kRunResults[static_cast<std::size_t>(Result())] << '\n';
        log.flush();
    }
}
