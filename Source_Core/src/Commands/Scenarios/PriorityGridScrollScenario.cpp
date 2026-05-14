// PriorityGridScrollScenario — drives the priority-grid scroll at a fixed
// pixels-per-frame rate for N frames, measuring UI perf row timings.
// See backlog/COMMAND_SYSTEM_PLAN.md §PriorityGridScrollScenario.

#include "Commands/Scenarios/IScenario.h"

#include "AppController.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <string>

namespace smatchet {
namespace cmd {

class PriorityGridScrollScenario : public IScenario {
public:
    std::string Name() const override { return "priority-grid-scroll"; }

    void OnStart(AppController& app, const nlohmann::json& args, std::string& outErr) override {
        frames_      = args.value("frames",        600);
        pixPerFrame_ = args.value("pixPerFrame",   8);
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
                luaProviderField_     = fid;
                registeredLuaPriority_ = true;
            } else {
                outErr = "ScenarioRegisterLuaCachedProvider failed: " + err;
            }
        }
        // Let the UiPerfMonitor reset so timings reflect this run only.
        // (No public Reset() yet; snapshot at start and diff at finish as a proxy.)
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {
        scrollY_ += pixPerFrame_;
    }

    bool IsDone(int frameIndex) const override {
        return frameIndex >= frames_;
    }

    nlohmann::json OnFinish(AppController& app) override {
        if (registeredLuaPriority_) {
            app.ScenarioUnregisterLuaCachedProvider(luaProviderField_);
            registeredLuaPriority_ = false;
        }
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
        nlohmann::json rowsJson = nlohmann::json::array();
        for (const UiPerfRow& r : rows) {
            rowsJson.push_back({
                {"name",         r.name},
                {"lastTotalMs",  r.lastTotalMs},
                {"avgPerCallMs", r.avgPerCallMs},
                {"maxMs",        r.maxMs},
                {"calls",        r.calls},
                {"emaAvgMs",     r.emaAvgMs},
            });
        }
        nlohmann::json out;
        out["frames"] = frames_;
        out["rows"]   = std::move(rowsJson);
        return out;
    }

    /// Called by ScenarioRunner::Tick before OnFrame so SmatchetActiveProjectGridUi
    /// can read the driven scroll position. Returns the pixel Y to set.
    int CurrentScrollY() const { return scrollY_; }

private:
    int         frames_                = 600;
    int         pixPerFrame_           = 8;
    int         scrollY_               = 0;
    bool        registeredLuaPriority_ = false;
    std::string luaProviderField_;
};

}  // namespace cmd
}  // namespace smatchet

/// Factory function callable from AppController::Initialize without including
/// the concrete type header. Declared `extern` at the call site.
std::unique_ptr<smatchet::cmd::IScenario> MakePriorityGridScrollScenario() {
    return std::unique_ptr<smatchet::cmd::IScenario>(
        new smatchet::cmd::PriorityGridScrollScenario());
}
