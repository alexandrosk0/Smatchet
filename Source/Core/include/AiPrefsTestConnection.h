#ifndef SMATCHET_AI_PREFS_TEST_CONNECTION_H
#define SMATCHET_AI_PREFS_TEST_CONNECTION_H

#include "AiTypes.h"

class IAppThreading; // narrow AppController threading facet — see Commands/IAppThreading.h
struct UiDrawSession;

namespace AiPrefsTestConnection {

// Kicks off the async Test-connection probe used by the Preferences > Assistant
// tab + by bucket-E TU `ai_prefs_autosave_flow.test.cpp`.
// Lifted from the inline `runProbe` lambda that historically lived inside
// `SmatchetPreferencesUi.cpp`'s `BeginTabItem("Assistant")`. The extraction
// lets bucket-E tests drive the probe without traversing
// `Preferences -> Assistant tab -> Test connection click`, which is not
// reachable through the engine's `ItemClick` path in the current bucket-E
// harness.
// Behaviour, verbatim from the original runProbe:
//   - Snapshots `d.cfg` field values by value (so concurrent UI edits during
//     the probe don't corrupt the worker's view).
//   - Sets `d.assistantPrefsTestInFlight = true`, `...TestResult = "Testing..."`,
//     `...TestResultType = 0`.
//   - Creates a fresh `assistantPrefsTestCancel` shared_ptr<atomic<bool>>.
//   - Spawns a detached worker thread that runs ProbeReachability +
//     SendStreaming on a 1-token "ping", catching exceptions.
//   - Posts the result back via `app.PostToMainThread`. The posted lambda
//     checks the cancel atom first; on cancel it returns without mutating any
//     `g_ui.*Test*` field (so the cancel-on-prefs-close branch in
//     `SmatchetDrawPreferencesPanel` can clear state and have the late dispatch
//     short-circuit).
// Threading: must be called from the UI thread.
// DRIFT WARNING: as long as `SmatchetPreferencesUi.cpp`'s `runProbe` lambda
// remains inline (pending the follow-up PR after whisper-dictation-phase-f
// merges), this function MUST stay in lock-step with that lambda's body.
// docs/plans/shipped/ai-client-test-override.md tracks the rewire follow-up.
void TriggerProbe(UiDrawSession& d, IAppThreading& app, AiProvider provider);

} // namespace AiPrefsTestConnection

#endif
