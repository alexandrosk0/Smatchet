#pragma once

// Pure abort-decision policy for the automation worker's Lua instruction-count hook
// (AppController::Impl::RunAutomationJob in AppController_LuaBindings.cpp).
//
// Security finding #13 (SECURITY_AUDIT_2026-06-13, Pillar 2 MEDIUM): the original count-hook
// fired every 50000 instructions but its ONLY escape was `app_.shuttingDown_`. Two exposures:
//
//   (A) Shutdown / exit hold. `~AppController` sets `automationWorkerShuttingDown_` BEFORE the
//       blocking `automationWorker_.join()`, but `shuttingDown_` is set only AFTER the join. A
//       long-running automation loop therefore never saw an abort signal during the join, and the
//       dtor blocked on exit indefinitely. The hook must observe `automationWorkerShuttingDown_`
//       (the flag actually raised before the join) — not just `shuttingDown_`.
//
//   (B) Unbounded runaway. Unlike the UI-thread hooks (LuaHookGuard / console snippet / setup
//       script), which self-terminate after a fixed instruction budget, the automation hook had
//       NO budget — a runaway pure-Lua loop ran forever on the worker thread (it never blocked the
//       UI thread, which is the worker-thread architecture's mitigation, but it pinned a core and
//       held shutdown). A bounded budget makes a runaway job self-abort even with no shutdown
//       signal, matching the UI-thread contract.
//
// This predicate is split out so the decision is unit-testable on the desktop test build with no
// Lua state, no threads, and no AppController — the caller passes the two flag snapshots plus the
// elapsed-instruction count, and acts on the returned reason.
//
// NOTE on the blocking-C++-glue half of #13 (e.g. JiraClient::UpdateIssueFields' synchronous HTTP
// PUT): an instruction-count hook cannot interrupt time spent inside a C++ call (no Lua opcodes
// retire there), so a single blocking glue call is bounded by that call's own HTTP timeout, not by
// this predicate. Because automation runs on the dedicated `automationWorker_` thread and never on
// the UI thread, that blocking call cannot freeze the UI (Pillar 2 satisfied by thread affinity);
// the residual is bounded worker-thread occupancy, which the shutdown-flag fix (A) bounds at exit.
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
