// InteractiveGridStressScenario — ACTIVE-load variant of side-by-side-grids.
// Where SideBySideNGridScenario is a passive observer (clones the bootstrap
// pane's offline backend, no input), this one drives a worst-case interactive
// load to answer "does the 8-pane grid hold the 6.94 ms / 144 Hz budget while a
// user actually USES it": a MIXED backend set (50% Jira — 4 Jira + 2 GitHub +
// 2 Plane, each with its OWN live GridLiveContext so the 4 Jira panes force real
// network sync = the sync contention a passive run never sees), plus per-frame
// vertical + horizontal scroll on the focused pane, plus periodic pane-focus
// switching (each cross-backend switch pays the cfg.TrackerType rewrite + Save +
// ViewState reload the host comment at SmatchetGridPaneWindows.cpp:165 calls out),
// plus a handful of cross-backend view switches (Jira + GitHub). It is a pure
// INPUT INJECTOR — it does not self-measure wall-clock; real-GL FPS is captured
// by main.cpp's FpsMeasure (SMATCHET_FPS_MEASURE_SECONDS) on the live window, and
// the per-scope UI-CPU cost is captured by the host's existing pane.render /
// drawActiveProjectWindow SMATCHET_UI_PERF_SCOPE markers. Effectively perpetual
// (frames_ defaults to a huge count) so it keeps injecting for the entire FPS
// window; FpsMeasure closes the window. All synthetic panes are removed on finish
// / cancel; the user's persisted pane set is additionally restored from a backup
// taken by the launch harness.

#include "Commands/Scenarios/IScenario.h"

// clang-format off
#include "Interfaces/IAppScenarioHost.h"
// clang-format on
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "GridPane.h"
#include "SmatchetUiSession.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <string>
#include <vector>

extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {
// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=deliberate ACTIVE-load sibling of the side-by-side-grids / concurrent-sync perf family — intentionally mirrors their synthetic-pane purge + EnsurePaneContextLive pane-build loop + OnFinish perf-rows JSON so the measured render path is byte-identical to the passive baseline; folding into a shared helper would couple independent perf probes (a Pillar-5 CRITICAL per ADR-0015); owner=perf-tooling; revisit=2026-12-31)
// clang-format on

// Synthetic-pane id prefix (distinct from side-by-side-grids so the two never
// collide). A crashed prior run leaves these behind; OnStart purges any match.
const char* const kPerfPaneIdPrefix = "perf-interactive-grid-";

bool HasPrefix(const std::string& id) {
    const std::string prefix = kPerfPaneIdPrefix;
    return id.size() >= prefix.size() && id.compare(0, prefix.size(), prefix) == 0;
}

// The mixed 8-pane spec: 4 Jira (50%) + 2 GitHub + 2 Plane. View ids are the
// valid per-backend views on this box — Jira has default_view / new_view /
// new_view_2, GitHub has new_view_2 / github_default_view / new_view / new_view_3,
// and Plane has only plane_default_view so it cannot view-switch.
struct PaneSpec {
    const char* backendKey;
    const char* viewId;
};
const PaneSpec kPaneSpecs[] = {
    {"Jira", "default_view"},          // 0
    {"GitHub", "new_view_2"},          // 1
    {"Jira", "new_view"},              // 2
    {"Plane", "plane_default_view"},   // 3
    {"Jira", "new_view_2"},            // 4
    {"GitHub", "github_default_view"}, // 5
    {"Jira", "default_view"},          // 6
    {"Plane", "plane_default_view"},   // 7
};
const int kPaneCount = 8;

// A scheduled cross-backend view switch: at frame `frame`, force focus to pane
// `paneIndex` and re-point its viewId to `newViewId` (Jira + GitHub only — Plane
// has one view). "switch views a couple of times across multiple backends."
struct ViewSwitchEvent {
    int frame;
    int paneIndex;
    const char* newViewId;
};
const ViewSwitchEvent kViewSwitches[] = {
    {200, 0, "new_view_2"},   // Jira pane 0: default_view -> new_view_2
    {400, 1, "new_view"},     // GitHub pane 1: new_view_2 -> new_view
    {600, 4, "default_view"}, // Jira pane 4: new_view_2 -> default_view
    {800, 5, "new_view_3"},   // GitHub pane 5: github_default_view -> new_view_3
};

// 0 -> amp -> 0 triangle over `period` frames; always >= 0 so the scroll hook
// stays active every frame (continuous scroll, like a user dragging the bar).
int Triangle(int frame, int period, int amp) {
    const int half = (std::max)(1, period / 2);
    const int p = ((frame % period) + period) % period;
    const int up = (p < half) ? p : (period - p);
    return (amp * up) / half;
}
} // namespace

class InteractiveGridStressScenario : public IScenario {
  public:
    std::string Name() const override { return "interactive-grid-stress"; }

    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& /*outErr*/) override {
        // Effectively perpetual: keep injecting for the whole FpsMeasure window; the
        // window-close from FpsMeasure ends the process before IsDone fires.
        frames_ = (std::max)(1, args.value("frames", 100000000));
        switchEvery_ = (std::max)(1, args.value("switchEvery", 75));
        scrollY_ = 0;
        paneIds_.clear();
        addedPaneIds_.clear();
        focusCursor_ = 0;

        // Purge synthetic panes from a crashed prior run BEFORE building the set so
        // we observe exactly the mixed 8.
        PurgeSyntheticPanes();

        // Build the mixed-backend pane set. Unlike side-by-side-grids these do NOT
        // clone the bootstrap backend — each pane owns its declared backend so the
        // 4 Jira panes force a real Jira sync (the contention this scenario exists
        // to measure).
        for (int i = 0; i < kPaneCount; ++i) {
            const std::string id = std::string(kPerfPaneIdPrefix) + std::to_string(i);
            paneIds_.push_back(id);
            if (FindGridPaneById(g_ui.gridPanes, id)) {
                continue; // defensive: never collide with an existing pane id.
            }
            GridPane clone;
            clone.id = id;
            clone.title = std::string(kPaneSpecs[i].backendKey) + " #" + std::to_string(i);
            clone.backendKey = kPaneSpecs[i].backendKey;
            clone.viewId = kPaneSpecs[i].viewId;
            clone.open = true;
            g_ui.gridPanes.push_back(clone);
            addedPaneIds_.push_back(id);
        }

        // Spin a live context per pane with its OWN backend key so each backend's
        // sync actually runs (4 Jira contexts = real network sync). Idempotent.
        for (int i = 0; i < kPaneCount; ++i) {
            app.EnsurePaneContextLive(paneIds_[static_cast<std::size_t>(i)], kPaneSpecs[i].backendKey);
        }

        // Start focused on the first pane so the scroll hook has a focused target.
        g_ui.focusedPaneId = paneIds_[0];
        g_ui.gridPaneFocusReassigned = true;

        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(IAppScenarioHost& /*app*/, int frameIndex) override {
        // Vertical + horizontal scroll on the focused pane every frame. Desynced
        // periods so H and V rarely peak together (more realistic, more stress).
        scrollY_ = Triangle(frameIndex, /*period=*/200, /*amp=*/1600);
        g_ui.scenarioScrollTargetX = Triangle(frameIndex, /*period=*/150, /*amp=*/900);

        // Scheduled cross-backend view switches take priority on their frame (they
        // also move focus, so skip the rotation that frame to avoid a double-switch).
        bool didViewSwitch = false;
        for (const ViewSwitchEvent& ev : kViewSwitches) {
            if (ev.frame == frameIndex && ev.paneIndex < static_cast<int>(paneIds_.size())) {
                const std::string& id = paneIds_[static_cast<std::size_t>(ev.paneIndex)];
                if (GridPane* p = FindGridPaneById(g_ui.gridPanes, id)) {
                    p->viewId = ev.newViewId;
                    g_ui.focusedPaneId = id;
                    g_ui.gridPaneFocusReassigned = true; // force the activate path (else steady-state reverts viewId)
                    focusCursor_ = ev.paneIndex;
                    didViewSwitch = true;
                }
            }
        }

        // Periodic pane-focus switch ("switch between the panes while doing that").
        // Each step lands on the next mixed-backend pane, so most switches are
        // cross-backend and pay the full cfg/Save/ViewState reload path.
        if (!didViewSwitch && frameIndex > 0 && (frameIndex % switchEvery_) == 0 && !paneIds_.empty()) {
            focusCursor_ = (focusCursor_ + 1) % static_cast<int>(paneIds_.size());
            g_ui.focusedPaneId = paneIds_[static_cast<std::size_t>(focusCursor_)];
            g_ui.gridPaneFocusReassigned = true;
        }
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    int CurrentScrollY() const override { return scrollY_; }

    void OnCancel(IAppScenarioHost& /*app*/) override { Cleanup(); }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        Cleanup();
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = nlohmann::json::array();
        for (const UiPerfRow& r : rows) {
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
        out["scenario"] = "interactive-grid-stress";
        out["panes"] = kPaneCount;
        out["jira_panes"] = 4;
        out["frames"] = frames_;
        out["rows"] = std::move(rowsJson);
        out["note"] = "ACTIVE 8-pane mixed-backend load (4 Jira / 2 GitHub / 2 Plane) with per-frame "
                      "vertical + horizontal scroll, periodic cross-backend pane-focus switching, and a "
                      "few cross-backend view switches. UI-thread CPU scopes only — real wall-clock FPS "
                      "is captured by SMATCHET_FPS_MEASURE_SECONDS on the live window.";
        return out;
    }

  private:
    void PurgeSyntheticPanes() {
        for (std::size_t i = g_ui.gridPanes.size(); i-- > 0;) {
            if (HasPrefix(g_ui.gridPanes[i].id)) {
                g_ui.gridPanes.erase(g_ui.gridPanes.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }

    void Cleanup() {
        g_ui.scenarioScrollTargetX = -1;
        g_ui.scenarioScrollActive = false;
        g_ui.scenarioScrollTarget = -1;
        addedPaneIds_.clear();
        PurgeSyntheticPanes();
    }

    int frames_ = 100000000;
    int switchEvery_ = 75;
    int scrollY_ = 0;
    int focusCursor_ = 0;
    std::vector<std::string> paneIds_;
    std::vector<std::string> addedPaneIds_;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeInteractiveGridStressScenario() {
    return std::make_unique<smatchet::cmd::InteractiveGridStressScenario>();
}
