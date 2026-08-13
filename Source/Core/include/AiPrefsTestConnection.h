#ifndef SMATCHET_AI_PREFS_TEST_CONNECTION_H
#define SMATCHET_AI_PREFS_TEST_CONNECTION_H

#include "AiTypes.h"

#include <atomic>
#include <memory>
#include <string>

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
void TriggerProbe(UiDrawSession& d, IAppThreading& app, AiProvider provider);

// Worker-thread probe body: reachability check on the cheap model-listing endpoint,
// then a 1-token "ping" chat handshake against the configured model so
// model-not-found / chat-disabled / missing-loaded-model errors surface BEFORE the
// user types their first real prompt (a healthy model-list endpoint does not imply a
// healthy chat endpoint against a loaded model). Returns an empty string on success,
// a user-facing error message otherwise.
//
// Catches every exception — MakeAiClient / ProbeReachability / SendStreaming all run
// third-party transport (cpr/libcurl) plus SSE-parser code, and an escape would
// propagate out of the background task and call std::terminate. `logTag` prefixes the
// LOG_WARN emitted on that path.
//
// Threading: call from a background task, never the UI thread. The caller owns
// publishing the verdict — the two callers commit it differently (this component
// writes cfg; the Preferences Assistant tab writes its working copy behind a
// generation guard), which is why only the probe body is shared.
std::string RunProbe(AiProvider provider, const AiClientConfig& clientCfg, const std::string& modelId,
                     const std::shared_ptr<std::atomic<bool>>& cancel, const char* logTag);

} // namespace AiPrefsTestConnection

#endif
