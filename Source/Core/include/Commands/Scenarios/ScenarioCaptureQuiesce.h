#ifndef SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_QUIESCE_H
#define SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_QUIESCE_H

#include <functional>

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
// but a real leak for a dev driving scenarios against their running app. Also drops
// the config-save repair hook QuiesceCaptureFrame armed for UpdateCheckEnabled —
// that field is persisted, so the in-session value alone is not the whole unwind
// (#2047).
//
// Idempotent and safe to call without a preceding quiesce: it no-ops unless a
// snapshot is armed, and taking the snapshot is itself first-call-only, so the
// per-frame re-quiesce never re-captures already-suppressed values.
void RestoreCaptureQuiesce();

// Defer `fn` to the START of the next drawn frame — i.e. past the post-swap
// screenshot handler that captures the CURRENT frame.
//
// A screenshot scenario's OnFinish runs inside SmatchetUI::drawPreWindowOverlays
// (the runner tick lives there), which is BEFORE any window draws. So a scenario
// that unwinds its pinned state directly in OnFinish un-pins it a few hundred
// lines ahead of the very frame the capture records: the User Info scenarios lost
// their identity + WhisperSetupCompleted pins that way and captured a first-run
// whisper-consent banner over an empty pane. Queue the unwind here instead and it
// lands one frame later, with the capture already on disk.
//
// The callback must own everything it touches by value: the runner destroys the
// scenario (`active_.reset()`) as soon as OnFinish returns, so capturing `this`
// dangles. Callbacks run once, on the UI thread, then the queue is cleared.
//
// A process that exits before the next frame (the --spawn screenshot child) never
// runs them. Do NOT read that as "the unwind is optional": the callbacks restore
// PERSISTED `TrackerConfig` fields (`GitHubPat`, the git-repo legs, `UiMode`,
// `VcsFeedLayout`, `WhisperSetupCompleted`, and `UpdateCheckEnabled` via
// `RestoreCaptureQuiesce`), not just session state. This header used to claim the
// opposite, and that false premise is what made "exit before the drain" look safe
// while it was in fact writing a CLEARED GitHub PAT to the user's real config
// (#2047). Durability of those fields is therefore NOT this queue's job — every
// pinning caller must also arm a `config_repair::RegisterTrackerConfigRepair` hook
// (Config/TrackerConfigSaveRepair.h) for the fields it pinned. That hook runs at
// `ConfigManager::Save`, the seam every TrackerConfig write funnels through, which
// makes the bad write unissuable no matter when (or whether) the drain runs. The
// queue only handles the frame-timing problem.
void QueuePostCaptureRestore(std::function<void()> fn);

// Drain the QueuePostCaptureRestore queue. Called once per frame from
// SmatchetUI::drawPreWindowOverlays, ahead of the scenario tick.
void RunPendingPostCaptureRestore();

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_SCENARIO_CAPTURE_QUIESCE_H
