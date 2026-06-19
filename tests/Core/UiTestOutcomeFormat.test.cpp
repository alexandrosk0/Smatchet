// UiTestOutcomeFormat doctests -- the pure, engine-free formatter behind the
// bucket-E ui_test.run --spawn child-log diagnostic dump. The ImGui-dependent
// extraction lives in UiTestScenario.cpp; this rig exercises only the pure text
// shaping (Source/Core/include/Commands/Scenarios/UiTestOutcomeFormat.h).

#include "Commands/Scenarios/UiTestOutcomeFormat.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::cmd::FormatUiTestOutcome;
using smatchet::cmd::UiTestOutcomeRow;

namespace {
bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}
} // namespace

TEST_CASE("FormatUiTestOutcome -- all-pass run (tested==passed, no failures)") {
    const std::string out = FormatUiTestOutcome(2, 2, "Views", true, {});
    CHECK(Contains(out, "[ui_test] run complete: filter='Views' tested=2 passed=2 failed=0"));
    CHECK(Contains(out, "[ui_test] Tests Result: OK (2/2 passed)"));
    // No KO lines and no defensive "failure reported by counts" note on a clean run.
    CHECK_FALSE(Contains(out, "[ui_test] KO:"));
    CHECK_FALSE(Contains(out, "no Error-status test found"));
}

TEST_CASE("FormatUiTestOutcome -- a single failure emits KO line + log excerpt + Errors result") {
    std::vector<UiTestOutcomeRow> failures;
    UiTestOutcomeRow row;
    row.category = "Views";
    row.name = "Foo";
    row.errorLog = "assert failed: item not found at SomeWindow/btn";
    failures.push_back(row);

    const std::string out = FormatUiTestOutcome(1, 0, "Views", true, failures);
    CHECK(Contains(out, "[ui_test] run complete: filter='Views' tested=1 passed=0 failed=1"));
    CHECK(Contains(out, "[ui_test] KO: Views/Foo"));
    CHECK(Contains(out, "assert failed: item not found at SomeWindow/btn"));
    CHECK(Contains(out, "[ui_test] Tests Result: Errors (0/1 passed)"));
}

TEST_CASE("FormatUiTestOutcome -- empty filter renders as (all)") {
    const std::string out = FormatUiTestOutcome(0, 0, "", true, {});
    CHECK(Contains(out, "filter='(all)'"));
}

TEST_CASE("FormatUiTestOutcome -- not-queued run carries the no-tests-queued suffix") {
    const std::string out = FormatUiTestOutcome(0, 0, "Bar", false, {});
    CHECK(Contains(out, "filter='Bar' tested=0 passed=0 failed=0 (no tests queued/registered)"));
    CHECK(Contains(out, "[ui_test] Tests Result: OK (0/0 passed)"));
}

TEST_CASE("FormatUiTestOutcome -- counts say failure but no rows -> defensive note") {
    // tested>passed with empty failures: surface the inconsistency rather than
    // emit a silent log (mirrors the engine-walk branch in the scenario).
    const std::string out = FormatUiTestOutcome(3, 1, "X", true, {});
    CHECK(Contains(out, "failed=2"));
    CHECK(Contains(out, "[ui_test] failure reported by counts but no Error-status test found"));
    CHECK(Contains(out, "[ui_test] Tests Result: Errors (1/3 passed)"));
}
