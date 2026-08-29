// SideBySideNGridScenario — N visible grid panes rendering simultaneously.
// Generalises SideBySide2GridScenario (multi-grid-tabs Slice 5b) so an operator
// can sweep the "how many visible panes before the 6.94 ms / 144 Hz budget
// blows" question the plan left open (§ Performance: "3-4 grids drawing
// simultaneously is ~N x the single-grid draw … if N x draw exceeds budget, cap
// simultaneous visible panes"). Tops the open-pane set up to `panes` (default 8)
// by cloning the bootstrap pane's backend / view (same offline data set, no
// network forced), spins a live GridLiveContext per pane via EnsurePaneContextLive
// (mirrors the host's per-visible-pane lifecycle), then runs N frames as a passive
// observer of the per-frame UI budget. The host's drawGridPaneWindows loop renders
// every open pane window per frame, so the existing `pane.render` /
// `drawActiveProjectWindow` scopes capture the N-grids-per-frame cost. All
// synthetic panes are removed on finish / cancel so the user's persisted pane set
// is untouched. Mirrors the passive-observer shape of side-by-side-2-grid.

#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/UiPerfRowsJson.h"

// clang-format off
#include "Interfaces/IAppScenarioHost.h"
// clang-format on
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "SmatchetUiSession.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <string>
#include <vector>

extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {
// Synthetic-pane id prefix. A crashed prior run leaves these behind; OnStart
// purges any match before sizing so the run observes exactly paneTarget_ panes.
const char* const kPerfPaneIdPrefix = "perf-side-by-side-grid-";
// Clamp the requested pane count to a sane band: >=2 (a "side-by-side" needs at
// least two), <=64 (guards a fat-finger --panes=100000 from OOM-ing the box).
const int kMinPanes = 2;
const int kMaxPanes = 64;

bool HasPrefix(const std::string& id) {
    const std::string prefix = kPerfPaneIdPrefix;
    return id.size() >= prefix.size() && id.compare(0, prefix.size(), prefix) == 0;
}
} // namespace

class SideBySideNGridScenario : public IScenario {
  public:
    std::string Name() const override { return "side-by-side-grids"; }

    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = (std::max)(1, args.value("frames", 600));
        paneTarget_ = (std::min)(kMaxPanes, (std::max)(kMinPanes, args.value("panes", 8)));
        addedPaneIds_.clear();
        maxFrameTopMs_ = 0.0;

        // Capture the bootstrap pane's identity BEFORE any push_back can
        // reallocate the vector and dangle the reference (LoadPanesOrBootstrap
        // guarantees >= 1 pane; focusedPane() self-heals if not).
        const GridPane& base = g_ui.focusedPane();
        const std::string basePaneId = base.id;
        const std::string baseBackendKey = base.backendKey;
        const std::string baseViewId = base.viewId;
        const std::string baseTitle = base.title;

        // Purge synthetic panes left by a crashed prior run BEFORE sizing. Counting
        // leftovers toward paneTarget_ would measure the wrong pane count — and when a
        // crash left more than paneTarget_ of them the sizing loop below adds nothing,
        // so the run would observe MORE than the requested N panes (CR review #1464).
        PurgeSyntheticPanes();

        // Top the open-pane set up to paneTarget_ by cloning the base pane's
        // backend + view so every grid renders the same offline data set. `base`
        // is dead after the first push_back — only the captured strings are used.
        int idx = 0;
        while (static_cast<int>(g_ui.gridPanes.size()) < paneTarget_ && idx < kMaxPanes * 2) {
            const std::string syntheticId = std::string(kPerfPaneIdPrefix) + std::to_string(idx);
            ++idx;
            if (FindGridPaneById(g_ui.gridPanes, syntheticId)) {
                continue; // defensive: never collide with an existing pane id.
            }
            GridPane clone;
            clone.id = syntheticId;
            clone.title = baseTitle + " #" + std::to_string(idx);
            clone.backendKey = baseBackendKey;
            clone.viewId = baseViewId;
            clone.open = true;
            g_ui.gridPanes.push_back(clone);
            addedPaneIds_.push_back(syntheticId);
        }

        // Spin a live context for the base pane + every synthetic clone so each
        // pane ticks its own GridLiveContext (mirrors the host's per-visible-pane
        // lifecycle). All clones use the base's (offline) backend — deterministic,
        // no backend swap / network forced. Idempotent.
        app.EnsurePaneContextLive(basePaneId, baseBackendKey);
        for (const std::string& id : addedPaneIds_) {
            app.EnsurePaneContextLive(id, baseBackendKey);
        }

        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {
        // Passive observer: track the worst per-frame UI scope across the run so a
        // one-frame spike from the N-grid draw is captured. LastFrameTopTotalMs
        // reads the prior frame's totals (aggregated by UiPerfMonitor::BeginFrame).
        const double topMs = UiPerfMonitor::Instance().LastFrameTopTotalMs();
        if (topMs > maxFrameTopMs_)
            maxFrameTopMs_ = topMs;
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    void OnCancel(IAppScenarioHost& /*app*/) override { RemoveSyntheticPanes(); }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        RemoveSyntheticPanes();

        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = UiPerfRowsToJson(rows);
        nlohmann::json out;
        out["scenario"] = "side-by-side-grids";
        out["panes"] = paneTarget_;
        out["frames"] = frames_;
        out["maxFrameTopMs"] = maxFrameTopMs_;
        out["target_p99_ms"] = 10.0; // 100 Hz floor
        out["ok_p99"] = maxFrameTopMs_ <= 10.0;
        out["rows"] = std::move(rowsJson);
        out["note"] = "N grid panes (panes=) drawing each frame (host drawGridPaneWindows loop "
                      "renders all). Measures the N-grids-per-frame draw cost via the `pane.render` "
                      "/ `drawActiveProjectWindow` scopes. Offline + deterministic: no backend swap "
                      "or network sync is forced — both/all grids render their (empty / cached) "
                      "projection, so the structural per-pane render work is the measured cost. NOTE: "
                      "this is UI-thread CPU time under headless software GL, NOT real wall-clock FPS "
                      "(no GPU / vsync / present) — use SMATCHET_FPS_MEASURE_SECONDS on the real "
                      "window for perceived FPS.";
        return out;
    }

  private:
    // Erase every synthetic (prefix-matched) pane from g_ui.gridPanes. Reverse
    // iteration keeps the unvisited indices valid across each erase. Shared by the
    // start-of-run purge (exact paneTarget_) and the on-finish cleanup.
    void PurgeSyntheticPanes() {
        for (std::size_t i = g_ui.gridPanes.size(); i-- > 0;) {
            if (HasPrefix(g_ui.gridPanes[i].id)) {
                g_ui.gridPanes.erase(g_ui.gridPanes.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
    }

    void RemoveSyntheticPanes() {
        // Erase every scenario-installed pane so the user's persisted pane set is
        // untouched. The lingering GridLiveContexts self-retire (hidden grace
        // window) via TickAllContexts.
        addedPaneIds_.clear();
        PurgeSyntheticPanes();
    }

    int frames_ = 600;
    int paneTarget_ = 8;
    double maxFrameTopMs_ = 0.0;
    std::vector<std::string> addedPaneIds_;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeSideBySideNGridScenario() {
    return std::make_unique<smatchet::cmd::SideBySideNGridScenario>();
}
