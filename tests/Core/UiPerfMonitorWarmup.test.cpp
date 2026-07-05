// UiPerfMonitor warmup-frame exclusion — the snapshot p99 seam that arms
// Quality Pillar 1's absolute p99 ceiling (scripts/dev/perf-compare.py
// p99_abs_ceiling_ms). These cases pin the contract behind
// tooling.md `p99-gate-warmup-frame-exclusion` (postmortems.md
// 2026-06-07): the uniform runner-level UiPerfMonitor::Reset() that
// ScenarioRunner::Tick fires after WarmupFrames() frames must drop cold-start
// spikes from the ring that feeds ComputeP99, WITHOUT defanging the ceiling —
// a real steady-state regression pushed in AFTER the reset must still report a
// high p99. Exercises the production Record / Reset / GetLastFrameRows(true)
// path, not a reimplementation.

#include "UiPerfMonitor.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

namespace {

// One scope-call recording of `ms` milliseconds into the live monitor's ring.
void RecordMs(const char* name, double ms) {
    UiPerfMonitor::Instance().Record(
        name, std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(ms * 1e6)));
}

// p99Ms for `name` from a snapshot (cold path). Requires a BeginFrame to have
// rolled the scope into lastFrame_ so the row exists; returns -1.0 if absent.
double SnapshotP99(const char* name) {
    UiPerfMonitor::Instance().BeginFrame();
    const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
    for (const UiPerfRow& r : rows) {
        if (r.name == name) {
            return r.p99Ms;
        }
    }
    return -1.0;
}

} // namespace

TEST_CASE("UiPerfMonitor warmup: a one-time cold-start spike before Reset is excluded from p99") {
    UiPerfMonitor::Instance().Reset();
    const char* kScope = "warmup_test:scope";

    // WARMUP window: a single huge cold-start spike (font-atlas / first-frame /
    // initial-sync shape) plus a few quiet frames — exactly the population that
    // dominated cell-edit-burst's p99 (p99 ~12 ms, steady max
    // ~0.0002 ms). Because cell-edit-burst's ring held ONLY these, ComputeP99
    // returned the spike.
    RecordMs(kScope, 42.0); // cold-start spike (>> the 10 ms ceiling)
    for (int i = 0; i < 5; ++i) {
        RecordMs(kScope, 0.30); // a handful of warmup-window quiet frames
    }
    // Sanity: WITHOUT exclusion the spike is the p99 (tiny n degrades to max).
    CHECK(SnapshotP99(kScope) == doctest::Approx(42.0));

    // The runner's warmup-frame UiPerfMonitor::Reset() fires once after
    // WarmupFrames() frames — clearing the ring so the spike can never be
    // ranked again.
    UiPerfMonitor::Instance().Reset();

    // STEADY-STATE window: a full ring of quiet samples (saturate kCapacity so
    // ComputeP99 ranks a complete steady-state window, as cell-edit-burst now
    // does with warmup + steady frames).
    for (std::size_t i = 0; i < PerfSampleRing::kCapacity; ++i) {
        RecordMs(kScope, 0.40);
    }

    const double p99 = SnapshotP99(kScope);
    CHECK(p99 == doctest::Approx(0.40)); // steady-state only — the 42 ms spike is gone
    CHECK(p99 < 10.0);                   // within the Pillar-1 absolute ceiling
}

TEST_CASE("UiPerfMonitor warmup: a REAL steady-state regression after Reset still trips the ceiling") {
    // The anti-defang guard. Warmup exclusion must NOT swallow a genuine
    // steady-state regression: if the post-reset population is itself above the
    // ceiling, ComputeP99 must still report it so perf-compare.py red-gates.
    UiPerfMonitor::Instance().Reset();
    const char* kScope = "warmup_test:regressed";

    // Warmup window (whatever it was) is cleared by the runner's reset.
    RecordMs(kScope, 3.0);
    UiPerfMonitor::Instance().Reset();

    // STEADY-STATE window is uniformly ABOVE the 10 ms ceiling — a real
    // regression, not a lone cold-start outlier. Fill the whole ring so the
    // p99 rank lands squarely in the regressed mass.
    for (std::size_t i = 0; i < PerfSampleRing::kCapacity; ++i) {
        RecordMs(kScope, 12.5);
    }

    const double p99 = SnapshotP99(kScope);
    CHECK(p99 == doctest::Approx(12.5));
    CHECK(p99 > 10.0); // ceiling still trips — warmup exclusion did not defang the gate
}

TEST_CASE("UiPerfMonitor warmup: the first-N warmup samples are absent from the post-reset ring") {
    // Lower-level assertion that Reset() removes the warmup samples from the
    // population entirely (not merely outranks them). After the reset the ring
    // for the scope must contain ONLY the post-reset samples.
    UiPerfMonitor::Instance().Reset();
    const char* kScope = "warmup_test:ring_pop";

    // N warmup samples (mix of a spike and quiet) pushed pre-reset.
    const int kWarmupN = 7;
    RecordMs(kScope, 99.0);
    for (int i = 1; i < kWarmupN; ++i) {
        RecordMs(kScope, 0.10);
    }

    UiPerfMonitor::Instance().Reset();

    // A small, known steady population post-reset. With n=3 the p99 rank
    // degrades toward the max (ceil(0.99*3)=3) — that max is 0.5, NOT the 99 ms
    // warmup spike, proving the warmup samples are gone from the population.
    RecordMs(kScope, 0.3);
    RecordMs(kScope, 0.4);
    RecordMs(kScope, 0.5);

    const double p99 = SnapshotP99(kScope);
    CHECK(p99 == doctest::Approx(0.5)); // max of the 3 post-reset samples only
    CHECK(p99 < 1.0);                   // the 99 ms warmup spike never re-enters the ring

    UiPerfMonitor::Instance().Reset(); // leave the singleton clean for sibling cases
}
