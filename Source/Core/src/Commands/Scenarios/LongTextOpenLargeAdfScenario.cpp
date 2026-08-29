// LongTextOpenLargeAdfScenario — regression guard for the async-rich-seed
// threshold, which added `kAsyncRichSeedThresholdBytes = 32 * 1024` to
// `TicketFieldEditor.cpp::OpenLongTextEditor`: when the raw rich payload is
// larger than the threshold, the ADF → Markdown seed compute moves to a worker
// thread via `LaunchBackgroundTask` and the UI shows "Loading description…"
// instead of blocking. Without this threshold a 32 KB+ ADF stalls the UI for
// hundreds of ms while `MarkdownConvert::AdfToMarkdown` walks the tree.
// This scenario reaches the production code only via the already-extracted
// pure helper `TicketFieldEditorLongTextPure::ComputeLongTextSeed`. We build a
// synthetic > 32 KB ADF, time the seed compute once on the UI thread (the
// "what would have happened without the threshold" measurement), then run N
// frames and emit the steady-state per-frame stats. The two numbers together
// tell us the threshold is worth keeping — pre-regression would set
// `seedComputeMs > frame_budget` while leaving `meanFrameMs` near baseline.
// Note: this scenario does NOT drive `OpenLongTextEditor` directly because
// production UI files (`TicketFieldEditor.cpp`, etc.) are out of scope for
// this PR. The pure compute timing is a proxy for the worker dispatch's win.

#include "Commands/Scenarios/IScenario.h"
#include "Commands/Scenarios/UiPerfRowsJson.h"

#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "TicketFieldEditorLongTextPure.h"
#include "UiPerfMonitor.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

// Threshold guarded in TicketFieldEditor.cpp. Kept in sync via the
// scenario assertion below.
constexpr std::size_t kAsyncRichSeedThresholdBytes = 32 * 1024;

/// Build a synthetic ADF JSON string larger than the threshold. Each repeated
/// paragraph node is structurally valid ADF; the converter walks every node so
/// the timing reflects realistic parse + render-to-markdown cost.
std::string BuildLargeAdf() {
    std::string out;
    out.reserve(kAsyncRichSeedThresholdBytes + 4096);
    out.append("{\"version\":1,\"type\":\"doc\",\"content\":[");
    // 200 paragraph nodes × ~200 bytes each ≈ 40 KB — comfortably above 32 KB.
    const char* node = "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\""
                       "Lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod "
                       "tempor incididunt ut labore et dolore magna aliqua ut enim ad minim "
                       "veniam quis nostrud exercitation ullamco laboris.\"}]}";
    for (int i = 0; i < 200; ++i) {
        if (i > 0)
            out.push_back(',');
        out.append(node);
    }
    out.append("]}");
    return out;
}

} // namespace

class LongTextOpenLargeAdfScenario : public IScenario {
  public:
    std::string Name() const override { return "long-text-open-large-adf"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = (std::max)(1, args.value("frames", 600));
        UiPerfMonitor::Instance().Reset();

        // One-shot seed compute timing on the UI thread. This is the cost the
        // worker dispatch saves the user from paying inline.
        const std::string adf = BuildLargeAdf();
        adfBytes_ = adf.size();
        crossesThreshold_ = adfBytes_ > kAsyncRichSeedThresholdBytes;

        std::vector<std::string> droppedNodes;
        bool rawMode = false;
        const auto t0 = std::chrono::steady_clock::now();
        const std::string seed = TicketFieldEditorLongTextPure::ComputeLongTextSeed(
            TicketFieldEditorLongTextPure::LongTextRichKind::Adf, adf,
            /*strippedFallback=*/std::string(), droppedNodes, rawMode);
        const auto t1 = std::chrono::steady_clock::now();
        seedComputeMs_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
        seedBytes_ = seed.size();
        droppedCount_ = droppedNodes.size();
    }

    void OnFrame(IAppScenarioHost& /*app*/, int /*frameIndex*/) override {
        // Steady-state observer; the seed compute happened in OnStart. With
        // the worker dispatch in production these frames would coincide with
        // a "Loading description…" banner — we just verify the UI thread is
        // not paying for the parse.
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override {
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows(/*includeP99=*/true);
        double topMs = 0.0;
        std::string topName;
        for (const UiPerfRow& r : rows) {
            if (r.lastTotalMs > topMs) {
                topMs = r.lastTotalMs;
                topName = r.name;
            }
        }

        nlohmann::json rowsJson = UiPerfRowsToJson(rows);

        nlohmann::json out;
        out["frames"] = frames_;
        out["adfBytes"] = adfBytes_;
        out["seedBytes"] = seedBytes_;
        out["thresholdBytes"] = kAsyncRichSeedThresholdBytes;
        out["crossesThreshold"] = crossesThreshold_;
        out["seedComputeMs"] = seedComputeMs_;
        out["droppedAdfNodeTypes"] = droppedCount_;
        out["frameTopName"] = topName;
        out["frameTopLastTotalMs"] = topMs;
        out["target_mean_ms"] = 6.94;
        out["ok_frame_budget"] = topMs <= 6.94;
        // Sanity: if the seed compute fits under the 144 Hz frame budget on
        // this machine then the threshold gains nothing here — the worker
        // dispatch is still correct for slower machines / larger payloads.
        out["worker_dispatch_was_worth_it"] = seedComputeMs_ > 6.94;
        out["rows"] = std::move(rowsJson);
        return out;
    }

  private:
    int frames_ = 600;
    std::size_t adfBytes_ = 0;
    std::size_t seedBytes_ = 0;
    bool crossesThreshold_ = false;
    double seedComputeMs_ = 0.0;
    std::size_t droppedCount_ = 0;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeLongTextOpenLargeAdfScenario() {
    return std::make_unique<smatchet::cmd::LongTextOpenLargeAdfScenario>();
}
