// ConcurrentSyncScenario — two live GridLiveContexts ticking under one shared
// TickAllContexts deadline. Multi-grid-tabs Slice 5b (plan
// docs/plans/multi-grid-tabs.md § Shared-budget hazard: "N panes posting sync
// applies could stack on one UI frame ... route through the existing drain-time
// budget + round-robin; gated by the concurrent-sync scenario").
// Spins TWO live contexts via EnsurePaneContextLive (deterministic + offline —
// no backend swap or network sync is kicked, so CI runs without tracker creds
// are reproducible), then runs N frames. The host's per-frame
// AppController::TickAllContexts rotates its drain start order across both
// contexts under the shared 4 ms deadline, so the `AppController::TickAllContexts`
// perf scope captures the multi-context dispatcher cost the plan flagged
// (§ Performance / item 18). The second context is re-stamped visible each frame
// so it stays live (not retired by the hidden-grace sweep) for the whole run.
// Passive observer otherwise — mirrors idle / attachment-preview-open.

#include "Commands/Scenarios/IScenario.h"

#include "Interfaces/IAppScenarioHost.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "SmatchetUiSession.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <string>

extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {
const char* const kSecondContextPaneId = "perf-concurrent-sync-2";
} // namespace

class ConcurrentSyncScenario : public IScenario {
  public:
    std::string Name() const override { return "concurrent-sync"; }

    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = (std::max)(1, args.value("frames", 600));
        maxFrameTopMs_ = 0.0;

        const GridPane& base = g_ui.focusedPane();
        basePaneId_ = base.id;
        backendKey_ = base.backendKey;

        // Spin the focused pane's context plus a SECOND live context so
        // TickAllContexts always rotates its drain across two contexts. No
        // g_ui pane is added — this isolates the dispatcher-drain cost from the
        // render cost (side-by-side-2-grid covers the render path).
        app.EnsurePaneContextLive(basePaneId_, backendKey_);
        app.EnsurePaneContextLive(kSecondContextPaneId, backendKey_);

        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(IAppScenarioHost& app, int /*frameIndex*/) override {
        // Keep the second context warm (re-stamp visible) so the hidden-grace
        // sweep in TickAllContexts does not retire it mid-run — both contexts
        // must stay live for the whole rotation measurement.
        app.EnsurePaneContextLive(kSecondContextPaneId, backendKey_);

        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
        double topMs = 0.0;
        for (const UiPerfRow& r : rows) {
            if (r.lastTotalMs > topMs)
                topMs = r.lastTotalMs;
        }
        if (topMs > maxFrameTopMs_)
            maxFrameTopMs_ = topMs;
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        // The second context (not focused, not default) self-retires after the
        // hidden-grace window once this scenario stops stamping it visible.
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        double tickAllMs = 0.0;
        nlohmann::json rowsJson = nlohmann::json::array();
        for (const UiPerfRow& r : rows) {
            if (r.name == "AppController::TickAllContexts")
                tickAllMs = r.lastTotalMs;
            rowsJson.push_back({
                {"name", r.name},
                {"lastTotalMs", r.lastTotalMs},
                {"avgPerCallMs", r.avgPerCallMs},
                {"maxMs", r.maxMs},
                {"calls", r.calls},
                {"emaAvgMs", r.emaAvgMs},
                {"p99Ms", r.p99Ms},
            });
        }
        nlohmann::json out;
        out["scenario"] = "concurrent-sync";
        out["frames"] = frames_;
        out["tickAllContextsLastTotalMs"] = tickAllMs;
        out["maxFrameTopMs"] = maxFrameTopMs_;
        out["target_p99_ms"] = 10.0; // 100 Hz floor
        out["ok_p99"] = maxFrameTopMs_ <= 10.0;
        out["rows"] = std::move(rowsJson);
        out["note"] = "Two live GridLiveContexts ticking under TickAllContexts' shared 4 ms "
                      "deadline + rotating drain. Measures the multi-context dispatcher-budget "
                      "path (§ Shared-budget hazard). Offline + deterministic: no live network "
                      "sync is kicked, so the measured cost is the two-context drain-loop + "
                      "rotation + hidden-grace sweep, not a real fetch/apply.";
        return out;
    }

  private:
    int frames_ = 600;
    double maxFrameTopMs_ = 0.0;
    std::string basePaneId_;
    std::string backendKey_;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeConcurrentSyncScenario() {
    return std::make_unique<smatchet::cmd::ConcurrentSyncScenario>();
}
