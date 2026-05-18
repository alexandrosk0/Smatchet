#pragma once

// SmatchetAgentProposalsUi — T6 of the agentic-flow plan.
// Renders the Pending agent-proposal queue with per-row Approve / Reject buttons.
//
// First user-facing surface for the agentic flow: every Pending row in
// AgentProposalStore (T4) shows up here, newest-first by createdAtSec. Approve
// and Reject call AgentProposalStore::Transition on the UI thread — the
// underlying SQLite UPDATE is sub-millisecond on the local cache db, so the
// transition runs inline rather than via LaunchBackgroundTask. State transitions
// other than Pending->{Approved,Rejected} are out of scope for T6 (decision #5
// of the plan locks "no auto-start on approve"; the Start-handoff button for
// ImplementIssue rows is H9).
//
// Source-list-conditional on SMATCHET_WITH_AGENTIC in the root CMakeLists.txt;
// the OFF build never compiles this TU. Consumers must `#if defined(SMATCHET_WITH_AGENTIC)`
// every include and call site.

class AppController;
struct UiDrawSession;

namespace SmatchetAgentProposalsUi {

// Renders the Agent-proposals window when `d.showAgentProposals == true`.
// Owns its own ImGui::Begin / ImGui::End — caller invokes once per frame from
// SmatchetUI::Draw and the function early-returns when the toggle is off.
//
// The function is also responsible for the ~1 Hz refresh cadence: a TU-static
// std::vector<AgentProposal> cache is refreshed from AgentProposalStore::Query
// when (a) the cache is empty, (b) the Refresh button is clicked, or (c) the
// last-refresh steady_clock timestamp is older than 1 second. Refresh cadence
// is intentionally human-scale (1 Hz) — proposal arrival is driven by triage
// runs at human timescale, not the 144 Hz UI loop, and per-frame Query() would
// hammer SQLite for zero perceptible benefit.
void Render(AppController& app, UiDrawSession& d);

} // namespace SmatchetAgentProposalsUi
