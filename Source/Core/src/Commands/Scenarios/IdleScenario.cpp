// IdleScenario — app at rest baseline.
// Drives N frames of the normal draw loop with **no** input injected and **no**
// transient state mutated. Used as the perf-floor reference: any other scenario
// whose mean-per-frame UI work exceeds this baseline is doing real work. Pair
// with `priority-grid-scroll` or `cell-edit-burst` to compute the delta.
// Audit lineage: perf-detective sweep on develop@31e1893 flagged the lack of a
// "baseline frame floor" scenario — the audit's noise floor had to be inferred
// from raw mean-per-frame numbers across other scenarios.

#include "Commands/Scenarios/IScenario.h"

#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "UiPerfMonitor.h"

#include <algorithm>

namespace smatchet {
namespace cmd {

class IdleScenario : public IScenario {
  public:
    std::string Name() const override { return "idle"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = (std::max)(1, args.value("frames", 600));
        // Reset so the captured numbers reflect this run only.
        UiPerfMonitor::Instance().Reset();
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {
        // No-op: idle baseline observes the natural draw loop with zero driver
        // interference. Anything that wakes background work shouldn't be here.
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        // Find the dominant UI-thread row (SmatchetUI::Draw is the canonical
        // umbrella scope) so the baseline is comparable across scenarios.
        double topMs = 0.0;
        std::string topName;
        for (const UiPerfRow& r : rows) {
            if (r.lastTotalMs > topMs) {
                topMs = r.lastTotalMs;
                topName = r.name;
            }
        }

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
        out["topName"] = topName;
        out["topLastTotalMs"] = topMs;
        // 144 Hz frame budget (1000 / 144). Baseline must be well under it.
        out["target_mean_ms"] = 6.94;
        out["ok_mean"] = topMs <= 6.94;
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    int frames_ = 600;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeIdleScenario() { return std::make_unique<smatchet::cmd::IdleScenario>(); }
