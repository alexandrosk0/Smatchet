#ifndef UI_PERF_MONITOR_H
#define UI_PERF_MONITOR_H

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
};

class UiPerfMonitor {
  public:
    static UiPerfMonitor& Instance();

    void BeginFrame();

    void Record(const char* name, std::chrono::nanoseconds duration);

    std::vector<UiPerfRow> GetLastFrameRows() const;

  private:
    UiPerfMonitor() = default;

    struct Agg {
        std::uint64_t totalNs = 0;
        std::uint32_t calls = 0;
        std::uint64_t maxNs = 0;
    };

    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, Agg>> working_;
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






