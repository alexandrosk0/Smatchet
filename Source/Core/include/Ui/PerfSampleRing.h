#ifndef PERF_SAMPLE_RING_H
#define PERF_SAMPLE_RING_H

#include <algorithm>
#include <cstddef>
#include <vector>

/// Fixed-capacity ring of recent per-call durations (in ms) for one perf scope.
/// Backs the snapshot-time `p99Ms` row field that arms Quality Pillar 1's
/// p99 <= 16.67 ms ceiling (`scripts/dev/perf-compare.py p99_abs_ceiling_ms`).
/// Hot-path contract: `Push` is one array store + one index increment — zero
/// steady-state allocation, no sorting, no branching beyond the wrap check.
/// The percentile itself is computed only at snapshot time (cold path) via
/// `ComputeP99` on a copy of the ring contents.
class PerfSampleRing {
  public:
    /// Ring capacity: 256 recent samples = 2 KB of doubles per scope. Large
    /// enough for a stable p99 (the rank-254 order statistic of a full ring)
    /// yet small enough that ~40 live scopes cost ~80 KB total.
    /// Enum (not `static const std::size_t`) so doctest CHECK macros — which
    /// bind operands by const reference — never ODR-use a member that has no
    /// out-of-line definition (IFNDR in C++14; CR-949-3).
    enum : std::size_t { kCapacity = 256 };

    /// Record one per-call duration. Overwrites the oldest sample once full.
    void Push(double ms) {
        samples_[head_] = ms;
        head_ = (head_ + 1 == kCapacity) ? 0 : head_ + 1;
        if (count_ < kCapacity) {
            ++count_;
        }
    }

    std::size_t Count() const { return count_; }

    /// Append the valid samples (unordered) to `out`. Cold path — may allocate
    /// in `out`; never called from the per-call recording path.
    void AppendSamples(std::vector<double>& out) const {
        // Slots [0, count_) are always the valid ones: before the first wrap
        // they are exactly the pushed samples; after wrapping count_ ==
        // kCapacity and every slot holds one of the most recent samples.
        for (std::size_t i = 0; i < count_; ++i) {
            out.push_back(samples_[i]);
        }
    }

    void Clear() {
        head_ = 0;
        count_ = 0;
    }

  private:
    double samples_[kCapacity] = {};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

/// p99 of `samples` (reordered in place by nth_element; empty -> 0.0).
/// Rank = ceil(0.99 * n); for small n this degrades toward the max, which is
/// the conservative (over-reporting) direction for a ceiling check —
/// perf-compare's `min_baseline_calls` already filters low-sample gating.
inline double ComputeP99(std::vector<double>& samples) {
    if (samples.empty()) {
        return 0.0;
    }
    const std::size_t n = samples.size();
    const std::size_t rank = (99 * n + 99) / 100; // ceil(0.99 * n) in integer math; rank in [1, n]
    std::nth_element(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(rank - 1), samples.end());
    return samples[rank - 1];
}

#endif
