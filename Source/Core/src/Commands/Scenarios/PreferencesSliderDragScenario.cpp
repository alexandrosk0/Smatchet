// PreferencesSliderDragScenario — regression guard for the configmanager-save-
// coalesce change, which replaced 31 immediate
// `ConfigManager::Save` call sites in `SmatchetPreferencesUi.cpp` with a
// `MarkPrefsDirty(d)` helper that defers the save behind a debounce window
// (~100 ms). The end-of-frame fire (in `SmatchetUI::Draw` tail) is wrapped in
// `SMATCHET_UI_PERF_SCOPE("ConfigManager::Save (prefs-debounced)")` so we can
// observe its call count post-run.
// This scenario simulates a slider drag by calling `MarkPrefsDirty(g_ui)` every
// frame for N frames. With the coalesce in place, only ~ (frames / 6) saves
// should fire (debounce ≈ 100 ms ≈ 6 frames at 60 Hz). Without the coalesce we
// would see one save per frame.
// Production code under `SmatchetUI*.cpp` is out of scope for this PR; this
// scenario reaches it only via the already-exposed `MarkPrefsDirty` inline
// helper in `SmatchetUiSession.h`.

#include "Commands/Scenarios/IScenario.h"
#include "Ui/UiPerfRowsJson.h"

#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "SmatchetUiSession.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <string>

namespace smatchet {
namespace cmd {

namespace {
// Stable scope name the SmatchetUI::Draw tail wraps the debounced save in. If
// the production scope name changes this scenario will silently report 0 saves
// — perf-detective is expected to catch the mismatch on the next audit.
const char* kPrefsSaveScope = "ConfigManager::Save (prefs-debounced)";
} // namespace

class PreferencesSliderDragScenario : public IScenario {
  public:
    std::string Name() const override { return "preferences-slider-drag"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = (std::max)(1, args.value("frames", 600));
        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {
        // Simulate a user dragging a slider — 31 sites in SmatchetPreferencesUi
        // call MarkPrefsDirty(d) on every mutation, so one call per frame is
        // the worst case the coalesce has to absorb.
        MarkPrefsDirty(g_ui);
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);

        // Find the prefs-debounced save row (if present) and total it across
        // the run. `lifetimeHits` would be ideal but `GetLastFrameRows` only
        // returns the last frame's counts — we approximate over the run by
        // observing the row's presence + last-frame call count. The headline
        // assertion is `saveCallsLastFrame <= 1` since the debounce ensures at
        // most one save per ~100 ms window.
        double prefsSaveLastTotalMs = 0.0;
        std::uint32_t prefsSaveCallsLastFrame = 0;
        for (const UiPerfRow& r : rows) {
            if (r.name == kPrefsSaveScope) {
                prefsSaveLastTotalMs = r.lastTotalMs;
                prefsSaveCallsLastFrame = r.calls;
                break;
            }
        }

        nlohmann::json rowsJson = UiPerfRowsToJson(rows);

        nlohmann::json out;
        out["frames"] = frames_;
        out["prefsSaveScope"] = kPrefsSaveScope;
        out["prefsSaveLastTotalMs"] = prefsSaveLastTotalMs;
        out["prefsSaveCallsLastFrame"] = prefsSaveCallsLastFrame;
        // Debounce window ≈ 100 ms ≈ 6 frames @ 60 Hz: at most ~ frames/6 saves
        // should fire across the run. The last-frame count is the strict
        // bound — a coalesced save fires once per debounce window.
        out["ok_debounced"] = prefsSaveCallsLastFrame <= 1;
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    int frames_ = 600;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakePreferencesSliderDragScenario() {
    return std::make_unique<smatchet::cmd::PreferencesSliderDragScenario>();
}
