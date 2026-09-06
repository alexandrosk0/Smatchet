// SideBySide2GridScenario — two visible grid panes rendering simultaneously.
// Multi-grid-tabs Slice 5b (plan docs/plans/multi-grid-tabs.md § Risks: "3-4
// grids drawing simultaneously is ~N x the single-grid draw" + § Performance).
// Bootstraps a SECOND open GridPane beside the bootstrap pane (cloning its
// backend / view so it renders the same offline data set), spins a live
// GridLiveContext for each via EnsurePaneContextLive (deterministic + offline —
// no backend swap / network is forced), then runs N frames as a passive
// observer of the per-frame UI budget. The host's drawGridPaneWindows loop now
// renders TWO pane windows per frame, so the existing `pane.render` /
// `drawActiveProjectWindow` perf scopes capture the N-grids-per-frame cost the
// plan flagged. The synthetic pane is removed on finish / cancel so the user's
// persisted pane set is untouched. Mirrors the passive-observer shape of
// attachment-preview-open / idle.

#include "Commands/Scenarios/IScenario.h"
#include "Ui/UiPerfRowsJson.h"

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
const char* const kPerfSecondPaneId = "perf-side-by-side-2";
} // namespace

class SideBySide2GridScenario : public IScenario {
  public:
    std::string Name() const override { return "side-by-side-2-grid"; }

    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = (std::max)(1, args.value("frames", 600));
        addedPane_ = false;
        maxFrameTopMs_ = 0.0;

        // Capture the bootstrap pane's identity BEFORE any push_back can
        // reallocate the vector and dangle the reference (LoadPanesOrBootstrap
        // guarantees >= 1 pane; focusedPane() self-heals if not).
        const GridPane& base = g_ui.focusedPane();
        const std::string basePaneId = base.id;
        const std::string baseBackendKey = base.backendKey;
        const std::string baseViewId = base.viewId;
        const std::string baseTitle = base.title;

        // Select the SECOND pane. Branch order avoids indexing a size-1 vector.
        // First: reclaim a leftover perf pane (a prior run crashed before cleanup),
        // owned and removed on finish. Next: when the user already runs two or more
        // panes, observe their second pane. Otherwise: add a synthetic second pane
        // cloning the base's backend and view so it renders the same offline rows.
        std::string secondPaneId;
        std::string secondBackendKey;
        if (const GridPane* leftover = FindGridPaneById(g_ui.gridPanes, kPerfSecondPaneId)) {
            secondPaneId = leftover->id;
            secondBackendKey = leftover->backendKey;
            addedPane_ = true; // reclaim ownership → RemoveSyntheticPane erases it on finish
        } else if (g_ui.gridPanes.size() >= 2) {
            // The base pane is the FOCUSED one, which is not necessarily index 0 — pick the
            // first pane that is not the base so the two grids are genuinely distinct (review
            // CR-1013: a hardcoded index 1 could re-select the focused base pane).
            for (size_t i = 0; i < g_ui.gridPanes.size(); ++i) {
                if (g_ui.gridPanes[i].id != basePaneId) {
                    secondPaneId = g_ui.gridPanes[i].id;
                    secondBackendKey = g_ui.gridPanes[i].backendKey;
                    break;
                }
            }
        } else {
            GridPane second;
            second.id = kPerfSecondPaneId;
            second.title = baseTitle + " #2";
            second.backendKey = baseBackendKey;
            second.viewId = baseViewId;
            second.open = true;
            g_ui.gridPanes.push_back(second);
            addedPane_ = true;
            secondPaneId = kPerfSecondPaneId;
            secondBackendKey = baseBackendKey;
        }

        // Spin a live context for both panes so two contexts tick simultaneously
        // (mirrors the host's per-visible-pane lifecycle). Idempotent.
        app.EnsurePaneContextLive(basePaneId, baseBackendKey);
        app.EnsurePaneContextLive(secondPaneId, secondBackendKey);

        // SMATCHET_DEVIATION(rule=duplication; reason=the 2-grid and N-grid side-by-side scenarios share their pane-setup/OnFrame/OnFinish framing by design (same measurement, different pane counts); the clone re-entered the delta scan when the shared rows serializer moved to UiPerfRowsJson.h; owner=scenarios; revisit=2027-03-01)
        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {
        // Passive observer: sample the dominant per-frame UI scope so a one-frame
        // spike from the two-grid draw is captured. LastFrameTopTotalMs reads the
        // prior frame's totals (aggregated by UiPerfMonitor::BeginFrame).
        const double topMs = UiPerfMonitor::Instance().LastFrameTopTotalMs();
        if (topMs > maxFrameTopMs_)
            maxFrameTopMs_ = topMs;
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    void OnCancel(IAppScenarioHost& /*app*/) override { RemoveSyntheticPane(); }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        RemoveSyntheticPane();

        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = UiPerfRowsToJson(rows);
        nlohmann::json out;
        out["scenario"] = "side-by-side-2-grid";
        out["frames"] = frames_;
        out["maxFrameTopMs"] = maxFrameTopMs_;
        out["target_p99_ms"] = 10.0; // 100 Hz floor
        out["ok_p99"] = maxFrameTopMs_ <= 10.0;
        out["rows"] = std::move(rowsJson);
        out["note"] = "Two grid panes drawing each frame (host drawGridPaneWindows loop renders "
                      "both). Measures the N-grids-per-frame draw cost via the `pane.render` / "
                      "`drawActiveProjectWindow` scopes. Offline + deterministic: no backend swap "
                      "or network sync is forced — CI runs have no tracker creds, so both grids "
                      "render their (empty / cached) projection. The structural per-pane render "
                      "work is the measured cost.";
        return out;
    }

  private:
    void RemoveSyntheticPane() {
        // Erase the scenario-installed pane so the user's persisted pane set is
        // untouched. The lingering GridLiveContext self-retires (hidden grace
        // window) via TickAllContexts. No-op when the pane was never added.
        if (!addedPane_)
            return;
        addedPane_ = false;
        for (std::size_t i = 0; i < g_ui.gridPanes.size(); ++i) {
            if (g_ui.gridPanes[i].id == kPerfSecondPaneId) {
                g_ui.gridPanes.erase(g_ui.gridPanes.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }

    int frames_ = 600;
    bool addedPane_ = false;
    double maxFrameTopMs_ = 0.0;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeSideBySide2GridScenario() {
    return std::make_unique<smatchet::cmd::SideBySide2GridScenario>();
}
