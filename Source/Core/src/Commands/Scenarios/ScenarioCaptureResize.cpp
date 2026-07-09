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
}

} // namespace cmd
} // namespace smatchet
