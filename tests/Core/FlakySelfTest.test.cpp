// FlakySelfTest.test.cpp — self-test for the SMATCHET_QUARANTINED_TEST_CASE /
// SMATCHET_QT_CHECK quarantine mechanism (build-quality-velocity-hardening #32).
//
// This file demonstrates and verifies the two behaviours:
//
//   1. Normal gating run (SMATCHET_RUN_FLAKY unset):
//      - The quarantined test body RUNS fully (assertions execute).
//      - Assertion failures downgrade to WARN: they print but do NOT fail the
//        suite. ctest exits 0.
//
//   2. Force-include run (SMATCHET_RUN_FLAKY=1):
//      - The quarantined test body RUNS fully.
//      - Assertion failures are CHECK-level: they fail the suite. ctest exits 1.
//
// The self-verifying tests below exercise the MECHANISM, not the macro alone:
//   - A "synthetic flaky" scenario: a seeded always-failing assertion inside
//     a quarantined test case. This proves the env-flip behaviour.
//   - A normal (non-quarantined) test case that verifies RunFlakyEnvSet() itself.
//
// NOTE: The "always-failing" quarantined test below DOES produce a WARN output
// in the gating run — the failure message is logged but ctest still exits 0.
// In the force-include run (SMATCHET_RUN_FLAKY=1) that same test causes ctest
// to exit 1, which is the expected and correct behavior.

#include <doctest/doctest.h>

#include "QuarantineTestCase.h"

#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// Verify RunFlakyEnvSet() itself.
// ---------------------------------------------------------------------------

TEST_CASE("QuarantineMechanism: RunFlakyEnvSet reflects SMATCHET_RUN_FLAKY env") {
    // We cannot set the env var inside the test in a portable C++14 way
    // (putenv/setenv are not standard), so this case reads the current state
    // and verifies that the function returns a consistent bool.
    const char* val = std::getenv("SMATCHET_RUN_FLAKY");
    const bool expected = (val != nullptr && val[0] != '\0');
    CHECK(smatchet_quarantine_detail::RunFlakyEnvSet() == expected);
}

// ---------------------------------------------------------------------------
// Quarantined test: seeded-always-failing assertion.
//
// Behaviour:
//   - env unset  → WARN (suite still passes; failure logged as [WARNING])
//   - env=1      → CHECK (suite fails; ctest exits 1)
//
// This is the canonical demonstration of Item #32.
// ---------------------------------------------------------------------------

// SMATCHET_QUARANTINED_TEST_CASE registers: name, reason, owner.
// The name has " [quarantined:owner]" appended automatically.
SMATCHET_QUARANTINED_TEST_CASE(
    "QuarantineMechanism: synthetic always-failing assertion",
    "Seeded failure to demonstrate the env-flip; not a real flaky test",
    "test-rig") {
    // This assertion ALWAYS evaluates to false (1 == 2).
    // Env-unset → WARN → suite passes.
    // Env=1     → CHECK → suite fails.
    SMATCHET_QT_CHECK(1 == 2);
}

// ---------------------------------------------------------------------------
// Verify that non-quarantined CHECK still works normally (sanity).
// ---------------------------------------------------------------------------

TEST_CASE("QuarantineMechanism: ordinary non-quarantined CHECK is unaffected") {
    // Always true — must never fail regardless of SMATCHET_RUN_FLAKY.
    CHECK(1 == 1);
    CHECK_FALSE(1 == 2);
}

// ---------------------------------------------------------------------------
// Verify SMATCHET_QT_CHECK_FALSE and SMATCHET_QT_CHECK_EQ forms.
// These use a true condition, so they must pass even when SMATCHET_RUN_FLAKY=1.
// ---------------------------------------------------------------------------

SMATCHET_QUARANTINED_TEST_CASE(
    "QuarantineMechanism: QT_CHECK_FALSE and QT_CHECK_EQ pass on true conditions",
    "Verifies the non-trivial macro forms with a passing assertion",
    "test-rig") {
    SMATCHET_QT_CHECK_FALSE(1 == 2);       // always true condition → must pass
    SMATCHET_QT_CHECK_EQ(42, 42);          // always true → must pass
}
