// CellEditBurstScenario — exposes `debug.grid.edit-burst` as a scenario.
// Verifies PR #186 (grid-cell-edit-perf): grid cell commit must NOT block the
// UI thread on an HTTP roundtrip — the dispatch hops to a worker via
// `MainThreadDispatcher`. This scenario lets `scenario.run --name=cell-edit-burst`
// drive the same headless harness used by
// `scripts/dev/test-grid-edit-perf-postfix.sh`.
// Implementation: OnStart dispatches the underlying command synchronously (the
// scenario runs on the UI thread, so RunOnUiThreadAsCommandResult takes the
// inline fast path). The result is stashed and returned from OnFinish. The
// scenario terminates after exactly 1 frame — the work is done at start time.

#include "Commands/Scenarios/IScenario.h"

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

class CellEditBurstScenario : public IScenario {
  public:
    std::string Name() const override { return "cell-edit-burst"; }

    void OnStart(AppController& app, const nlohmann::json& args, std::string& outErr) override {
        const int count = (std::max)(1, args.value("count", 200));
        const std::string fieldId = args.value("field", std::string("summary"));
        const std::string issueIdArg = args.value("issue", std::string());
        const std::string newValue = args.value("value", std::string("burst-edit"));

        nlohmann::json dispatchArgs;
        dispatchArgs["count"] = count;
        dispatchArgs["field"] = fieldId;
        if (!issueIdArg.empty())
            dispatchArgs["issue"] = issueIdArg;
        dispatchArgs["value"] = newValue;

        CommandContext ctx;
        ctx.App = &app;
        ctx.Source = CommandSource::Internal;

        const CommandResult res = app.Commands().Dispatch("debug.grid.edit-burst", dispatchArgs, ctx);
        if (res.Error.Code != ErrorCode::None) {
            outErr = "debug.grid.edit-burst failed: " + res.Error.Message;
            return;
        }
        result_ = res.Data;
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {}

    // One-frame scenario — the burst already completed in OnStart and the result
    // is staged. Returning true from IsDone(0) here would skip OnFrame entirely
    // (Tick increments frame_ before checking), so wait one frame so the runner
    // writes the result file. Matches `priority-grid-scroll` shape but with a
    // single-frame budget.
    bool IsDone(int frameIndex) const override { return frameIndex >= 1; }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        nlohmann::json out = result_;
        out["scenario"] = "cell-edit-burst";
        // Perf-rows emit so `scripts/dev/perf-baseline.sh init cell-edit-burst`
        // captures a baseline (per the 8-of-15 retrofit in
        // docs/self-improvement/categories/tooling.md). Pattern mirrors
        // PriorityGridScrollScenario::OnFinish.
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = nlohmann::json::array();
        std::transform(rows.begin(), rows.end(), std::back_inserter(rowsJson), [](const UiPerfRow& r) {
            return nlohmann::json{
                {"name", r.name},
                {"lastTotalMs", r.lastTotalMs},
                {"avgPerCallMs", r.avgPerCallMs},
                {"maxMs", r.maxMs},
                {"calls", r.calls},
                {"emaAvgMs", r.emaAvgMs},
                {"p99Ms", r.p99Ms},
            };
        });
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    nlohmann::json result_;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeCellEditBurstScenario() {
    return std::make_unique<smatchet::cmd::CellEditBurstScenario>();
}
