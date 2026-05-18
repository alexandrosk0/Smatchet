#ifndef SMATCHET_HARNESS_RUN_STATE_H
#define SMATCHET_HARNESS_RUN_STATE_H

// Lifecycle FSM for one agentic-handoff invocation. The state machine lives
// outside `AgenticHandoffController` so it stays a pure, testable C++14
// helper with no AGENTIC-gated symbols — the controller (H4) plumbs it
// through the runner callbacks, audit-trails every transition, and rejects
// any callback that requests a disallowed jump.
//
// Why a separate FSM unit: the runner emits state names as strings
// (ICodingHarnessRunner::StateChangeCallback) — without this validator the
// controller would silently honour "Spawning → Complete" or worse. The
// FSM here is the integrity boundary; loosening it for any reason defeats
// the load-bearing piece. See AGENTS.md § Anti-deception note.
//
// Header is always-compiled — declarations only, no AGENTIC-dependent
// symbols. Consumers may still `#if defined(SMATCHET_WITH_AGENTIC)` their
// include for stylistic consistency, but it is not required.

#include <string>

namespace CodingHarness {

// Allowed transitions per the FSM (canonical table — keep in sync with
// docs/CONTEXT.md § HarnessRunState):
//
//   Pending      -> Spawning
//   Spawning     -> Running, Failed
//   Running      -> AwaitingUser, PrOpen, Failed, Cancelled
//   AwaitingUser -> Running, Failed, Cancelled
//   PrOpen       -> Iterating, Complete, Failed, Cancelled
//   Iterating    -> PrOpen, Complete, Failed, Cancelled
//   Complete / Failed / Cancelled  -> (terminal)
//
// Terminal states reject all outbound transitions including self-loops.
enum class RunState {
    Pending,
    Spawning,
    Running,
    AwaitingUser,
    PrOpen,
    Iterating,
    Complete,
    Failed,
    Cancelled,
};

// Stable canonical string per state. Matches the literals emitted by the
// runner's StateChangeCallback (see ClaudeCodeLocalRunner.cpp). Unknown
// returns "Unknown".
const char* RunStateToString(RunState s);

// Inverse of RunStateToString. Returns false + leaves `out` untouched when
// `s` is not a known literal. Used by the controller when receiving a
// runner-side state-name callback to enforce the FSM at the boundary.
bool ParseRunState(const std::string& s, RunState& out);

// Predicate over the FSM table above. Returns true iff `from -> to` is an
// allowed transition. The controller validates every state change against
// this predicate before audit-trailing + applying it.
bool IsTransitionAllowed(RunState from, RunState to);

// True when `s` is one of the terminal states (Complete / Failed /
// Cancelled). Convenience wrapper for controller code that needs to suppress
// further mutation post-terminal.
bool IsTerminal(RunState s);

} // namespace CodingHarness

#endif // SMATCHET_HARNESS_RUN_STATE_H
