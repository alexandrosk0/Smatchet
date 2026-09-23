#include "Commands/Scenarios/ScenarioCaptureQuiesce.h"

#include "Config/TrackerConfigSaveRepair.h" // persisted-field repair hook for UpdateCheckEnabled (#2047)
#include "SmatchetUiSession.h"
#include "Ui/SmatchetToast.h"

#include <imgui.h>

#include <utility>
#include <vector>

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
    /// Token of the config-save repair hook covering UpdateCheckEnabled; 0 when none is armed.
    int configRepairToken = 0;
};

QuiescedSession g_savedSession;

// Unwind callbacks staged by QueuePostCaptureRestore, drained one frame later by
// RunPendingPostCaptureRestore. File-static for the same reason as the snapshot
// above: the state is global (g_ui) and every touch is on the UI thread.
std::vector<std::function<void()>> g_pendingPostCaptureRestores;

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
    // Three of the four writes are session-scoped, but UpdateCheckEnabled is a PERSISTED config
    // field: a save enqueued while the suppression is live would write UpdateCheckEnabled=false to
    // the user's config on disk and silently disable their update checks for good — the same
    // deferred-unwind hazard as the User Info scenarios' cleared PAT (#2047). So arm a config-save
    // repair alongside the snapshot; the chokepoint puts the user's value back on every outgoing
    // snapshot until RestoreCaptureQuiesce() drops it. Snapshot on the FIRST quiesce only: the
    // per-frame re-call would otherwise overwrite the snapshot with the suppressed values (and
    // re-arm a duplicate hook).
    if (!g_savedSession.armed) {
        g_savedSession.updateCheckEnabled = g_ui.cfg.UpdateCheckEnabled;
        g_savedSession.startupCheckStarted = g_ui.appUpdateStartupCheckStarted;
        g_savedSession.checkInFlight = g_ui.appUpdateCheckInFlight;
        g_savedSession.modalOpen = g_ui.appUpdateModalOpen;
        const bool userUpdateCheckEnabled = g_savedSession.updateCheckEnabled;
        g_savedSession.configRepairToken = config_repair::RegisterTrackerConfigRepair(
            [userUpdateCheckEnabled](TrackerConfig& cfg) { cfg.UpdateCheckEnabled = userUpdateCheckEnabled; });
        g_savedSession.armed = true;
    }
    g_ui.cfg.UpdateCheckEnabled = false;
    g_ui.appUpdateStartupCheckStarted = true;
    g_ui.appUpdateCheckInFlight = false;
    g_ui.appUpdateModalOpen = false;

    // Unsaved-layout strip: this used to be latched by drawActiveProjectGridPost's
    // width/sort write-back on a frame that moved with pane-focus + data-arrival timing,
    // flipping a captured frame's whole lower half (L_inf 240) run to run — hence a
    // frame-number fence here to suppress it for the capture window without clobbering a
    // real user's pending-edit Save/Discard affordance. Removed (column-view-save-
    // simplification): layout now autosaves straight into the active view with no dirty
    // flag and no strip at all, so there is nothing left for a first-launch capture to
    // spuriously latch. The one remaining strip (an unsaved QUERY edit) is only ever
    // produced by an explicit user action, never passively during a scripted capture.
}

void RestoreCaptureQuiesce() {
    if (!g_savedSession.armed) {
        return;
    }
    g_ui.cfg.UpdateCheckEnabled = g_savedSession.updateCheckEnabled;
    g_ui.appUpdateStartupCheckStarted = g_savedSession.startupCheckStarted;
    g_ui.appUpdateCheckInFlight = g_savedSession.checkInFlight;
    g_ui.appUpdateModalOpen = g_savedSession.modalOpen;
    config_repair::UnregisterTrackerConfigRepair(g_savedSession.configRepairToken);
    g_savedSession.configRepairToken = 0;
    g_savedSession.armed = false;
}

void QueuePostCaptureRestore(std::function<void()> fn) {
    if (fn) {
        g_pendingPostCaptureRestores.push_back(std::move(fn));
    }
}

void RunPendingPostCaptureRestore() {
    if (g_pendingPostCaptureRestores.empty()) {
        return;
    }
    // Swap out before running: a callback is free to queue another one (or to
    // start a fresh scenario), and mutating the vector we are iterating would
    // invalidate the iterator.
    std::vector<std::function<void()>> pending;
    pending.swap(g_pendingPostCaptureRestores);
    for (size_t i = 0; i < pending.size(); ++i) {
        pending[i]();
    }
}

} // namespace cmd
} // namespace smatchet
