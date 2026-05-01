#pragma once

#include "UiPerfMonitor.h"

#include <string>
#include <unordered_map>
#include <vector>

/// ImGui window: FPS, CPU scope timings (`UiPerfMonitor`), and `NetworkUsageTracker` stats.
class SmatchetPerfUi {
  public:
    void DrawWindow(bool* pOpen);
    /// Reads `io.Framerate` / delta; call after `DrawWindow` while the performance UI is open.
    void DrawFpsOverlay();

  private:
    void updateSmoothedFps();
    struct CpuSmoothBucket {
        double lastTotalMs = 0.0;
        double avgPerCallMs = 0.0;
        double emaAvgMs = 0.0;
        double maxMs = 0.0;
        bool hasValue = false;
    };

    /// Per-scope EMA so the table is readable; per-frame `calls` / `lifetimeHits` come from the monitor.
    void buildSmoothedCpuRows(const std::vector<UiPerfRow>& raw, std::vector<UiPerfRow>& out, double dtSec);

    std::unordered_map<std::string, CpuSmoothBucket> cpuSmooth_;
    double smoothFps_ = 0.0;
    double smoothFrameMs_ = 0.0;
    bool fpsSmoothInit_ = false;
};
