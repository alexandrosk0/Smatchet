// Worklog Save validation + in-flight gate (#2043). The submit itself is now a
// LaunchBackgroundTask + PostToMainThread round-trip (no headless seam); what IS testable is the
// pure accept/reject decision the dialog makes BEFORE dispatching, and the guard that stops a
// second click from queueing a duplicate worklog against an in-flight POST. Both are exercised
// through the production symbols in Ui/SmatchetWorklogSubmitPure.h — the same ones
// TicketFieldEditor.cpp's HandleWorklogSave calls.
//
// The key decisions the worklog dialog shares with the other dialogs that hold typed text
// (Ui/DialogKeysPure.h: Esc, the caret request, a dismissed popup coming back) are covered here too.

#include "Ui/DialogKeysPure.h"
#include "Ui/SmatchetWorklogSubmitPure.h"

#include <doctest/doctest.h>

#include <set>
#include <string>

using smatchet::dialogkeys::CaretRequest;
using smatchet::dialogkeys::DismissedPopupComesBack;
using smatchet::dialogkeys::EscapeAction;
using smatchet::dialogkeys::ResolveCaretRequest;
using smatchet::dialogkeys::ResolveEscape;
using smatchet::worklog::CanSubmitWorklog;
using smatchet::worklog::ClearWorklogSubmitInFlight;
using smatchet::worklog::MarkWorklogSubmitInFlight;
using smatchet::worklog::ShouldRefocusTimeSpentAfterSaveKey;
using smatchet::worklog::StalePostBackReleasesDialogSubmit;
using smatchet::worklog::ValidateWorklogSubmission;
using smatchet::worklog::WorklogSubmitInFlightSet;
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

TEST_CASE("ValidateWorklogSubmission accepts the natural spellings of a duration (#38)") {
    // Each of these used to be refused with "Invalid Time spent format" or, worse, mean something else.
    for (const char* typed : {"1.5h", "90 m", "90", "1h30", "1:30", "2h, 30m", "1 hour 15 mins", ".5d"}) {
        INFO("typed: " << std::string(typed));
        CHECK(ValidateWorklogSubmission(typed, kGoodDate).empty());
    }
}

TEST_CASE("ValidateWorklogSubmission rejects an empty / unparseable / zero time spent, and says what to fix") {
    // Nothing logged: ask for a time above zero.
    const char* kAboveZero = "Enter a time above zero, like 30m or 1h 30m.";
    CHECK(ValidateWorklogSubmission("", kGoodDate) == kAboveZero);
    CHECK(ValidateWorklogSubmission("   ", kGoodDate) == kAboveZero);
    CHECK(ValidateWorklogSubmission("0m", kGoodDate) == kAboveZero);
    CHECK(ValidateWorklogSubmission("0", kGoodDate) == kAboveZero);
    // Unreadable: the parser names the part that is wrong.
    CHECK(ValidateWorklogSubmission("not-a-duration", kGoodDate) ==
          "\"not-a-duration\" is not part of a duration. Use numbers with w, d, h or m, like 2d 4h.");
    CHECK(ValidateWorklogSubmission("3x", kGoodDate) == "\"x\" is not a time unit. Use w, d, h or m, like 1h 30m.");
    CHECK(ValidateWorklogSubmission("-1h", kGoodDate) == "A duration cannot be negative. Remove the minus sign.");
}

TEST_CASE("ValidateWorklogSubmission rejects an unparseable date started") {
    CHECK(ValidateWorklogSubmission("2h", "") == "Invalid Date started format.");
    CHECK(ValidateWorklogSubmission("2h", "yesterday-ish") == "Invalid Date started format.");
}

TEST_CASE("ValidateWorklogSubmission reports the time-spent failure first") {
    // Both inputs bad: the dialog shows one message, and it is the duration one (unchanged from
    // the pre-async ordering, so the user-visible wording does not shift with this fix).
    CHECK(ValidateWorklogSubmission("junk", "junk") ==
          "\"junk\" is not part of a duration. Use numbers with w, d, h or m, like 2d 4h.");
}

TEST_CASE("CanSubmitWorklog gates a second Save while the POST is in flight") {
    CHECK(CanSubmitWorklog(/*submitInFlight=*/false));
    CHECK_FALSE(CanSubmitWorklog(/*submitInFlight=*/true));
}

TEST_CASE("WorklogSubmitOutstandingFor survives the dialog's per-open reset (#2085 duplicate)") {
    // The bug this pins: `SubmitInFlight` is dialog-instance state and is reset on every open,
    // so Save -> Cancel -> re-open the SAME ticket re-enabled Save while the first POST was
    // still running, creating a second worklog for one intent. The cross-instance id set is what
    // the re-open now consults instead of assuming "not in flight".
    WorklogSubmitInFlightSet inFlight;
    MarkWorklogSubmitInFlight(inFlight, "PROJ-1");
    CHECK(WorklogSubmitOutstandingFor(inFlight, "PROJ-1"));
    // A different ticket is unaffected — one outstanding submit must not lock the whole grid.
    CHECK_FALSE(WorklogSubmitOutstandingFor(inFlight, "PROJ-2"));
    // Defensive: an empty issue id must never match an outstanding submit, or a dialog opened
    // before its id is populated would come up spuriously disabled.
    CHECK_FALSE(WorklogSubmitOutstandingFor(inFlight, ""));
    // Nothing outstanding.
    const WorklogSubmitInFlightSet none;
    CHECK_FALSE(WorklogSubmitOutstandingFor(none, "PROJ-1"));
    CHECK_FALSE(WorklogSubmitOutstandingFor(none, ""));
}

TEST_CASE("WorklogSubmitOutstandingFor is exact, not prefix or case-folded") {
    // Issue keys share prefixes (PROJ-1 vs PROJ-10); a loose match would wrongly disable Save on
    // a neighbouring ticket for the whole round-trip.
    WorklogSubmitInFlightSet inFlight;
    MarkWorklogSubmitInFlight(inFlight, "PROJ-1");
    CHECK_FALSE(WorklogSubmitOutstandingFor(inFlight, "PROJ-10"));
    CHECK_FALSE(WorklogSubmitOutstandingFor(inFlight, "proj-1"));

    WorklogSubmitInFlightSet other;
    MarkWorklogSubmitInFlight(other, "PROJ-10");
    CHECK_FALSE(WorklogSubmitOutstandingFor(other, "PROJ-1"));
}

TEST_CASE("Two overlapping submits both stay latched (#2167 two-ticket interleaving)") {
    // The regression: with ONE latch slot, Save on A -> Cancel -> open B -> Save (latch := B)
    // -> re-open A seeded "not in flight" and re-enabled Save while A's POST was still running,
    // producing a second worklog for one intent. The set keeps BOTH outstanding.
    WorklogSubmitInFlightSet inFlight;
    MarkWorklogSubmitInFlight(inFlight, "PROJ-A");
    MarkWorklogSubmitInFlight(inFlight, "PROJ-B");
    CHECK(WorklogSubmitOutstandingFor(inFlight, "PROJ-A"));
    CHECK(WorklogSubmitOutstandingFor(inFlight, "PROJ-B"));

    // Each post-back releases its OWN id and leaves the other ticket latched — a late post-back
    // can neither strand nor clear a submit it does not own.
    ClearWorklogSubmitInFlight(inFlight, "PROJ-B");
    CHECK(WorklogSubmitOutstandingFor(inFlight, "PROJ-A"));
    CHECK_FALSE(WorklogSubmitOutstandingFor(inFlight, "PROJ-B"));

    ClearWorklogSubmitInFlight(inFlight, "PROJ-A");
    CHECK_FALSE(WorklogSubmitOutstandingFor(inFlight, "PROJ-A"));
    CHECK(inFlight.empty());
}

TEST_CASE("Marking is idempotent and never latches an empty issue id") {
    // A re-dispatch for the same ticket cannot double-latch (the post-back's single erase must
    // fully release it), and an empty id is never inserted — otherwise a dialog whose id is not
    // yet populated could latch a phantom submit that nothing ever clears.
    WorklogSubmitInFlightSet inFlight;
    MarkWorklogSubmitInFlight(inFlight, "PROJ-1");
    MarkWorklogSubmitInFlight(inFlight, "PROJ-1");
    CHECK(inFlight.size() == 1u);
    ClearWorklogSubmitInFlight(inFlight, "PROJ-1");
    CHECK_FALSE(WorklogSubmitOutstandingFor(inFlight, "PROJ-1"));

    MarkWorklogSubmitInFlight(inFlight, "");
    CHECK(inFlight.empty());

    // Clearing an id that is not latched is a harmless no-op (a post-back landing twice, or
    // after the ticket was already released).
    ClearWorklogSubmitInFlight(inFlight, "PROJ-9");
    CHECK(inFlight.empty());
}

TEST_CASE("StalePostBackReleasesDialogSubmit frees a re-opened same-ticket dialog (#2168)") {
    // Re-open the SAME ticket mid-POST: the open seeded SubmitInFlight=true and burned a fresh
    // generation, so the post-back is stale — but it is THIS ticket's POST that completed, so
    // the seed must be released or Save stays disabled until the dialog is closed and re-opened.
    CHECK(StalePostBackReleasesDialogSubmit(true, "PROJ-1", "PROJ-1"));
    // A dialog on another ticket owns its own state; a late post-back leaves it alone.
    CHECK_FALSE(StalePostBackReleasesDialogSubmit(true, "PROJ-2", "PROJ-1"));
    // No dialog open (Cancel mid-flight): nothing to release.
    CHECK_FALSE(StalePostBackReleasesDialogSubmit(false, "PROJ-1", "PROJ-1"));
    // Defensive: an empty post-back id never matches, even an uninitialised dialog id.
    CHECK_FALSE(StalePostBackReleasesDialogSubmit(true, "", ""));
    CHECK_FALSE(StalePostBackReleasesDialogSubmit(true, "PROJ-1", ""));
}

TEST_CASE("ResolveEscape ignores Esc that is not the dialog's") {
    // Not pressed at all.
    CHECK(ResolveEscape(false, true, true, true, true) == EscapeAction::None);
    // A popup above the dialog (suggestions, date picker, templates) has the keyboard: the key is its.
    CHECK(ResolveEscape(true, false, true, false, true) == EscapeAction::None);
    CHECK(ResolveEscape(true, false, false, true, false) == EscapeAction::None);
}

TEST_CASE("ResolveEscape never cancels around typed text in one press") {
    // Caret in a field, something typed: the first Esc only leaves the field.
    CHECK(ResolveEscape(true, true, true, /*fieldActive=*/true, /*nothingTyped=*/false) == EscapeAction::LeaveField);
    // Out of the field: the next Esc cancels.
    CHECK(ResolveEscape(true, true, true, /*fieldActive=*/false, /*nothingTyped=*/false) == EscapeAction::Cancel);
}

TEST_CASE("ResolveEscape closes an untouched dialog on the first Esc") {
    // The caret starts in Time spent, so an untouched dialog always has a field active.
    CHECK(ResolveEscape(true, true, true, /*fieldActive=*/true, /*nothingTyped=*/true) == EscapeAction::Cancel);
    CHECK(ResolveEscape(true, true, true, /*fieldActive=*/false, /*nothingTyped=*/true) == EscapeAction::Cancel);
}

TEST_CASE("ResolveEscape does not cancel on the Esc that closed a popup above the dialog") {
    // The popup had the keyboard last frame and closed on this Esc; the dialog only just got it back.
    CHECK(ResolveEscape(true, true, /*dialogOwnedKeyboardLastFrame=*/false, false, true) == EscapeAction::None);
    CHECK(ResolveEscape(true, true, /*dialogOwnedKeyboardLastFrame=*/false, false, false) == EscapeAction::None);
}

TEST_CASE("ResolveCaretRequest waits out a held Enter and a drag, and is dropped under a popup") {
    // Enter still held from the key that opened the dialog, or something being dragged: later.
    CHECK(ResolveCaretRequest(/*enterHeld=*/true, false, true) == CaretRequest::Wait);
    CHECK(ResolveCaretRequest(false, /*dragInProgress=*/true, true) == CaretRequest::Wait);
    // Waiting comes first: a request made under a popup is not dropped while Enter is still down.
    CHECK(ResolveCaretRequest(true, false, /*dialogOwnsKeyboard=*/false) == CaretRequest::Wait);
    // The dialog has the keyboard: the caret goes into the field.
    CHECK(ResolveCaretRequest(false, false, true) == CaretRequest::Take);
    // A popup above the dialog has it: the request is forgotten, not kept for when the popup closes.
    CHECK(ResolveCaretRequest(false, false, false) == CaretRequest::Drop);
}

TEST_CASE("DismissedPopupComesBack brings back typed text once, and does not loop") {
    // Nothing typed: the dismissal stands.
    CHECK_FALSE(DismissedPopupComesBack(/*hasUnsavedText=*/false, false, 100, -1, 2));
    // Typed text, never brought back before: it comes back to ask.
    CHECK(DismissedPopupComesBack(true, false, 100, /*lastReopenFrame=*/-1, 2));
    // Already on its way back this frame: no second request.
    CHECK_FALSE(DismissedPopupComesBack(true, /*comingBackNow=*/true, 100, -1, 2));
    // Dismissed again within the settle window of the last return: something keeps closing it.
    CHECK_FALSE(DismissedPopupComesBack(true, false, 100, 99, 2));
    CHECK_FALSE(DismissedPopupComesBack(true, false, 100, 98, 2));
    // Past the settle window, a new dismissal is the user again.
    CHECK(DismissedPopupComesBack(true, false, 100, 97, 2));
}

TEST_CASE("ShouldRefocusTimeSpentAfterSaveKey puts the caret back only on a turned-down value") {
    // Enter on a value that does not parse: back into the field to fix it.
    CHECK(ShouldRefocusTimeSpentAfterSaveKey(true, false, ""));
    CHECK(ShouldRefocusTimeSpentAfterSaveKey(true, false, "junk"));
    CHECK(ShouldRefocusTimeSpentAfterSaveKey(true, false, "0m"));
    // A natural spelling is a good value, not a turned-down one.
    CHECK_FALSE(ShouldRefocusTimeSpentAfterSaveKey(true, false, "1.5h"));
    CHECK_FALSE(ShouldRefocusTimeSpentAfterSaveKey(true, false, "90 m"));
    // The save went out: the dialog is about to close, leave the caret alone.
    CHECK_FALSE(ShouldRefocusTimeSpentAfterSaveKey(true, true, "2h"));
    // A good value turned down for another reason (the date): the error is not in this field.
    CHECK_FALSE(ShouldRefocusTimeSpentAfterSaveKey(true, false, "2h"));
    // Save by the button or Ctrl+Enter: the caret stays where the user had it.
    CHECK_FALSE(ShouldRefocusTimeSpentAfterSaveKey(false, false, "junk"));
}

TEST_CASE("TimeRemainingProblem: blank and zero are fine, an unreadable value is not") {
    using smatchet::worklog::TimeRemainingProblem;
    CHECK_FALSE(TimeRemainingProblem("").IsError());
    CHECK_FALSE(TimeRemainingProblem("   ").IsError());
    CHECK_FALSE(TimeRemainingProblem("0m").IsError());
    CHECK_FALSE(TimeRemainingProblem("1.5h").IsError());
    CHECK_FALSE(TimeRemainingProblem("90").IsError());
    CHECK(TimeRemainingProblem("soon").IsError());
    CHECK(TimeRemainingProblem("5y").Id == TrackerParseExplain::ParseMessageId::DurationUnknownUnit);
}

TEST_CASE("CanonicalDurationText sends the canonical spelling and leaves unreadable text alone") {
    using smatchet::worklog::CanonicalDurationText;
    CHECK(CanonicalDurationText("1.5h") == "1h 30m");
    CHECK(CanonicalDurationText("90 m") == "1h 30m");
    CHECK(CanonicalDurationText("90") == "1h 30m");
    CHECK(CanonicalDurationText("2h 30m") == "2h 30m");
    CHECK(CanonicalDurationText("45m") == "45m");
    CHECK(CanonicalDurationText("") == "");
    CHECK(CanonicalDurationText("soon") == "soon");
}

TEST_CASE("TypedDurationSeconds is 0 for anything that does not read, so the progress bar stays quiet") {
    using smatchet::worklog::TypedDurationSeconds;
    CHECK(TypedDurationSeconds("1.5h") == 5400);
    CHECK(TypedDurationSeconds("90") == 5400);
    CHECK(TypedDurationSeconds("") == 0);
    CHECK(TypedDurationSeconds("soon") == 0);
    CHECK(TypedDurationSeconds("-1h") == 0);
}
