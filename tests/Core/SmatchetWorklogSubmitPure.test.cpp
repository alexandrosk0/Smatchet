// Worklog Save validation + in-flight gate (#2043). The submit itself is now a
// LaunchBackgroundTask + PostToMainThread round-trip (no headless seam); what IS testable is the
// pure accept/reject decision the dialog makes BEFORE dispatching, and the guard that stops a
// second click from queueing a duplicate worklog against an in-flight POST. Both are exercised
// through the production symbols in Ui/SmatchetWorklogSubmitPure.h — the same ones
// TicketFieldEditor.cpp's HandleWorklogSave calls.

#include "Ui/SmatchetWorklogSubmitPure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::worklog::CanSubmitWorklog;
using smatchet::worklog::ValidateWorklogSubmission;
using smatchet::worklog::WorklogSubmitOutstandingFor;

namespace {
// A date string the dialog itself produces (GetCurrentJiraDateTimeString shape).
const char* kGoodDate = "2026-08-16T10:30:00.000+0000";
} // namespace

TEST_CASE("ValidateWorklogSubmission accepts a well-formed time-spent + date pair") {
    CHECK(ValidateWorklogSubmission("2h 30m", kGoodDate).empty());
    CHECK(ValidateWorklogSubmission("45m", kGoodDate).empty());
    CHECK(ValidateWorklogSubmission("1w 2d 3h 4m", kGoodDate).empty());
}

TEST_CASE("ValidateWorklogSubmission rejects an empty / unparseable / zero time spent") {
    // Empty, junk, and an explicit zero all parse to <= 0 seconds and must not dispatch a POST.
    CHECK(ValidateWorklogSubmission("", kGoodDate) == "Invalid Time spent format. Please use e.g. 2h 30m.");
    CHECK(ValidateWorklogSubmission("not-a-duration", kGoodDate) ==
          "Invalid Time spent format. Please use e.g. 2h 30m.");
    CHECK(ValidateWorklogSubmission("0m", kGoodDate) == "Invalid Time spent format. Please use e.g. 2h 30m.");
}

TEST_CASE("ValidateWorklogSubmission rejects an unparseable date started") {
    CHECK(ValidateWorklogSubmission("2h", "") == "Invalid Date started format.");
    CHECK(ValidateWorklogSubmission("2h", "yesterday-ish") == "Invalid Date started format.");
}

TEST_CASE("ValidateWorklogSubmission reports the time-spent failure first") {
    // Both inputs bad: the dialog shows one message, and it is the duration one (unchanged from
    // the pre-async ordering, so the user-visible wording does not shift with this fix).
    CHECK(ValidateWorklogSubmission("junk", "junk") == "Invalid Time spent format. Please use e.g. 2h 30m.");
}

TEST_CASE("CanSubmitWorklog gates a second Save while the POST is in flight") {
    CHECK(CanSubmitWorklog(/*submitInFlight=*/false));
    CHECK_FALSE(CanSubmitWorklog(/*submitInFlight=*/true));
}

TEST_CASE("WorklogSubmitOutstandingFor survives the dialog's per-open reset (#2085 duplicate)") {
    // The bug this pins: `SubmitInFlight` is dialog-instance state and is reset on every open,
    // so Save -> Cancel -> re-open the SAME ticket re-enabled Save while the first POST was
    // still running, creating a second worklog for one intent. The cross-instance id is what
    // the re-open now consults instead of assuming "not in flight".
    CHECK(WorklogSubmitOutstandingFor("PROJ-1", "PROJ-1"));
    // A different ticket is unaffected — one outstanding submit must not lock the whole grid.
    CHECK_FALSE(WorklogSubmitOutstandingFor("PROJ-1", "PROJ-2"));
    // Nothing outstanding.
    CHECK_FALSE(WorklogSubmitOutstandingFor("", "PROJ-1"));
    // Defensive: an empty issue id must never match an outstanding submit, or a dialog opened
    // before its id is populated would come up spuriously disabled.
    CHECK_FALSE(WorklogSubmitOutstandingFor("PROJ-1", ""));
    CHECK_FALSE(WorklogSubmitOutstandingFor("", ""));
}

TEST_CASE("WorklogSubmitOutstandingFor is exact, not prefix or case-folded") {
    // Issue keys share prefixes (PROJ-1 vs PROJ-10); a loose match would wrongly disable Save on
    // a neighbouring ticket for the whole round-trip.
    CHECK_FALSE(WorklogSubmitOutstandingFor("PROJ-1", "PROJ-10"));
    CHECK_FALSE(WorklogSubmitOutstandingFor("PROJ-10", "PROJ-1"));
    CHECK_FALSE(WorklogSubmitOutstandingFor("proj-1", "PROJ-1"));
}
