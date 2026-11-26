#ifndef UI_PERF_MONITOR_H
#define UI_PERF_MONITOR_H

#include "PerfSampleRing.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct UiPerfRow {
    std::string name;
    double lastTotalMs = 0.0;
    double avgPerCallMs = 0.0;
    double maxMs = 0.0;
    std::uint32_t calls = 0;
    /// Running total of `calls` across frames (each scope typically records once per frame).
    std::uint64_t lifetimeHits = 0;
    double emaAvgMs = 0.0;
    /// p99 of the last `PerfSampleRing::kCapacity` per-call durations (ms),
    /// spanning frames. Populated only by `GetLastFrameRows(true)` (snapshot
    /// serialization — cold path); stays 0.0 on the per-frame UI-panel path.
    double p99Ms = 0.0;
};

class UiPerfMonitor {
  public:
    static UiPerfMonitor& Instance();

    void BeginFrame();

    void Record(const char* name, std::chrono::nanoseconds duration);

    /// Rows aggregated by the most recent BeginFrame. With `includeP99` each
    /// row also gets `p99Ms` computed from its scope's sample ring (one
    /// nth_element per scope — snapshot/cold path only; the per-frame perf
    /// panel passes false and pays nothing).
    std::vector<UiPerfRow> GetLastFrameRows(bool includeP99 = false) const;

    /// Largest `lastTotalMs` across the rows of the most recent BeginFrame
    /// (0.0 when no rows). The perf scenarios' per-frame passive observer:
    /// they fold this into a running max so a one-frame spike is captured.
    double LastFrameTopTotalMs() const;

    /// Clear all accumulated measurements (last-frame rows, working accumulators, EMA history).
    /// Call before starting a benchmarking scenario so timings reflect only the run of interest.
    void Reset();

  private:
    UiPerfMonitor() = default;

    struct Agg {
        std::uint64_t totalNs = 0;
        std::uint32_t calls = 0;
        std::uint64_t maxNs = 0;
    };

    /// One record per scope name, persistent across frames (Reset clears).
    /// `frame` accumulates the current frame and is zeroed by BeginFrame;
    /// `ring` keeps the most recent per-call durations across frames so
    /// snapshot-time p99 has a real sample population (preallocated here —
    /// the per-call Record path never allocates for an existing scope).
    struct ScopeRecord {
        std::string name;
        Agg frame;
        PerfSampleRing ring;
    };

    mutable std::mutex mutex_;
    std::vector<ScopeRecord> scopes_;
    std::vector<UiPerfRow> lastFrame_;
    std::unordered_map<std::string, double> emaByName_;
    std::unordered_map<std::string, std::uint64_t> lifetimeHits_;
};

// name == nullptr: no timing recorded (cheap no-op; use for conditional scopes).
class UiPerfScope {
  public:
    explicit UiPerfScope(const char* name);
    ~UiPerfScope();

    UiPerfScope(const UiPerfScope&) = delete;
    UiPerfScope& operator=(const UiPerfScope&) = delete;

  private:
    const char* name_;
    std::chrono::steady_clock::time_point t0_;
};

#define SMATCHET_UI_PERF_SCOPE(name) UiPerfScope SMATCHET_UI_PERF_SCOPE_CAT(_ui_perf_, __LINE__)(name)
#define SMATCHET_UI_PERF_SCOPE_CAT(a, b) SMATCHET_UI_PERF_SCOPE_CAT2(a, b)
#define SMATCHET_UI_PERF_SCOPE_CAT2(a, b) a##b

#endif
