#include "Measurement.h"

#include <cassert>
#include <latch>
#include <sstream>
#include <thread>

using namespace Probe;

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }

    Measurement concurrent;
    constexpr std::uint32_t mainThread = 41;
    constexpr std::uint32_t threads = 10;
    constexpr int callsPerThread = 1000;
    concurrent.Configure(nullptr, nullptr, { true, true, true, true }, Candidate::A);
    concurrent.SetMainThread(mainThread);
    concurrent.Start({ .paused = 0, .freeze = 0 });
    concurrent.Record(Candidate::A, mainThread);
    concurrent.Record(Candidate::D, mainThread);
    for (std::uint32_t index = 0; index < threads; ++index) {
        concurrent.Record(Candidate::C, mainThread + index);
    }

    std::ostringstream log;
    std::ostringstream tsv;
    concurrent.Flush(log, tsv, true);
    assert(log.str().find("thread=41") != std::string::npos);

    std::latch start(threads + 1);
    std::vector<std::thread> workers;
    for (std::uint32_t index = 0; index < threads; ++index) {
        workers.emplace_back([&, index] {
            start.arrive_and_wait();
            for (int call = 0; call < callsPerThread; ++call) {
                concurrent.Record(Candidate::C, mainThread + index);
            }
        });
    }
    start.arrive_and_wait();
    for (int interval = 0; interval < 50; ++interval) {
        concurrent.Flush(log, tsv, true);
    }
    for (auto& worker : workers) {
        worker.join();
    }
    concurrent.Finish(RunResult::Done);
    concurrent.Record(Candidate::C, mainThread);
    concurrent.Flush(log, tsv, false);
    concurrent.Summarize(log);
    assert(log.str().find("Baseline C: 10010 / -1 / -1 / ") != std::string::npos);
    assert(log.str().find("+ / 10.00%") != std::string::npos);
    assert(!concurrent.Active());

    // Write the actual C++ schema for the analyzer's normal file entry point.
    Measurement fixture;
    auto frames = (std::numeric_limits<std::uint32_t>::max)() - 1;
    auto updates = frames;
    fixture.Configure(&frames, &updates, { true, true, true, true }, Candidate::A);
    fixture.SetMainThread(mainThread);
    std::ofstream report(argv[1], std::ios::trunc);
    assert(report);
    Measurement::WriteHeader(report);
    constexpr Context playing{ .paused = 0, .freeze = 0 };
    constexpr std::array contexts{ playing, Context{ .menus = Menu::Journal, .paused = 1, .freeze = 0 }, playing,
        Context{ .menus = Menu::Inventory, .paused = 1, .freeze = 0 }, playing,
        Context{ .menus = Menu::Loading, .paused = 1, .freeze = 0, .loadActive = true }, playing };
    fixture.Start(playing);
    for (std::size_t phase = 0; phase < kPhases; ++phase) {
        for (int call = 0; call < 4; ++call) {
            fixture.Record(Candidate::A, mainThread);
            fixture.Record(Candidate::C, mainThread);
            ++frames;
            ++updates;
        }
        fixture.Record(Candidate::D, mainThread);
        if (phase == static_cast<std::size_t>(Phase::EngineWrite)) {
            fixture.SetWriteResult(WriteResult::Passed);
        }
        if (phase + 1 < contexts.size()) {
            fixture.Next(static_cast<Phase>(phase + 1), contexts[phase + 1]);
        }
    }
    fixture.Finish(RunResult::Done);
    fixture.Flush(log, report, false);
    assert(report);
}
