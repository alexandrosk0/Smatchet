#include "Commands/Scenarios/ScenarioCaptureQuiesce.h"

#include "SmatchetUiSession.h"
#include "Ui/SmatchetToast.h"

#include <imgui.h>

// g_ui — unconditional extern. Defined in SmatchetUI.cpp without a
// SMATCHET_WITH_LUA_AUTOMATION guard; the header-side extern in
// SmatchetUiSession.h is gated, so re-declare it here, matching the sibling
// screenshot scenarios' shim.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

// Pre-quiesce snapshot of the session fields QuiesceCaptureFrame() overwrites,
// armed on the first call of a run and consumed by RestoreCaptureQuiesce().
// File-static rather than per-scenario member state: the suppression is global
// (it targets g_ui), scenarios never nest, and every caller runs on the UI
// thread by contract, so a single latch is both sufficient and race-free.
struct QuiescedSession {
    bool armed = false;
    bool updateCheckEnabled = false;
    bool startupCheckStarted = false;
    bool checkInFlight = false;
    bool modalOpen = false;
};

QuiescedSession g_savedSession;

} // namespace

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
    // disk — and RestoreCaptureQuiesce() puts them back at scenario teardown, so
    // an in-process run against a long-lived instance does not leave update
    // checks disabled. Snapshot on the FIRST quiesce only: the per-frame re-call
    // would otherwise overwrite the snapshot with the suppressed values.
    if (!g_savedSession.armed) {
        g_savedSession.updateCheckEnabled = g_ui.cfg.UpdateCheckEnabled;
        g_savedSession.startupCheckStarted = g_ui.appUpdateStartupCheckStarted;
        g_savedSession.checkInFlight = g_ui.appUpdateCheckInFlight;
        g_savedSession.modalOpen = g_ui.appUpdateModalOpen;
        g_savedSession.armed = true;
    }
    g_ui.cfg.UpdateCheckEnabled = false;
    g_ui.appUpdateStartupCheckStarted = true;
    g_ui.appUpdateCheckInFlight = false;
    g_ui.appUpdateModalOpen = false;

    // Unsaved-layout strip: drawActiveProjectGridPost writes the table's resolved
    // column widths + sort specs back onto the active view and latches viewsDirty,
    // which paints an "Unsaved layout changes to <view>" bar the user never asked
    // for on a first-launch (empty user-data) session. The bar is ~33px tall and
    // pushes every pane below it down, so whether it had latched by the captured
    // frame flipped the whole lower half of the frame (L_inf 240) run to run — the
    // latch frame moves with pane-focus and data-arrival timing.
    //
    // Fence the strip by frame number rather than clearing the dirty flags: the
    // write-back can re-latch on ANY later frame including the capture frame (which
    // gets no OnFrame tick), and clobbering the flags would cost a real user the
    // Save/Discard affordance for genuinely pending edits. The fence reaches two
    // frames ahead so it still covers the capture frame — the frame after the last
    // OnFrame, on which OnFinish stages the screenshot. It auto-expires, so
    // RestoreCaptureQuiesce has nothing to unwind here.
    g_ui.suppressUnsavedLayoutStripUntilFrame = ImGui::GetFrameCount() + 2;
}

void RestoreCaptureQuiesce() {
    if (!g_savedSession.armed) {
        return;
    }
    g_ui.cfg.UpdateCheckEnabled = g_savedSession.updateCheckEnabled;
    g_ui.appUpdateStartupCheckStarted = g_savedSession.startupCheckStarted;
    g_ui.appUpdateCheckInFlight = g_savedSession.checkInFlight;
    g_ui.appUpdateModalOpen = g_savedSession.modalOpen;
    g_savedSession.armed = false;
}

} // namespace cmd
} // namespace smatchet
