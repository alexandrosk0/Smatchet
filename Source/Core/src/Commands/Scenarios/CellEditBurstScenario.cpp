// CellEditBurstScenario — exposes `debug.grid.edit-burst` as a scenario.
// Verifies grid-cell-edit-perf: grid cell commit must NOT block the
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
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
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

    void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& outErr) override {
        const int count = (std::max)(1, args.value("count", 200));
        const std::string fieldId = args.value("field", std::string("summary"));
        const std::string issueIdArg = args.value("issue", std::string());
        const std::string newValue = args.value("value", std::string("burst-edit"));

        // Warmup + steady-frame budget (tooling.md `p99-gate-warmup-frame-
        // exclusion`). Before this fix cell-edit-burst was a one-frame scenario:
        // its sample ring held ONLY the cold-start frames that ran between
        // perf.reset and the single measured frame, so the snapshot p99 was pure
        // warmup (observed in CI: SmatchetUI::Draw p99 ~12 ms, drawEnsureCatalogAnd-
        // InitialSync ~9 ms, with a 0.0002 ms steady max). WarmupFrames() now
        // opts into the runner's one-time UiPerfMonitor::Reset() after `warmup_`
        // frames, then the scenario keeps running `steady_` more frames so the
        // ring re-fills with steady-state grid-draw samples. The full ring
        // (kCapacity=256) is saturated with steady samples so ComputeP99 ranks a
        // complete steady-state window. The burst itself still happens once in
        // OnStart; the extra frames only let the post-burst UI render settle and
        // be measured cleanly. Both knobs are args so a CI calibration pass can
        // retune without a recompile.
        // Clamp to a sane upper bound so warmup_ + steady_ cannot overflow a
        // signed int in IsDone() even with absurd CLI args (CodeRabbit, #1385).
        // The ring is kCapacity=256, so anything past a few hundred frames is
        // already pointless; 1e6 is a generous headroom well below INT_MAX/2.
        const int kMaxFrames = 1000000;
        warmup_ = (std::min)(kMaxFrames, (std::max)(0, args.value("warmup", 64)));
        steady_ = (std::min)(kMaxFrames, (std::max)(1, args.value("steady", 256)));

        nlohmann::json dispatchArgs;
        dispatchArgs["count"] = count;
        dispatchArgs["field"] = fieldId;
        if (!issueIdArg.empty())
            dispatchArgs["issue"] = issueIdArg;
        dispatchArgs["value"] = newValue;

        CommandContext ctx;
        // The runner only ever drives scenarios with the AppController that owns it (ScenarioRunner.cpp
        // stashes CommandContext::App), and AppController is the sole IAppScenarioHost implementer, so
        // the downcast is safe. This scenario needs the concrete type for CommandContext::App wiring.
        AppController& concrete = static_cast<AppController&>(app);
        ctx.App = &concrete;
        ctx.Source = CommandSource::Internal;

        const CommandResult res = concrete.Commands().Dispatch("debug.grid.edit-burst", dispatchArgs, ctx);
        if (res.Error.Code != ErrorCode::None) {
            outErr = "debug.grid.edit-burst failed: " + res.Error.Message;
            return;
        }
        result_ = res.Data ? *res.Data : nlohmann::json{};
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {}

    // Exclude the first `warmup_` frames from the snapshot p99 population: the
    // runner Reset()s the perf monitor once after this many frames, dropping the
    // cold-start spikes before they are ranked.
    int WarmupFrames() const override { return warmup_; }

    // Run warmup + steady frames so the post-reset ring is saturated with
    // steady-state samples (was a single-frame budget — see OnStart). Tick
    // increments frame_ before IsDone, so the scenario writes its result on the
    // frame after the last steady frame is drawn.
    bool IsDone(int frameIndex) const override { return frameIndex >= warmup_ + steady_; }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
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
    int warmup_ = 64;
    int steady_ = 256;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeCellEditBurstScenario() {
    return std::make_unique<smatchet::cmd::CellEditBurstScenario>();
}
