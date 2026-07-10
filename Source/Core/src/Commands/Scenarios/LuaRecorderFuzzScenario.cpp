// LuaRecorderFuzzScenario — hostile-input fuzz of the recorded-cmd-list path.
// Plan reference: docs/plans/shipped/lua-recorded-cmd-list.md §Crash-safety hardening
//   §Fuzz test (verification step 14).
// Registers a Lua provider (`render_fuzz_cell`, defined in
// scripts/LuaRecorderFuzz.lua) on a configurable field (default `summary`) and
// drives the grid scroll for N frames. The provider body randomises calls with
// every documented hostile-input class (NaN sizes, OOB color indices, oversized
// strings, unbalanced push/pop, label collisions). Crash-safety hardening must
// hold across this run.
// Default frames = 600 (10 s at 60 fps). For a CI smoke job, raise to 3600.

#include "Commands/Scenarios/IScenario.h"

#include "Interfaces/IAppScenarioHost.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "UiPerfMonitor.h"

#include <algorithm>
#include <string>

namespace smatchet {
namespace cmd {

class LuaRecorderFuzzScenario : public IScenario {
  public:
    std::string Name() const override { return "lua-recorder-fuzz"; }

    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& outErr) override {
        frames_ = args.value("frames", 600);
        pixPerFrame_ = args.value("pixPerFrame", 4);
        const std::string fieldId = args.value("field", std::string("summary"));
        const std::string luaFn = args.value("luaFn", std::string("render_fuzz_cell"));

        std::string err;
        const std::vector<std::string> extraScripts = {"LuaRecorderFuzz.lua"};
        if (!app.ScenarioRegisterLuaCachedProvider(fieldId, luaFn, extraScripts, err)) {
            outErr = "ScenarioRegisterLuaCachedProvider failed: " + err;
            return;
        }
        boundField_ = fieldId;
        scrollY_ = 0;
    }

    void OnFrame(IAppScenarioHost& app, int /*frameIndex*/) override {
        scrollY_ += pixPerFrame_;
        // Drop every cell cache entry so the next paint re-records via the fuzz provider.
        // Without this the cache hit-rate becomes 100% after first paint and the recorder
        // path itself is no longer exercised — defeating the point of fuzzing.
        app.ScenarioInvalidateLuaFieldCache();
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    int CurrentScrollY() const override { return scrollY_; }

    void OnCancel(IAppScenarioHost& app) override {
        if (!boundField_.empty()) {
            app.ScenarioUnregisterLuaCachedProvider(boundField_);
            boundField_.clear();
        }
    }

    nlohmann::json OnFinish(IAppScenarioHost& app) override {
        if (!boundField_.empty()) {
            app.ScenarioUnregisterLuaCachedProvider(boundField_);
            boundField_.clear();
        }
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
        out["frames"] = frames_;
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    int frames_ = 600;
    int pixPerFrame_ = 4;
    int scrollY_ = 0;
    std::string boundField_;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeLuaRecorderFuzzScenario() {
    return std::make_unique<smatchet::cmd::LuaRecorderFuzzScenario>();
}
