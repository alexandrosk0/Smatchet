// CommandPaletteFuzzyScenario — see header for rationale. Drives the command
// palette open + pre-filtered through the UiDrawSession::requestCommandPalette*
// flag pair (consumed in SmatchetUI::Draw on the next frame). After a small
// warm-up window the scenario triggers `debug.window.screenshot` so the post-
// swap handler writes a PPM the bash driver can diff against
// tests/golden/command_palette_fuzzy.ppm.

#include "Commands/Scenarios/IScenario.h"

#include "AppController.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: AppController.h closed the transitive json door (json_fwd); this TU uses nlohmann::json directly.
#include "Commands/Scenarios/ScenarioCaptureSizing.h"
#include "Commands/Scenarios/ScenarioScreenshotPath.h"
#include "Logger.h"
#include "SmatchetUiSession.h"

#include <string>

// Same singleton extern shim as DockGapSentinelScenario.cpp + BuiltinCommands_
// Debug.cpp — the SmatchetUiSession.h-side extern is gated on
// SMATCHET_WITH_LUA_AUTOMATION while g_ui itself is defined unconditionally
// in SmatchetUI.cpp.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

class CommandPaletteFuzzyScenario : public IScenario {
  public:
    std::string Name() const override { return "command-palette-fuzzy"; }

    void OnStart(AppController& /*app*/, const nlohmann::json& args, std::string& outErr) override {
        // Shared prologue: warmup/captureSize parse + screenshotPath require/confine
        // (#1566 class). warmupMin=2 — the palette open request lands on frame 0 and
        // the modal renders the next frame, so at least two frames must pass before
        // the screenshot is meaningful. Must precede the latch flip below so a
        // rejected path leaves no session state mutated.
        if (!ConfigureScreenshotScenario("command-palette-fuzzy", args, 8, 2, warmupFrames_, captureSize_,
                                         screenshotPath_, outErr))
            return;
        filter_ = StringArg(args, "filter", std::string("scenario."));
        // Latch flip must follow the error-return guard above — otherwise
        // an OnStart that errors out leaves BackendHasBeenReachable=true
        // for the session (OnCancel/OnFinish may never run).
        // Headless-spawn ephemeral exes never trip `BackendHasBeenReachable`
        // (no live tracker). SmatchetUI::Draw gates `commandPalette_.Draw`
        // behind that latch, so the palette never renders + our open request
        // is never consumed. Force the latch on for the scenario; restore in
        // OnCancel/OnFinish so the user-config state is untouched. The
        // production latch flow (SmatchetUI.cpp:422-425) only Save()s when
        // the tracker is actually Authenticated — our in-memory flip never
        // persists for ephemeral spawns.
        savedBackendReachable_ = g_ui.cfg.BackendHasBeenReachable;
        g_ui.cfg.BackendHasBeenReachable = true;
        // Stage the open + filter request now; SmatchetUI::Draw will consume
        // it on its next frame (which is also the scenario's first OnFrame).
        g_ui.requestCommandPaletteOpen = true;
        g_ui.requestCommandPaletteFilter = filter_;
    }

    void OnFrame(AppController& /*app*/, int /*frameIndex*/) override {
        // No per-frame mutation needed — the palette stays open until either
        // the user dismisses it or we tear down at scenario end.
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= warmupFrames_; }

    void OnCancel(AppController& /*app*/) override {
        // Clear any pending request that hadn't been consumed yet so a
        // cancelled scenario doesn't pop the palette mid-stream for the user.
        g_ui.requestCommandPaletteOpen = false;
        g_ui.requestCommandPaletteFilter.clear();
        g_ui.cfg.BackendHasBeenReachable = savedBackendReachable_;
    }

    nlohmann::json OnFinish(AppController& /*app*/) override {
        // Same capture-trigger pattern as DockGapSentinelScenario — the post-
        // swap handler in Source/Standalone/main.cpp will write the PPM
        // after the next SwapBuffers.
        g_ui.requestScreenshotPath = screenshotPath_;
        g_ui.requestScreenshot = true;
        // Restore the backend-reachable latch we forced in OnStart (no-op for
        // an already-reachable session; matches the OnCancel unwind path).
        g_ui.cfg.BackendHasBeenReachable = savedBackendReachable_;

        nlohmann::json out;
        out["scenario"] = Name();
        out["warmupFrames"] = warmupFrames_;
        out["windowWidth"] = captureSize_.Width;
        out["windowHeight"] = captureSize_.Height;
        out["filter"] = filter_;
        out["screenshotPath"] = screenshotPath_;
        out["captureRequested"] = true;
        return out;
    }

  private:
    int warmupFrames_ = 8;
    ScenarioCaptureSize captureSize_;
    std::string filter_;
    std::string screenshotPath_;
    bool savedBackendReachable_ = false;
};

} // namespace

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeCommandPaletteFuzzyScenario() {
    return std::make_unique<smatchet::cmd::CommandPaletteFuzzyScenario>();
}
