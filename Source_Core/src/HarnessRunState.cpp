// HarnessRunState — pure FSM helpers. Always-compiled; no AGENTIC gate so
// the OFF build can still link AgenticTriageController tests against this
// translation unit if needed.

#include "HarnessRunState.h"

namespace CodingHarness {

const char* RunStateToString(RunState s) {
    switch (s) {
    case RunState::Pending:      return "Pending";
    case RunState::Spawning:     return "Spawning";
    case RunState::Running:      return "Running";
    case RunState::AwaitingUser: return "AwaitingUser";
    case RunState::PrOpen:       return "PrOpen";
    case RunState::Iterating:    return "Iterating";
    case RunState::Complete:     return "Complete";
    case RunState::Failed:       return "Failed";
    case RunState::Cancelled:    return "Cancelled";
    }
    return "Unknown";
}

bool ParseRunState(const std::string& s, RunState& out) {
    if (s == "Pending")      { out = RunState::Pending; return true; }
    if (s == "Spawning")     { out = RunState::Spawning; return true; }
    if (s == "Running")      { out = RunState::Running; return true; }
    if (s == "AwaitingUser") { out = RunState::AwaitingUser; return true; }
    if (s == "PrOpen")       { out = RunState::PrOpen; return true; }
    if (s == "Iterating")    { out = RunState::Iterating; return true; }
    if (s == "Complete")     { out = RunState::Complete; return true; }
    if (s == "Failed")       { out = RunState::Failed; return true; }
    if (s == "Cancelled")    { out = RunState::Cancelled; return true; }
    return false;
}

bool IsTerminal(RunState s) {
    return s == RunState::Complete || s == RunState::Failed || s == RunState::Cancelled;
}

bool IsTransitionAllowed(RunState from, RunState to) {
    // Terminal states are sinks — including self-loops. A runner that fires
    // "Complete" twice is a bug; the FSM rejects the second one so the audit
    // trail does not record a duplicate event.
    if (IsTerminal(from)) {
        return false;
    }
    switch (from) {
    case RunState::Pending:
        return to == RunState::Spawning;
    case RunState::Spawning:
        return to == RunState::Running || to == RunState::Failed;
    case RunState::Running:
        return to == RunState::AwaitingUser
            || to == RunState::PrOpen
            || to == RunState::Failed
            || to == RunState::Cancelled;
    case RunState::AwaitingUser:
        return to == RunState::Running
            || to == RunState::Failed
            || to == RunState::Cancelled;
    case RunState::PrOpen:
        return to == RunState::Iterating
            || to == RunState::Complete
            || to == RunState::Failed
            || to == RunState::Cancelled;
    case RunState::Iterating:
        return to == RunState::PrOpen
            || to == RunState::Complete
            || to == RunState::Failed
            || to == RunState::Cancelled;
    case RunState::Complete:
    case RunState::Failed:
    case RunState::Cancelled:
        return false; // terminal, handled above; switch exhaustiveness.
    }
    return false;
}

} // namespace CodingHarness
