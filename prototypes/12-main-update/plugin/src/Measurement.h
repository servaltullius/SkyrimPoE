#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Probe
{
    using Clock = std::chrono::steady_clock;
    using Tick = std::int64_t;
    Tick Now() noexcept;

    enum class Candidate { A, B, C, D, Count };
    enum class Phase { Baseline, Journal, AfterJournal, Inventory, EngineWrite, Reload, AfterReload, Count };

    inline constexpr auto kCandidates = static_cast<std::size_t>(Candidate::Count);
    inline constexpr auto kPhases = static_cast<std::size_t>(Phase::Count);
    inline constexpr std::array kCandidateNames{ "A", "B", "C", "D" };
    inline constexpr std::array kWritePriority{ Candidate::A, Candidate::C, Candidate::B };

    constexpr unsigned CandidateBit(Candidate candidate) noexcept
    {
        return 1u << static_cast<unsigned>(candidate);
    }

    constexpr Candidate SelectWriteCandidate(const std::array<bool, kCandidates>& installed, unsigned observed = ~0u)
    {
        for (auto candidate : kWritePriority) {
            if (installed[static_cast<std::size_t>(candidate)] && (observed & CandidateBit(candidate)) != 0) {
                return candidate;
            }
        }
        return Candidate::Count;
    }

    inline constexpr std::array kPhaseNames{ "Baseline", "Journal", "AfterJournal", "Inventory",
        "EngineWrite", "Reload", "AfterReload" };
    inline constexpr std::array<std::string_view, 5> kMenus{
        "Journal Menu", "InventoryMenu", "Loading Menu", "Console", "Main Menu" };
    enum Menu : unsigned { Journal = 1, Inventory = 2, Loading = 4, Console = 8, MainMenu = 16 };

    struct Context
    {
        unsigned menus = 0;
        int paused = -1;
        int freeze = -1;
        bool loadActive = false;
        bool operator==(const Context&) const = default;
    };

    enum class PhaseResult { Pending, Complete, MenuNotOpened, Skipped, Aborted };
    enum class WriteResult { Pending, Passed, Failed, NotRun };
    enum class RunResult { Running, Done, Aborted };

    class Measurement
    {
    public:
        void Configure(const std::uint32_t* frames, const std::uint32_t* updates,
            const std::array<bool, kCandidates>& installed, Candidate writer = Candidate::Count);
        void SetMainThread(std::uint32_t id) noexcept;
        void Start(Context context);
        void SetContext(Context context);
        void Next(Phase phase, Context context, PhaseResult previous = PhaseResult::Complete);
        void Finish(RunResult result);
        void Note(std::string message);
        void SetWriteResult(WriteResult result, Candidate writer = Candidate::Count);
        void Record(Candidate candidate, std::uint32_t thread) noexcept;
        [[nodiscard]] bool Active() const noexcept { return epoch_.load(std::memory_order_relaxed) != 0; }
        [[nodiscard]] RunResult Result() const;
        void Flush(std::ostream& log, std::ostream& tsv, bool sample);
        void Summarize(std::ostream& log) const;
        static void WriteHeader(std::ostream& tsv);

    private:
        static constexpr std::size_t kThreads = 8;
        static constexpr std::size_t kTaskSamples = 1024;
        static constexpr Tick kNoGap = (std::numeric_limits<Tick>::max)();

        struct TaskCall
        {
            Tick time = 0;
            std::uint32_t thread = 0;
        };

        struct Stats
        {
            std::uint64_t calls = 0;
            std::uint64_t mainCalls = 0;
            std::uint64_t unknownMain = 0;
            std::array<std::uint32_t, kThreads> threads{};
            bool overflow = false;
            Tick first = 0;
            Tick last = 0;
            Tick minGap = kNoGap;
            Tick maxGap = 0;
        };

        struct AtomicStats
        {
            std::atomic_uint64_t calls{ 0 }, mainCalls{ 0 }, unknownMain{ 0 };
            std::array<std::atomic_uint32_t, kThreads> threads{};
            std::atomic_bool overflow{ false };
            std::atomic<Tick> first{ 0 }, last{ 0 }, minGap{ kNoGap }, maxGap{ 0 };

            std::uint64_t Add(Tick& time, std::uint32_t thread, std::uint32_t mainThread) noexcept;
            Stats Drain() noexcept;
        };

        struct Buffer
        {
            std::atomic_uint32_t writers{ 0 };
            std::array<AtomicStats, kCandidates> stats;
            std::array<TaskCall, kTaskSamples> tasks;
        };

        struct Interval
        {
            Phase phase = Phase::Baseline;
            Context context;
            Tick start = 0;
            Tick end = 0;
            std::int64_t frames = -1;
            std::int64_t updates = -1;
            PhaseResult phaseResult = PhaseResult::Pending;
            Candidate writer = Candidate::Count;
            WriteResult writeResult = WriteResult::Pending;
            RunResult runResult = RunResult::Running;
            std::array<Stats, kCandidates> stats;
            std::vector<TaskCall> tasks;
        };

        struct Total
        {
            std::uint64_t calls = 0;
            std::uint64_t mainCalls = 0;
            std::uint64_t unknownMain = 0;
            std::int64_t frames = 0;
            std::int64_t updates = 0;
            bool seen = false;
            bool overflow = false;
            std::set<std::uint32_t> threads;
        };

        struct ContextUpdates
        {
            std::optional<std::int64_t> menu;
            std::optional<std::int64_t> load;
        };

        void Cut(PhaseResult result = PhaseResult::Pending, bool stop = false);
        void Write(const Interval& interval, std::ostream& log, std::ostream& tsv);

        std::atomic_uint64_t epoch_{ 0 };
        std::atomic_uint32_t mainThread_{ 0 };
        std::array<Buffer, 2> buffers_;
        mutable std::mutex mutex_;
        const volatile std::uint32_t* frames_ = nullptr;
        const volatile std::uint32_t* updates_ = nullptr;
        std::array<bool, kCandidates> installed_{};
        Candidate writer_ = Candidate::Count;
        Phase phase_ = Phase::Baseline;
        Context context_;
        std::uint64_t generation_ = 0;
        Tick origin_ = 0;
        Tick start_ = 0;
        std::uint32_t startFrames_ = 0;
        std::uint32_t startUpdates_ = 0;
        WriteResult writeResult_ = WriteResult::Pending;
        RunResult result_ = RunResult::Running;
        std::vector<Interval> pending_;
        std::vector<std::string> notes_;

        // Ticker owns the streams, totals and interval numbering.
        std::uint64_t intervalNumber_ = 0;
        std::array<std::array<Total, kCandidates>, kPhases> totals_;
        std::array<ContextUpdates, kPhases> contextUpdates_;
    };
}
