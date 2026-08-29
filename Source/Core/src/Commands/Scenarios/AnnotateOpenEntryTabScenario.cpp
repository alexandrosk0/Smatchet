// AnnotateOpenEntryTabScenario — slice 8 of autonomous-debugging-no-creds.
// Pillar-1 regression gate stand-in for the annotate Entry tab tokenizer hot
// path. The full backlog ask (tooling.md P2 line 178 / now renamed
// `annotate-open-entry-tab`) names a fake-callstack injection API on
// AppController so the scenario can prime AnnotateAnalysisUi::State().
// callstackBuf without going through the live Jira fetch. That API doesn't
// exist yet — this scenario ships the *driver shape* in advance so the
// perf-pr-fast scenario subset has a row to compare against, and so the
// `annotate-open-entry-tab` name resolves at `scenario.run --name=` time.
// What it does today:
//   * Snapshot + restore `g_ui.showAnnotateAnalysis` (open / close the panel).
//   * Run N frames with the panel open so SmatchetUI::Draw's annotate branch
//     accumulates UiPerfMonitor rows.
//   * Emit `rows[]` so `perf-gatekeeper` can read it.
// What it doesn't do (and is explicitly deferred to the backlog follow-up):
//   * Seed `callstackBuf` with a fake callstack — needs the AppController
//     fake-callstack injection seam named in tooling.md line 180.
//   * Switch to the Entry tab via grid selection — needs a live ticket.
// The scenario is registered as `annotate-open-entry-tab` (kebab convention
// matching `priority-grid-scroll` / `dock-gap-sentinel` /
// `command-palette-fuzzy`).

#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/UiPerfRowsJson.h"

#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "SmatchetUiSession.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

class AnnotateOpenEntryTabScenario : public IScenario {
  public:
    std::string Name() const override { return "annotate-open-entry-tab"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = (std::max)(1, args.value("frames", 300));
        savedShowAnnotate_ = g_ui.showAnnotateAnalysis;
        g_ui.showAnnotateAnalysis = true;
        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {
        // Idle — let the panel-open branch accumulate timings.
        // SMATCHET_DEVIATION(rule=duplication; reason=perf-scenario framing clone (OnFrame/IsDone/OnFinish skeleton around scenario-specific drive logic) re-entered the delta scan when the shared rows serializer moved to UiPerfRowsJson.h; the residual framing is per-scenario configuration, not extractable logic; owner=scenarios; revisit=2027-03-01)
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    void OnCancel(IAppScenarioHost& /*app*/) override { Restore(); }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        nlohmann::json rowsJson = UiPerfRowsToJson(rows);
        nlohmann::json out;
        out["frames"] = frames_;
        // 144 Hz frame budget — pillar 1 target. The scenario doesn't fail
        // hard on overshoot (perf-gatekeeper compares vs baseline); the
        // target is recorded so the row table is self-describing.
        out["target_mean_ms"] = 6.94;
        out["rows"] = std::move(rowsJson);
        Restore();
        return out;
    }

  private:
    void Restore() { g_ui.showAnnotateAnalysis = savedShowAnnotate_; }

    int frames_ = 300;
    bool savedShowAnnotate_ = false;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeAnnotateOpenEntryTabScenario() {
    return std::unique_ptr<smatchet::cmd::IScenario>(std::make_unique<smatchet::cmd::AnnotateOpenEntryTabScenario>());
}
