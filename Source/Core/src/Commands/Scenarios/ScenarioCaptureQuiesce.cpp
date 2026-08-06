#include "Commands/Scenarios/ScenarioCaptureQuiesce.h"

#include "SmatchetUiSession.h"
#include "Ui/SmatchetToast.h"

// g_ui — unconditional extern. Defined in SmatchetUI.cpp without a
// SMATCHET_WITH_LUA_AUTOMATION guard; the header-side extern in
// SmatchetUiSession.h is gated, so re-declare it here, matching the sibling
// screenshot scenarios' shim.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

void QuiesceCaptureFrame() {
    // Live toasts only — the bounded session history stays intact so a scenario
    // run never hides a real notification from the Notification Center.
    SmatchetToastManager::Instance().DismissAllLive();

    // App-update modal: the startup check is an async GitHub round-trip whose
    // completion frame depends on the network, so on a fast connection it lands
    // mid-capture and DrawAppUpdateModal paints a full "Update Available" dialog
    // (version strings + live release notes) over the whole frame — a
    // whole-frame L_inf ~240 diff that flips run to run, and whose text would
    // rot on every release even when it did land. Suppress it for every capture:
    //
    //  * UpdateCheckEnabled=false stops a check that has not started yet
    //    (SmatchetUI::drawPerFrameTicksAndHandlers gates the startup kick on it),
    //    and appUpdateStartupCheckStarted latches that shut belt-and-braces.
    //  * appUpdateCheckInFlight=false makes DrainAppUpdateCheck early-out for a
    //    check that was ALREADY in flight before the scenario started — the
    //    common case, since the kick happens on the first drawn frame and the
    //    scenario only starts once the MCP port is up. This runs before the
    //    drain within the frame (the scenario tick is in drawPreWindowOverlays,
    //    called ahead of DrawAppUpdateCheck in SmatchetUI::Draw), so the result
    //    is never consumed and the popup is never opened. The future itself is
    //    left in place, so the worker is joined normally at teardown.
    //  * appUpdateModalOpen=false covers a modal that a drain already opened
    //    before the scenario's first frame.
    //
    // All four writes are session-scoped — nothing reaches the user's config on
    // disk, and the ephemeral --spawn child exits as soon as the capture lands.
    g_ui.cfg.UpdateCheckEnabled = false;
    g_ui.appUpdateStartupCheckStarted = true;
    g_ui.appUpdateCheckInFlight = false;
    g_ui.appUpdateModalOpen = false;
}

} // namespace cmd
} // namespace smatchet
