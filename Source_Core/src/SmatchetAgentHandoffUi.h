#pragma once

// SmatchetAgentHandoffUi — H8 of the agentic-flow plan.
// Renders the in-flight agentic-handoff table + per-row detail pane (question +
// clarification reply box + PR-URL launcher + worktree-open).
//
// The panel reads via `AgenticHandoffController::SnapshotActive()` (cached on a
// ~1 Hz tick — same cadence as the T6 proposals UI; per-frame snapshot at
// 144 Hz would copy under the controller mutex for zero perceptible benefit
// since state transitions arrive at human / network timescale). Submit and
// Cancel call the controller directly on the UI thread: both methods are
// bookkeeping-only — the controller posts the actual runner work via its
// worker dispatcher seam.
//
// Source-list-conditional on SMATCHET_WITH_AGENTIC in the root CMakeLists.txt;
// the OFF build never compiles this TU. Consumers must
// `#if defined(SMATCHET_WITH_AGENTIC)` every include + call site.

class AppController;
struct UiDrawSession;

namespace SmatchetAgentHandoffUi {

// Renders the Agent-handoffs window when `d.showAgentHandoffs == true`. Owns
// its own ImGui::Begin / ImGui::End — caller invokes once per frame from
// SmatchetUI::Draw and the function early-returns when the toggle is off.
//
// The function maintains its own ~1 Hz refresh cadence on a TU-static cache —
// the controller's SnapshotActive() is cheap (deep copy under one mutex) but
// the panel does not need 144 Hz freshness for handoff state, which changes
// at human + network latency.
void Render(AppController& app, UiDrawSession& d);

} // namespace SmatchetAgentHandoffUi
