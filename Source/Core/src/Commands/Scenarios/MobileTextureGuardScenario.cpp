// MobileTextureGuardScenario — Issue #1133 mobile CI smoke gate.
// Drives the dynamic-texture guard end-to-end inside the live render loop so a
// headless CI run proves the [P0 #1122] orphaned-command repoint path (a) does
// not SIGABRT and (b) actually fires its once-latched LOG_WARN. The fault
// injector (Source/Core/include/Ui/SmatchetTextureFaultInjector.h) commits a
// doomed ImGui draw command; the end-of-frame guard
// (SmatchetImGuiTextureGuardRuntime.h GuardImGuiDynamicTextures, called from
// Source/Standalone/main.cpp AFTER ImGui::Render()) repoints the orphan and
// emits the verbatim LOG_WARN substring "[P0 #1122] repointed".
// Frame ordering: this scenario's OnFrame runs inside SmatchetUI::Draw (between
// NewFrame and Render), so a fault forced on frame N is repointed by the SAME
// frame's end-of-frame guard; we scan the log on frame N+1. The mere fact
// OnFrame(N+1) runs at all proves frame N's RenderDrawData did not abort (a real
// SIGABRT kills the process before the next Tick) — that is the no-crash
// assertion, no in-process catch needed.
// The injector/guard symbols are ALWAYS callable (no-ops when
// SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION is undefined), so this TU compiles
// unconditionally in CORE_SOURCES on every preset. When the macro is OFF the
// forced faults do nothing and recoveryLogSeen stays false — the scenario only
// meaningfully passes on the test / ui-test presets where the macro is ON. That
// is expected and correct; do NOT #ifdef this TU.

#include "Commands/Scenarios/IScenario.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Commands/Scenarios/ScenarioArgs.h"
#include "Logger.h"
#include "Ui/SmatchetImGuiTextureGuardRuntime.h"
#include "Ui/SmatchetTextureFaultInjector.h"

#include <algorithm>
#include <string>
#include <vector>

namespace smatchet {
namespace cmd {

namespace {

// IntArg comes from the shared Commands/Scenarios/ScenarioArgs.h — the
// per-scenario anon-namespace copy is gone (debt 2026-06-28).

// Verbatim substring the guard's once-latched LOG_WARN must contain.
const char kRepointMarker[] = "[P0 #1122] repointed";

// COUNT (not presence) of repoint WARNs in the Logger snapshot. A boolean
// "does the log contain the marker?" green-washes: the marker LogEntry PERSISTS
// across frames (ResetTextureGuardLogLatch only re-arms emission; it does not
// clear Logger entries), so once any sub-step's WARN lands, every later
// presence-scan returns true even if that later sub-step's own fault repointed
// nothing. Each sub-step instead captures a baseline count at fault time and
// asserts the count STRICTLY INCREASED by scan time — proving THIS fault's guard
// pass emitted a fresh WARN, not that a prior one is lingering.
int RepointMarkerCount() {
    const std::vector<LogEntry> entries = Logger::Instance().GetEntriesSnapshot();
    int n = 0;
    for (const LogEntry& e : entries) {
        if (e.message.find(kRepointMarker) != std::string::npos)
            ++n;
    }
    return n;
}

// Per-sub-step fault/scan state machine. CRITICAL ORDERING (the #1133 honesty
// fix): the runner calls Scenarios().Tick() — which runs OnFrame — at the TOP of
// the render loop, BEFORE ImGui::Render() and the end-of-frame
// GuardImGuiDynamicTextures (StandaloneAppBootstrap.cpp). So a fault injected in
// OnFrame(N) is only repointed (and its WARN emitted) by the guard at the END of
// render-frame N — which is AFTER OnFrame(N+1) has already run. Scanning on N+1
// therefore races the WARN and intermittently misses it (the original stuck:0).
// We leave a TWO-frame gap (fault on f, scan on f+2) so the fault frame's guard
// pass is guaranteed complete before the scan. The scan is count-delta based (see
// RepointMarkerCount) so a lingering prior WARN cannot satisfy a later sub-step.
class MobileTextureGuardScenario : public IScenario {
  public:
    std::string Name() const override { return "mobile-texture-guard"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& /*outErr*/) override {
        // Total frame budget is fixed at 10 (3 sub-steps × fault+2-frame-gap, see
        // IsDone); the arg only lets a caller extend the run for manual
        // inspection, never shorten it below the 10 the state machine needs.
        totalFrames_ = (std::max)(10, IntArg(args, "frames", 10));
    }

    void OnFrame(AppController& /*app*/, int frameIndex) override {
        framesAdvanced_ = (std::max)(framesAdvanced_, frameIndex);

        switch (frameIndex) {
        case 0:
        case 1:
            // Warm-up: let the font atlas bake + panels settle before any
            // fault is injected.
            break;
        case 2:
            smatchet::ui::ResetTextureGuardLogLatch();
            repointBaselineStuck_ = RepointMarkerCount();
            smatchet::ui::ForceTextureStuck();
            break;
        // f3 = guard repoints f2's orphan + emits WARN (gap frame).
        case 4:
            recoveryLogSeenStuck_ = RepointMarkerCount() > repointBaselineStuck_;
            smatchet::ui::ResetTextureGuardLogLatch();
            repointBaselineCtxLoss_ = RepointMarkerCount();
            smatchet::ui::ForceContextLoss();
            break;
        // f5 = guard repoints f4's orphan + emits WARN (gap frame).
        case 6:
            recoveryLogSeenCtxLoss_ = RepointMarkerCount() > repointBaselineCtxLoss_;
            // Negative control: with re-arm disabled the guard must STILL
            // repoint the orphaned command.
            smatchet::ui::SetRearmDisabled(true);
            smatchet::ui::ResetTextureGuardLogLatch();
            repointBaselineNegCtl_ = RepointMarkerCount();
            smatchet::ui::ForceTextureStuck();
            break;
        // f7 = guard repoints f6's orphan + emits WARN (gap frame).
        case 8:
            recoveryLogSeenNegCtl_ = RepointMarkerCount() > repointBaselineNegCtl_;
            smatchet::ui::SetRearmDisabled(false); // restore global state.
            break;
        default:
            break;
        }
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= totalFrames_; }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        int passed = 0;
        if (recoveryLogSeenStuck_)
            ++passed;
        if (recoveryLogSeenCtxLoss_)
            ++passed;
        if (recoveryLogSeenNegCtl_)
            ++passed;
        const int failed = 3 - passed;

        nlohmann::json recoveryLogSeen;
        recoveryLogSeen["stuck"] = recoveryLogSeenStuck_;
        recoveryLogSeen["ctxLoss"] = recoveryLogSeenCtxLoss_;
        recoveryLogSeen["negativeControl"] = recoveryLogSeenNegCtl_;

        nlohmann::json out;
        out["scenario"] = Name();
        out["aborted"] = false;
        out["framesAdvanced"] = framesAdvanced_;
        out["recoveryLogSeen"] = recoveryLogSeen;
        out["passed"] = passed;
        out["failed"] = failed;
        out["ok"] = (passed == 3);
        return out;
    }

  private:
    int totalFrames_ = 10;
    int framesAdvanced_ = 0;
    int repointBaselineStuck_ = 0;
    int repointBaselineCtxLoss_ = 0;
    int repointBaselineNegCtl_ = 0;
    bool recoveryLogSeenStuck_ = false;
    bool recoveryLogSeenCtxLoss_ = false;
    bool recoveryLogSeenNegCtl_ = false;
};

} // namespace

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeMobileTextureGuardScenario() {
    return std::make_unique<smatchet::cmd::MobileTextureGuardScenario>();
}
