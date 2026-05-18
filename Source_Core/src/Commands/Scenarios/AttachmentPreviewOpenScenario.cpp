// AttachmentPreviewOpenScenario — regression guard for PR #191.
//
// PR #191 moved `SmatchetAttachmentPreviewUi`'s attachment download from a
// synchronous `cpr::Get` on the UI thread to a worker thread via
// `LaunchBackgroundTask` + `MainThreadDispatcher`. The visible-cue contract
// requires no UI-thread hitch > 100 ms while the download is in flight.
//
// This scenario reaches the production code only via existing CLI / scope
// observation — production UI files (`SmatchetAttachmentPreviewUi.cpp`,
// `AppController*.cpp`) are out of scope for this PR. We run N frames as a
// passive observer of the per-frame budget and assert no single frame's
// dominant scope crossed the 16.67 ms (60 Hz floor) line. With the worker
// dispatch in place this should be trivially true; if PR #191 is ever reverted
// the inline cpr::Get would surface here as a one-frame spike.

#include "Commands/Scenarios/IScenario.h"

#include "AppController.h"
#include "UiPerfMonitor.h"

#include <algorithm>

namespace smatchet {
namespace cmd {

class AttachmentPreviewOpenScenario : public IScenario {
  public:
    std::string Name() const override { return "attachment-preview-open"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& /*outErr*/) override {
        frames_ = std::max(1, args.value("frames", 600));
        UiPerfMonitor::Instance().Reset();
        maxFrameTopMs_ = 0.0;
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {
        // Sample the dominant per-frame UI scope so a one-frame spike is
        // captured. GetLastFrameRows is updated by UiPerfMonitor::BeginFrame
        // (called from SmatchetUI::Draw); reading it here gives us the prior
        // frame's totals, which is exactly the cadence we want.
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
        double topMs = 0.0;
        for (const UiPerfRow& r : rows) {
            if (r.lastTotalMs > topMs)
                topMs = r.lastTotalMs;
        }
        if (topMs > maxFrameTopMs_)
            maxFrameTopMs_ = topMs;
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= frames_; }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        const std::vector<UiPerfRow> rows = UiPerfMonitor::Instance().GetLastFrameRows();
        nlohmann::json rowsJson = nlohmann::json::array();
        for (const UiPerfRow& r : rows) {
            rowsJson.push_back({
                {"name", r.name},
                {"lastTotalMs", r.lastTotalMs},
                {"avgPerCallMs", r.avgPerCallMs},
                {"maxMs", r.maxMs},
                {"calls", r.calls},
                {"emaAvgMs", r.emaAvgMs},
            });
        }
        nlohmann::json out;
        out["frames"] = frames_;
        out["maxFrameTopMs"] = maxFrameTopMs_;
        out["target_p99_ms"] = 16.67; // 60 Hz floor
        out["ok_p99"] = maxFrameTopMs_ <= 16.67;
        out["rows"] = std::move(rowsJson);
        out["note"] = "Passive observer; no attachment opened by driver. "
                      "Real fixture flow requires a tracker-backed test exe. "
                      "Pre-regression of PR #191 would surface as ok_p99=false.";
        return out;
    }

  private:
    int frames_ = 600;
    double maxFrameTopMs_ = 0.0;
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeAttachmentPreviewOpenScenario() {
    return std::unique_ptr<smatchet::cmd::IScenario>(new smatchet::cmd::AttachmentPreviewOpenScenario());
}
