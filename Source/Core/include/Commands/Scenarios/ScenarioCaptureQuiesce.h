#ifndef SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_QUIESCE_H
#define SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_QUIESCE_H

namespace smatchet {
namespace cmd {

// Quiesce the ambient, wall-clock-driven UI chrome that would otherwise land in
// a screenshot capture at an arbitrary animation phase.
//
// Covers three surfaces: live toasts, the app-update modal, and the grid's
// unsaved-layout strip (details in the .cpp).
//
// The first is: dismiss every LIVE toast (history untouched). The startup
// connectivity poll pushes timed "Syncing / Refreshing issues from Tracker..."
// toasts whose fade alpha + text advance with wall-clock time, so two captures
// of the same scenario disagree by far more than the L_inf <= 4 diff tolerance
// (observed 97-113 in the bottom-right toast strip). Call once per warm-up
// frame from a screenshot scenario's OnFrame: the toast draw in
// drawGlobalOverlays runs AFTER the scenario tick within SmatchetUI::Draw, so a
// same-frame clear keeps them off the capture, and re-calling every frame
// catches toasts pushed between ticks.
//
// Declared here (not in ScenarioCaptureSizing.h) and defined out-of-line so the
// Commands/Scenarios headers stay free of Ui includes — a Scenarios header
// including a Ui header is an include-cycle layer back-edge; the Ui include
// lives in the .cpp (the sanctioned Scenario->Ui seam).
void QuiesceCaptureFrame();

// Undo the update-check suppression QuiesceCaptureFrame() applied, restoring the
// values that were live before the first quiesce of this scenario run. Call from
// every caller's OnFinish AND OnCancel (the runner invokes exactly one of them).
//
// Without this, an in-process `scenario.run` on a long-lived instance would leave
// UpdateCheckEnabled=false + appUpdateStartupCheckStarted=true for the rest of the
// session, silently disabling startup update checks. Harmless in the ephemeral
// --spawn child that the screenshot-diff driver uses (it exits after the capture),
// but a real leak for a dev driving scenarios against their running app.
//
// Idempotent and safe to call without a preceding quiesce: it no-ops unless a
// snapshot is armed, and taking the snapshot is itself first-call-only, so the
// per-frame re-quiesce never re-captures already-suppressed values.
void RestoreCaptureQuiesce();

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_QUIESCE_H
