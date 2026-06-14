#include <doctest/doctest.h>

#include <string>

#include "LuaAutomationHookPolicyPure.h"

using LuaAutomationHookPolicyPure::AbortReason;
using LuaAutomationHookPolicyPure::AbortReasonMessage;
using LuaAutomationHookPolicyPure::DecideAutomationAbort;

// Security finding #13 (SECURITY_AUDIT_2026-06-13, Pillar 2): the automation worker's Lua
// count-hook must abort on shutdown AND on a runaway instruction budget. The two exposures:
//   (A) shutdown hold — the dtor raises automationWorkerShuttingDown_ BEFORE the blocking join,
//       but shuttingDown_ only AFTER, so the hook must observe the worker flag, not just the
//       process flag.
//   (B) unbounded runaway — a pure-Lua infinite loop must self-abort once the budget is exhausted
//       even with no shutdown signal.
// Args to DecideAutomationAbort: (shuttingDown, workerShuttingDown, instructionsElapsed, budget).

namespace {
constexpr unsigned long long kBudget = 500000000ULL;
}

TEST_CASE("DecideAutomationAbort: keeps running with no shutdown and budget not reached") {
    CHECK(DecideAutomationAbort(/*shuttingDown=*/false, /*workerShuttingDown=*/false,
                                /*elapsed=*/0ULL, kBudget) == AbortReason::kNone);
    CHECK(DecideAutomationAbort(false, false, kBudget - 1ULL, kBudget) == AbortReason::kNone);
}

TEST_CASE("DecideAutomationAbort: process shutdown flag aborts (legacy escape still works)") {
    CHECK(DecideAutomationAbort(/*shuttingDown=*/true, /*workerShuttingDown=*/false,
                                /*elapsed=*/0ULL, kBudget) == AbortReason::kShutdown);
}

TEST_CASE("DecideAutomationAbort: worker-shutdown flag aborts — the #13 deadlock fix") {
    // automationWorkerShuttingDown_ is the flag raised BEFORE the dtor's blocking join. Checking
    // shuttingDown_ alone (false here, set only after the join) is exactly the bug; the worker flag
    // must release the running job so the join can complete.
    CHECK(DecideAutomationAbort(/*shuttingDown=*/false, /*workerShuttingDown=*/true,
                                /*elapsed=*/0ULL, kBudget) == AbortReason::kShutdown);
}

TEST_CASE("DecideAutomationAbort: runaway budget exhaustion aborts (exposure B)") {
    CHECK(DecideAutomationAbort(false, false, kBudget, kBudget) == AbortReason::kBudgetExceeded);
    CHECK(DecideAutomationAbort(false, false, kBudget + 1ULL, kBudget) == AbortReason::kBudgetExceeded);
}

TEST_CASE("DecideAutomationAbort: shutdown takes precedence over budget exhaustion") {
    // Both conditions true -> report shutdown (so the message names the real reason at exit).
    CHECK(DecideAutomationAbort(/*shuttingDown=*/true, /*workerShuttingDown=*/false, kBudget + 100ULL, kBudget) ==
          AbortReason::kShutdown);
    CHECK(DecideAutomationAbort(false, /*workerShuttingDown=*/true, kBudget + 100ULL, kBudget) ==
          AbortReason::kShutdown);
}

TEST_CASE("DecideAutomationAbort: budget 0 disables the runaway guard, shutdown still applies") {
    CHECK(DecideAutomationAbort(false, false, 999999999ULL, /*budget=*/0ULL) == AbortReason::kNone);
    CHECK(DecideAutomationAbort(true, false, 999999999ULL, /*budget=*/0ULL) == AbortReason::kShutdown);
}

TEST_CASE("AbortReasonMessage: each reason has stable, non-empty text (and kNone is empty)") {
    CHECK(std::string(AbortReasonMessage(AbortReason::kShutdown)) == "Script execution aborted (shutdown).");
    CHECK(std::string(AbortReasonMessage(AbortReason::kBudgetExceeded)).find("budget") != std::string::npos);
    CHECK(std::string(AbortReasonMessage(AbortReason::kNone)).empty());
}
