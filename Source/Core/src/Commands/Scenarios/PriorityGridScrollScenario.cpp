// PriorityGridScrollScenario — drives the priority-grid scroll at a fixed
// pixels-per-frame rate for N frames, measuring UI perf row timings.
// See docs/plans/shipped/command-system-plan.md §PriorityGridScrollScenario.

#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/UiPerfRowsJson.h"

#include "Interfaces/IAppScenarioHost.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "UiPerfMonitor.h"

#include <algorithm>
#include <string>

namespace smatchet {
namespace cmd {

class PriorityGridScrollScenario : public IScenario {
  public:
    std::string Name() const override { return "priority-grid-scroll"; }

    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& outErr) override {
        frames_ = args.value("frames", 600);
        pixPerFrame_ = args.value("pixPerFrame", 8);
        // Seed the scroll at y=0 before we begin.
        scrollY_ = 0;
        // Optional: force the Lua `priority` cached provider on for the run so the
        // LuaDrawList::Replay path is actually exercised. Lets perf-measure runs validate
        // the recorded-cmd-list feature end-to-end without manual `SmatchetHooks.lua` edits.
        registeredLuaPriority_ = false;
        if (args.value("registerLuaProvider", false)) {
            const std::string fn = args.value("luaProviderFn", std::string("render_priority_cell"));
            const std::string fid = args.value("luaProviderField", std::string("priority"));
            std::string err;
            if (app.ScenarioRegisterLuaCachedProvider(fid, fn, err)) {
                luaProviderField_ = fid;
                registeredLuaPriority_ = true;
            } else {
                outErr = "ScenarioRegisterLuaCachedProvider failed: " + err;
            }
        }
        // Let the UiPerfMonitor reset so timings reflect this run only.
        // (No public Reset() yet; snapshot at start and diff at finish as a proxy.)
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override { scrollY_ += pixPerFrame_; }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    void OnCancel(IAppScenarioHost& app) override {
        // Unwind transient state so cancelling a perf run never leaves a scenario-installed
        // provider clobbering the user's actual `priority` renderer for the rest of the
        // session. ScenarioUnregisterLuaCachedProvider restores the prior provider.
        if (registeredLuaPriority_) {
            app.ScenarioUnregisterLuaCachedProvider(luaProviderField_);
            registeredLuaPriority_ = false;
        }
    }

    nlohmann::json OnFinish(IAppScenarioHost& app) override {
        if (registeredLuaPriority_) {
            app.ScenarioUnregisterLuaCachedProvider(luaProviderField_);
            registeredLuaPriority_ = false;
        }
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = UiPerfRowsToJson(rows);
        nlohmann::json out;
        out["frames"] = frames_;
        out["rows"] = std::move(rowsJson);
        return out;
    }

    /// Called by ScenarioRunner::Tick each frame; the runner propagates the value into
    /// `outScrollTarget` so SmatchetActiveProjectGridUi can honour the driven scroll. The
    /// IScenario default returns -1 (no scroll); this override returns the accumulated Y.
    int CurrentScrollY() const override { return scrollY_; }

  private:
    int frames_ = 600;
    int pixPerFrame_ = 8;
    int scrollY_ = 0;
    bool registeredLuaPriority_ = false;
    std::string luaProviderField_;
};

} // namespace cmd
} // namespace smatchet

/// Factory function callable from AppController::Initialize without including
/// the concrete type header. Declared `extern` at the call site.
std::unique_ptr<smatchet::cmd::IScenario> MakePriorityGridScrollScenario() {
    return std::make_unique<smatchet::cmd::PriorityGridScrollScenario>();
}
