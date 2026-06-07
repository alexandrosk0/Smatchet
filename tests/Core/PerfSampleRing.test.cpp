// PerfSampleRing + ComputeP99 — the pure seam behind UiPerfMonitor's
// snapshot-time `p99Ms` row field (arms Quality Pillar 1's p99 <= 16.67 ms
// ceiling read by scripts/dev/perf-compare.py). Header-only; no monitor or
// ImGui dependency.

#include "PerfSampleRing.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace {

std::vector<double> Samples(const PerfSampleRing& ring) {
    std::vector<double> out;
    ring.AppendSamples(out);
    return out;
}

} // namespace

TEST_CASE("PerfSampleRing: empty ring has no samples") {
    PerfSampleRing ring;
    CHECK(ring.Count() == 0);
    CHECK(Samples(ring).empty());
}

TEST_CASE("PerfSampleRing: fill below capacity keeps every sample") {
    PerfSampleRing ring;
    for (int i = 0; i < 10; ++i) {
        ring.Push(static_cast<double>(i));
    }
    CHECK(ring.Count() == 10);
    std::vector<double> got = Samples(ring);
    REQUIRE(got.size() == 10);
    for (int i = 0; i < 10; ++i) {
        CHECK(got[static_cast<std::size_t>(i)] == doctest::Approx(static_cast<double>(i)));
    }
}

TEST_CASE("PerfSampleRing: exact-capacity fill saturates without wrap") {
    PerfSampleRing ring;
    for (std::size_t i = 0; i < PerfSampleRing::kCapacity; ++i) {
        ring.Push(1.0);
    }
    CHECK(ring.Count() == PerfSampleRing::kCapacity);
    CHECK(Samples(ring).size() == PerfSampleRing::kCapacity);
}

TEST_CASE("PerfSampleRing: wrap-around overwrites the oldest samples") {
    PerfSampleRing ring;
    const std::size_t total = PerfSampleRing::kCapacity + 10;
    for (std::size_t i = 0; i < total; ++i) {
        ring.Push(static_cast<double>(i));
    }
    CHECK(ring.Count() == PerfSampleRing::kCapacity);
    std::vector<double> got = Samples(ring);
    REQUIRE(got.size() == PerfSampleRing::kCapacity);
    // Survivors must be exactly the most recent kCapacity values [10, total).
    std::sort(got.begin(), got.end());
    CHECK(got.front() == doctest::Approx(10.0));
    CHECK(got.back() == doctest::Approx(static_cast<double>(total - 1)));
    for (std::size_t i = 0; i < got.size(); ++i) {
        CHECK(got[i] == doctest::Approx(static_cast<double>(i + 10)));
    }
}

TEST_CASE("PerfSampleRing: Clear empties the ring") {
    PerfSampleRing ring;
    ring.Push(1.0);
    ring.Push(2.0);
    ring.Clear();
    CHECK(ring.Count() == 0);
    CHECK(Samples(ring).empty());
}

TEST_CASE("ComputeP99: empty input yields 0") {
    std::vector<double> v;
    CHECK(ComputeP99(v) == doctest::Approx(0.0));
}

TEST_CASE("ComputeP99: single sample yields that sample") {
    std::vector<double> v{3.25};
    CHECK(ComputeP99(v) == doctest::Approx(3.25));
}

TEST_CASE("ComputeP99: uniform 1..100 yields 99 (rank ceil(0.99*100))") {
    std::vector<double> v;
    for (int i = 100; i >= 1; --i) { // reverse order — nth_element must not care
        v.push_back(static_cast<double>(i));
    }
    CHECK(ComputeP99(v) == doctest::Approx(99.0));
}

TEST_CASE("ComputeP99: small n degrades toward max (conservative)") {
    // n = 10 -> rank ceil(9.9) = 10 -> the max. A spike among few samples
    // must be reported, not hidden.
    std::vector<double> v{1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 100.0};
    CHECK(ComputeP99(v) == doctest::Approx(100.0));
}

TEST_CASE("ComputeP99: full ring with a spike cluster lands inside the cluster") {
    // 250 quiet samples + 6 spikes in a 256-ring: rank ceil(0.99*256) = 254,
    // which falls within the 6 spike values (positions 251..256 when sorted).
    std::vector<double> v(250, 1.0);
    for (int i = 0; i < 6; ++i) {
        v.push_back(100.0);
    }
    CHECK(v.size() == 256);
    CHECK(ComputeP99(v) == doctest::Approx(100.0));
}

TEST_CASE("ComputeP99: lone spike among a full ring is excluded by the 99th rank") {
    // 255 quiet + 1 spike: rank 254 of 256 stays in the quiet mass — a single
    // outlier in 256 samples is below the p99 threshold by definition.
    std::vector<double> v(255, 2.0);
    v.push_back(500.0);
    CHECK(ComputeP99(v) == doctest::Approx(2.0));
}

TEST_CASE("Ring + p99 end-to-end: wrap-around evicts an old spike") {
    PerfSampleRing ring;
    ring.Push(1000.0); // ancient spike
    for (std::size_t i = 0; i < PerfSampleRing::kCapacity; ++i) {
        ring.Push(1.0); // a full capacity of quiet samples evicts it
    }
    std::vector<double> got = Samples(ring);
    REQUIRE(got.size() == PerfSampleRing::kCapacity);
    CHECK(ComputeP99(got) == doctest::Approx(1.0));
}
