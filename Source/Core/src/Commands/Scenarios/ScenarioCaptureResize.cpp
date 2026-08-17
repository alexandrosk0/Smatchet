#include "Commands/Scenarios/ScenarioCaptureSizing.h"

#include "SmatchetUiSession.h"

// Same unconditional-extern shim the scenario TUs use: g_ui is defined in
// SmatchetUI.cpp without a guard while the header-side extern is gated on
// SMATCHET_WITH_LUA_AUTOMATION. Kept out of ScenarioCaptureSizing.cpp so the
// pure parse half links into the doctest rig without the UI globals.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

void RequestScenarioCaptureWindowResize(const ScenarioCaptureSize& size) {
    g_ui.requestWindowWidth = size.Width;
    g_ui.requestWindowHeight = size.Height;
    g_ui.requestWindowResize = true;
    // Arm the capture-size contract for the screenshot this scenario will request. The
    // resize above is only a request — the window system may clamp it — and the scenario
    // cannot check the result itself (its OnFinish merely *asks* for the screenshot; the
    // capture lands frames later). Publishing the expectation here lets the capture path,
    // the one place that sees both numbers, reject a wrong-sized frame (#2092).
    g_ui.expectedCaptureWidth = size.Width;
    g_ui.expectedCaptureHeight = size.Height;
}

} // namespace cmd
} // namespace smatchet
