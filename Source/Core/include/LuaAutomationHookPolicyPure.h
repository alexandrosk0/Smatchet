#pragma once

// Pure abort-decision policy for the automation worker's Lua instruction-count hook
// in AppController::Impl::RunAutomationJob. Security finding #13 from the 2026-06-13
// audit (Pillar 2 MEDIUM): the original count-hook fired every 50000 instructions but
// its only escape was app_.shuttingDown_, leaving two exposures.
// Exposure A — shutdown / exit hold: the destructor raises automationWorkerShuttingDown_
// BEFORE the blocking automationWorker_.join, but shuttingDown_ is raised only AFTER the
// join, so a long-running automation loop saw no abort signal during the join and the
// destructor blocked on exit indefinitely. The hook must observe the early flag, not just
// shuttingDown_. Exposure B — unbounded runaway: unlike the UI-thread hooks, the automation
// hook had no instruction budget, so a runaway pure-Lua loop ran forever on the worker
// thread; it never blocked the UI thread, but it pinned a core and held shutdown. A bounded
// budget makes a runaway job self-abort even with no shutdown signal.
// The decision is split into this predicate so it is unit-testable on the desktop test build
// with no Lua state, no threads, and no AppController: the caller passes the two flag
// snapshots plus the elapsed-instruction count and acts on the returned reason.
// On the blocking-C++-glue half of #13 such as a synchronous tracker HTTP PUT: an
// instruction-count hook cannot interrupt time spent inside a C++ call because no Lua
// opcodes retire there, so a single blocking glue call is bounded by that call's own HTTP
// timeout, not by this predicate. Automation runs on the dedicated automationWorker_ thread
// and never on the UI thread, so that blocking call cannot freeze the UI and Pillar 2 holds
// by thread affinity; the residual is bounded worker-thread occupancy, which the exposure-A
// shutdown-flag fix bounds at exit. See debt.md automation-shutdown-blocking-glue.
namespace LuaAutomationHookPolicyPure {

// Why an automation job's count-hook decided to abort. kNone means keep running.
enum class AbortReason {
    kNone = 0,
    kShutdown,      // process / worker is tearing down (exposure A)
    kBudgetExceeded // instruction budget exhausted — runaway guard (exposure B)
};

// Decide whether the running automation job should abort this hook tick.
//   shuttingDown          - AppController::shuttingDown_ snapshot (set late, after the join).
//   workerShuttingDown    - automationWorkerShuttingDown_ snapshot (set early, before the join).
//                           Either flag means the job must yield so the dtor's join can complete.
//   instructionsElapsed   - count-hook ticks * hookCountInterval accumulated for THIS job.
//   instructionBudget     - hard ceiling for a single automation job; 0 disables the runaway guard
//                           (shutdown abort still applies).
// Shutdown takes precedence over the budget so a shutting-down app reports the shutdown reason.
inline AbortReason DecideAutomationAbort(bool shuttingDown, bool workerShuttingDown,
                                         unsigned long long instructionsElapsed, unsigned long long instructionBudget) {
    if (shuttingDown || workerShuttingDown) {
        return AbortReason::kShutdown;
    }
    if (instructionBudget != 0 && instructionsElapsed >= instructionBudget) {
        return AbortReason::kBudgetExceeded;
    }
    return AbortReason::kNone;
}

// Stable message text for each abort reason (surfaced to the Lua error path / logs).
inline const char* AbortReasonMessage(AbortReason reason) {
    switch (reason) {
    case AbortReason::kShutdown:
        return "Script execution aborted (shutdown).";
    case AbortReason::kBudgetExceeded:
        return "Script execution aborted (automation instruction budget exceeded).";
    case AbortReason::kNone:
    default:
        return "";
    }
}

} // namespace LuaAutomationHookPolicyPure
