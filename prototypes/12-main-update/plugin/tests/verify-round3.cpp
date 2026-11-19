#include "Measurement.h"

#include <cassert>
#include <sstream>

using namespace Probe;

constexpr std::array allInstalled{ true, true, true, true };
constexpr auto allObserved = CandidateBit(Candidate::A) | CandidateBit(Candidate::B) | CandidateBit(Candidate::C);
static_assert(SelectWriteCandidate(allInstalled, allObserved) == Candidate::A);
static_assert(SelectWriteCandidate(allInstalled, CandidateBit(Candidate::B) | CandidateBit(Candidate::C)) == Candidate::C);
static_assert(SelectWriteCandidate(allInstalled, CandidateBit(Candidate::B)) == Candidate::B);
static_assert(SelectWriteCandidate({ false, true, true, true }, allObserved) == Candidate::C);
static_assert(SelectWriteCandidate({ true, false, false, true }, CandidateBit(Candidate::C)) == Candidate::Count);
static_assert(SelectWriteCandidate(allInstalled, CandidateBit(Candidate::D)) == Candidate::Count);
static_assert(SelectWriteCandidate(allInstalled, 0) == Candidate::Count);

int main()
{
    constexpr Context playing{ .paused = 0, .freeze = 0 };
    constexpr Context journal{ .menus = Menu::Journal, .paused = 1, .freeze = 0 };
    constexpr Context loading{ .menus = Menu::Loading, .paused = 1, .freeze = 0 };
    constexpr Context loadActive{ .paused = 0, .freeze = 0, .loadActive = true };

    std::uint32_t updates = 0;
    Measurement measurement;
    measurement.Configure(nullptr, &updates, allInstalled);
    measurement.Start(playing);
    updates += 10;
    measurement.Next(Phase::Journal, journal);
    updates += 5;
    measurement.SetContext(playing);
    updates += 12;
    measurement.Next(Phase::EngineWrite, playing);
    measurement.SetContext(loadActive);
    updates += 3;
    measurement.SetContext(playing);
    measurement.SetWriteResult(WriteResult::Passed, Candidate::C);
    measurement.Next(Phase::Reload, loading);
    measurement.SetContext(loadActive);
    measurement.SetContext(playing);
    updates += 20;
    measurement.Finish(RunResult::Done);

    std::ostringstream log;
    std::ostringstream tsv;
    Measurement::WriteHeader(tsv);
    measurement.Flush(log, tsv, false);
    measurement.Summarize(log);
    const auto summary = log.str();
    assert(summary.find("Baseline | paused menu not observed | load not observed") != std::string::npos);
    assert(summary.find("Journal | paused menu yes (+5) | load not observed") != std::string::npos);
    assert(summary.find("EngineWrite | paused menu not observed | load yes (+3)") != std::string::npos);
    assert(summary.find("Reload | paused menu not observed | load no (+0)") != std::string::npos);

    std::istringstream rows(tsv.str());
    std::string row;
    int passed = 0;
    while (std::getline(rows, row)) {
        if (row.ends_with("\tpassed\t-")) {
            assert(row.find("\tEngineWrite\tC\t") != std::string::npos);
            ++passed;
        }
    }
    assert(passed == 1);

    Measurement unavailable;
    unavailable.Configure(nullptr, nullptr, allInstalled);
    unavailable.Start(playing);
    unavailable.Next(Phase::Reload, loading);
    unavailable.Finish(RunResult::Done);
    unavailable.Flush(log, tsv, false);
    unavailable.Summarize(log);
    assert(log.str().find("Reload | paused menu not observed | load counter unavailable") != std::string::npos);
}
