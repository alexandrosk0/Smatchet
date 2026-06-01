// BackgroundWorkerReap.test.cpp — Phase 4 of
// docs/plans/shipped/memory-budget-and-lifetime-hardening.md.
//
// Unit-tests the pure ReapFinishedWorkers algorithm that backs AppController's mid-session
// background-worker reaping, using a fake worker (no real std::thread / no AppController):
// reap finished non-self workers (and "join" them), keep running + self workers, order-preserving.

#include "BackgroundWorkerReap.h"

#include "doctest/doctest.h"

#include <vector>

namespace {
struct FakeWorker {
    bool done = false;   // task finished (the real done atomic)
    bool isSelf = false; // the reaping thread itself (never self-join)
    int* joins = nullptr;
    int id = 0;
};
auto Reapable = [](const FakeWorker& w) { return w.done && !w.isSelf; };
auto OnReap = [](FakeWorker& w) {
    if (w.joins) {
        ++(*w.joins);
    }
};
} // namespace

TEST_CASE("ReapFinishedWorkers reaps finished non-self workers, joins them, keeps the rest") {
    int joins = 0;
    std::vector<FakeWorker> v = {
        {true, false, &joins, 1},  // finished       -> reap + join
        {false, false, &joins, 2}, // still running   -> keep
        {true, true, &joins, 3},   // finished + self -> keep (never self-join)
        {true, false, &joins, 4},  // finished        -> reap + join
    };
    const std::size_t reaped = smatchet::ReapFinishedWorkers(v, Reapable, OnReap);
    CHECK(reaped == 2);
    CHECK(joins == 2); // only the reaped two were "joined"
    REQUIRE(v.size() == 2);
    CHECK(v[0].id == 2); // survivors keep order: running, then self
    CHECK(v[1].id == 3);
}

TEST_CASE("ReapFinishedWorkers is a no-op when nothing is reapable") {
    int joins = 0;
    std::vector<FakeWorker> v = {{false, false, &joins, 1}, {true, true, &joins, 2}};
    const std::size_t reaped = smatchet::ReapFinishedWorkers(v, Reapable, OnReap);
    CHECK(reaped == 0);
    CHECK(joins == 0);
    CHECK(v.size() == 2);
}

TEST_CASE("ReapFinishedWorkers empties a vector where every worker is reapable") {
    int joins = 0;
    std::vector<FakeWorker> v = {{true, false, &joins, 1}, {true, false, &joins, 2}};
    const std::size_t reaped =
        smatchet::ReapFinishedWorkers(v, [](const FakeWorker& w) { return w.done; }, OnReap);
    CHECK(reaped == 2);
    CHECK(joins == 2);
    CHECK(v.empty());
}
