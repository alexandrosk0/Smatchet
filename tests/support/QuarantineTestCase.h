// QuarantineTestCase.h — flaky-test quarantine mechanism for the SmatchetTests doctest rig.
//
// ## Purpose
// A test known to be flaky (intermittent) should still RUN for signal, but must NOT
// fail the gating ctest suite. When the root cause is understood and the fix is ready,
// the quarantine is removed — not replaced with a skip.
//
// ## Usage
//
//     SMATCHET_QUARANTINED_TEST_CASE("My flaky test", "reason", "owner") {
//         SMATCHET_QT_CHECK(some_condition);   // degrades to WARN when unquarantined
//         SMATCHET_QT_CHECK_EQ(a, b);
//     }
//
// When SMATCHET_RUN_FLAKY is unset (default gating run):
//   - The test EXECUTES fully (assertions run, output is logged).
//   - Assertion failures are WARN-level: they print but do NOT fail the suite.
//
// When SMATCHET_RUN_FLAKY=1 (force-include mode):
//   - The test EXECUTES fully.
//   - Assertion failures are CHECK-level: they fail the suite immediately.
//
// ## Companion macros
//   SMATCHET_QT_CHECK(expr)       — WARN/CHECK depending on env
//   SMATCHET_QT_CHECK_FALSE(expr) — WARN_FALSE/CHECK_FALSE
//   SMATCHET_QT_CHECK_EQ(a,b)     — WARN_EQ/CHECK_EQ
//
// ## Registry
// See the QUARANTINE REGISTRY block at the bottom of this file.
// Every quarantined test MUST be listed there. An entry without a revisit date
// is not permitted; stale quarantine (past revisit date) must be reviewed.
//
// ## Quarantine is NOT a skip
// The test body ALWAYS runs. The env flag only promotes failures from WARN to CHECK.
// Never quarantine a test that is broken by design — fix it or file it as a product bug.
//
// C++14-compliant; header-only; doctest must already be included by the translation unit.

#ifndef SMATCHET_TESTS_SUPPORT_QUARANTINE_TEST_CASE_H
#define SMATCHET_TESTS_SUPPORT_QUARANTINE_TEST_CASE_H

#include <cstdlib> // std::getenv

namespace smatchet_quarantine_detail {

// Returns true when SMATCHET_RUN_FLAKY is set and non-empty.
inline bool RunFlakyEnvSet() {
    const char* val = std::getenv("SMATCHET_RUN_FLAKY");
    return val != nullptr && val[0] != '\0';
}

} // namespace smatchet_quarantine_detail

// ---------------------------------------------------------------------------
// SMATCHET_QUARANTINED_TEST_CASE
//
// Declares a quarantined test case. The body follows like a normal TEST_CASE.
// The first argument is the test name (same as doctest TEST_CASE).
// The second is the reason string (why it is flaky).
// The third is the owner (team/person responsible for resolving it).
// ---------------------------------------------------------------------------
#define SMATCHET_QUARANTINED_TEST_CASE(name, reason, owner)                     \
    TEST_CASE(name " [quarantined:" owner "]")

// ---------------------------------------------------------------------------
// Assertion macros for use INSIDE a quarantined test body.
// Outside a quarantined body, use CHECK / WARN as normal.
// ---------------------------------------------------------------------------

// SMATCHET_QT_CHECK — degrades to WARN when env-unset, promotes to CHECK when set.
#define SMATCHET_QT_CHECK(expr)                                                 \
    do {                                                                        \
        if (smatchet_quarantine_detail::RunFlakyEnvSet()) {                     \
            CHECK(expr);                                                        \
        } else {                                                                \
            WARN(expr);                                                         \
        }                                                                       \
    } while (false)

// SMATCHET_QT_CHECK_FALSE — inverted form.
#define SMATCHET_QT_CHECK_FALSE(expr)                                           \
    do {                                                                        \
        if (smatchet_quarantine_detail::RunFlakyEnvSet()) {                     \
            CHECK_FALSE(expr);                                                  \
        } else {                                                                \
            WARN_FALSE(expr);                                                   \
        }                                                                       \
    } while (false)

// SMATCHET_QT_CHECK_EQ — equality form.
#define SMATCHET_QT_CHECK_EQ(a, b)                                              \
    do {                                                                        \
        if (smatchet_quarantine_detail::RunFlakyEnvSet()) {                     \
            CHECK_EQ(a, b);                                                     \
        } else {                                                                \
            WARN_EQ(a, b);                                                      \
        }                                                                       \
    } while (false)

// ===========================================================================
// QUARANTINE REGISTRY
// Every quarantined test must appear here. No entry without a revisit date.
// Format: name | reason | owner | revisit
// ===========================================================================
//
// (empty — no real flaky tests are known at time of writing; the mechanism is
//  demonstrated by FlakySelfTest.test.cpp which uses a synthetic seeded failure.)
//
// ===========================================================================

#endif // SMATCHET_TESTS_SUPPORT_QUARANTINE_TEST_CASE_H
