// Worklog Save validation + in-flight gate (#2043). The submit itself is now a
// LaunchBackgroundTask + PostToMainThread round-trip (no headless seam); what IS testable is the
// pure accept/reject decision the dialog makes BEFORE dispatching, and the guard that stops a
// second click from queueing a duplicate worklog against an in-flight POST. Both are exercised
// through the production symbols in Ui/SmatchetWorklogSubmitPure.h — the same ones
// TicketFieldEditor.cpp's HandleWorklogSave calls.
//
// The key decisions the worklog dialog shares with the other dialogs that hold typed text
// (Ui/DialogKeysPure.h: Esc, the caret request, a dismissed popup coming back) are covered here too,
// and so are the shared text field's rules (Ui/SmatchetInputPure.h: growth, trim, focus on appear,
// how an edit ends, the multiline keys, the near-full counter). They live in this TU because a new
// test file needs a tests/CMakeLists.txt edit, and that file is plan-locked by other branches; the
// move to tests/Core/SmatchetInputPure.test.cpp is owed (debt 2026-11-16-text-field-migration-blocked-surfaces).

#include "StringUtil.h" // TrimCopyAsciiWhitespace: the trim the text field must agree with
#include "Ui/DialogKeysPure.h"
#include "Ui/SmatchetInputPure.h"
#include "Ui/SmatchetWorklogSubmitPure.h"

#include <doctest/doctest.h>

#include <cstring>
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

// ---------------------------------------------------------------------------------------------
// The shared text field (Ui/SmatchetInputPure.h).
// ---------------------------------------------------------------------------------------------

namespace ti = smatchet::textinput;

TEST_CASE("GrowStringForEdit sizes the string to the new length and hands back its storage") {
    std::string s = "abc";
    char* buf = ti::GrowStringForEdit(s, 600);
    CHECK(s.size() == 600u);
    CHECK(buf == s.c_str()); // ImGui writes into this exact storage
    CHECK(s.capacity() >= 600u);

    buf = ti::GrowStringForEdit(s, 2);
    CHECK(s.size() == 2u);
    CHECK(buf == s.c_str());
    CHECK(std::string(buf, 2) == "ab"); // shrinking keeps the head

    ti::GrowStringForEdit(s, -5); // never sent by ImGui; read as empty, never as a huge size
    CHECK(s.empty());
}

TEST_CASE("TrimEdgeWhitespace strips space, tab, CR and LF from both ends of a string") {
    std::string s = " \t host.example.com\r\n";
    CHECK(ti::TrimEdgeWhitespace(s));
    CHECK(s == "host.example.com");
    CHECK_FALSE(ti::TrimEdgeWhitespace(s)); // already clean: reports no change
    CHECK(s == "host.example.com");

    std::string inner = "a b";
    CHECK_FALSE(ti::TrimEdgeWhitespace(inner)); // inner spaces stay
    CHECK(inner == "a b");

    std::string blank = " \t ";
    CHECK(ti::TrimEdgeWhitespace(blank));
    CHECK(blank.empty());

    std::string empty;
    CHECK_FALSE(ti::TrimEdgeWhitespace(empty));
}

TEST_CASE("TrimEdgeWhitespace strips exactly what TrimCopyAsciiWhitespace strips") {
    // The Templates add handlers use TrimCopyAsciiWhitespace and the widget uses TrimEdgeWhitespace; a
    // value must come out the same either way, or a template added with Enter would differ from one
    // added with the button.
    const char* const samples[] = {"",     " ",       "a",    " a",       "a ",   "\t a b \r\n",
                                   "a\vb", "\v a \v", "\f x", "  \n\n  ", "a  b", "\xC2\xA0nbsp"};
    for (const char* sample : samples) {
        std::string viaWidget = sample;
        ti::TrimEdgeWhitespace(viaWidget);
        CHECK(viaWidget == TrimCopyAsciiWhitespace(sample));
    }
}

TEST_CASE("TrimEdgeWhitespace over a fixed buffer is bounded and always NUL-terminates") {
    char buf[16] = "  key  ";
    CHECK(ti::TrimEdgeWhitespace(buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "key") == 0);

    char lead[8] = " abc";
    CHECK(ti::TrimEdgeWhitespace(lead, sizeof(lead)));
    CHECK(std::strcmp(lead, "abc") == 0);

    char clean[8] = "abc";
    CHECK_FALSE(ti::TrimEdgeWhitespace(clean, sizeof(clean)));
    CHECK(std::strcmp(clean, "abc") == 0);

    char spaces[8] = "   ";
    CHECK(ti::TrimEdgeWhitespace(spaces, sizeof(spaces)));
    CHECK(spaces[0] == '\0');

    // No NUL inside the buffer: cut at the last byte rather than read past the end.
    char full[4] = {'a', 'b', 'c', 'd'};
    CHECK(ti::TrimEdgeWhitespace(full, sizeof(full)));
    CHECK(std::strcmp(full, "abc") == 0);

    CHECK_FALSE(ti::TrimEdgeWhitespace(static_cast<char*>(nullptr), 8u));
    char one[1] = {'x'};
    CHECK(ti::TrimEdgeWhitespace(one, 0u) == false); // zero size: untouched
    CHECK(one[0] == 'x');
}

TEST_CASE("FieldAppearedThisFrame is true only on the first frame of a run of frames") {
    CHECK(ti::FieldAppearedThisFrame(-1, 0));        // never drawn
    CHECK(ti::FieldAppearedThisFrame(-1, 500));      // never drawn, late in the session
    CHECK_FALSE(ti::FieldAppearedThisFrame(41, 42)); // drawn last frame: still showing
    CHECK(ti::FieldAppearedThisFrame(40, 42));       // skipped a frame: swapped back in
    CHECK(ti::FieldAppearedThisFrame(10, 300));      // popup reopened much later
    CHECK(ti::FieldAppearedThisFrame(42, 42));       // drawn twice in one frame is not "still showing"
}

TEST_CASE("ResolveSingleLineEnd: Esc reverts, anything else that ends the edit commits") {
    const ti::SingleLineEnd going = ti::ResolveSingleLineEnd(false, true, true);
    CHECK_FALSE(going.committed); // the edit has not ended: nothing to report, whatever the keys
    CHECK_FALSE(going.enterCommitted);
    CHECK_FALSE(going.reverted);

    const ti::SingleLineEnd enter = ti::ResolveSingleLineEnd(true, false, true);
    CHECK(enter.committed);
    CHECK(enter.enterCommitted);
    CHECK_FALSE(enter.reverted);

    // Tab, a click elsewhere, or Ctrl+Enter: committed, but not the field's Enter action.
    const ti::SingleLineEnd away = ti::ResolveSingleLineEnd(true, false, false);
    CHECK(away.committed);
    CHECK_FALSE(away.enterCommitted);
    CHECK_FALSE(away.reverted);

    const ti::SingleLineEnd esc = ti::ResolveSingleLineEnd(true, true, false);
    CHECK_FALSE(esc.committed); // a reverted value is never saved or trimmed
    CHECK_FALSE(esc.enterCommitted);
    CHECK(esc.reverted);
}

TEST_CASE("MultilineEscLeavesField only while the composer has the keyboard") {
    CHECK(ti::MultilineEscLeavesField(true, true));
    CHECK_FALSE(ti::MultilineEscLeavesField(false, true)); // the second Esc is the dialog's
    CHECK_FALSE(ti::MultilineEscLeavesField(true, false));
}

TEST_CASE("MultilineSubmitted: Ctrl+Enter alone submits; Enter and Shift+Enter type a newline") {
    CHECK(ti::MultilineSubmitted(true, true, true, false, false, false, true));
    CHECK_FALSE(ti::MultilineSubmitted(false, true, true, false, false, false, true)); // edit still going
    CHECK_FALSE(ti::MultilineSubmitted(true, true, false, false, false, false, true)); // bare Enter
    CHECK_FALSE(ti::MultilineSubmitted(true, true, true, true, false, false, true));   // Ctrl+Shift+Enter
    CHECK_FALSE(ti::MultilineSubmitted(true, true, true, false, true, false, true));   // Ctrl+Alt+Enter
    CHECK_FALSE(ti::MultilineSubmitted(true, true, true, false, false, true, true));   // Ctrl+Super+Enter
    CHECK_FALSE(ti::MultilineSubmitted(true, false, true, false, false, false, true)); // ended by a click
    CHECK_FALSE(ti::MultilineSubmitted(true, true, true, false, false, false, false)); // caller opted out
}

TEST_CASE("ShowCapacityCounter appears within a tenth of a fixed buffer's end") {
    // 128-byte buffer: 127 usable bytes; the count shows from 115 (127 - 12).
    CHECK_FALSE(ti::ShowCapacityCounter(0u, 128u));
    CHECK_FALSE(ti::ShowCapacityCounter(114u, 128u));
    CHECK(ti::ShowCapacityCounter(115u, 128u));
    CHECK(ti::ShowCapacityCounter(127u, 128u)); // full: the next keystroke is dropped
    // Tiny buffers: a 2-byte buffer is full at 1; a 1-byte buffer holds nothing and is always full.
    CHECK_FALSE(ti::ShowCapacityCounter(0u, 2u));
    CHECK(ti::ShowCapacityCounter(1u, 2u));
    CHECK(ti::ShowCapacityCounter(0u, 1u));
    CHECK_FALSE(ti::ShowCapacityCounter(0u, 0u));
    // No overflow on absurd sizes.
    const std::size_t huge = static_cast<std::size_t>(-1);
    CHECK_FALSE(ti::ShowCapacityCounter(0u, huge));
    CHECK(ti::ShowCapacityCounter(huge - 1u, huge));
}

// ---------------------------------------------------------------------------------------------------------
// Confirm dialogs and the global-shortcut mute (input-ux-w2-dialog-contract; audit #86, #88). These are the
// rules the shared footer in Ui/SmatchetDialogScope.h reads; kept beside the Wave 1 dialog-key cases above.
// ---------------------------------------------------------------------------------------------------------

using smatchet::dialogkeys::ConfirmDefault;
using smatchet::dialogkeys::ConfirmPick;
using smatchet::dialogkeys::GlobalShortcutAllowedUnderModal;
using smatchet::dialogkeys::ResolveConfirmDefault;
using smatchet::dialogkeys::ResolveConfirmEnter;

TEST_CASE("ResolveConfirmDefault: a destructive or consent prompt defaults to the safe button") {
    CHECK(ResolveConfirmDefault(true, false) == ConfirmDefault::Safe);
    CHECK(ResolveConfirmDefault(false, true) == ConfirmDefault::Safe);
    CHECK(ResolveConfirmDefault(true, true) == ConfirmDefault::Safe);
}

TEST_CASE("ResolveConfirmDefault: a non-destructive confirm defaults to its action") {
    CHECK(ResolveConfirmDefault(false, false) == ConfirmDefault::Primary);
}

TEST_CASE("ResolveConfirmEnter: with no keyboard cursor on an item, Enter runs the default button") {
    CHECK(ResolveConfirmEnter(true, true, true, false, ConfirmDefault::Safe) == ConfirmPick::Cancel);
    CHECK(ResolveConfirmEnter(true, true, true, false, ConfirmDefault::Primary) == ConfirmPick::Primary);
}

TEST_CASE("ResolveConfirmEnter: Enter on a destructive confirm never runs the action by default") {
    const ConfirmDefault def = ResolveConfirmDefault(true, false);
    CHECK(ResolveConfirmEnter(true, true, true, false, def) != ConfirmPick::Primary);
}

TEST_CASE("ResolveConfirmEnter: an item under the visible keyboard cursor takes Enter itself") {
    // ImGui activates that item and the footer reads the click, so the decision must not also pick.
    CHECK(ResolveConfirmEnter(true, true, true, true, ConfirmDefault::Primary) == ConfirmPick::None);
    CHECK(ResolveConfirmEnter(true, true, true, true, ConfirmDefault::Safe) == ConfirmPick::None);
}

TEST_CASE("ResolveConfirmEnter: nothing without Enter, or while a popup above the dialog has the keyboard") {
    CHECK(ResolveConfirmEnter(false, true, true, false, ConfirmDefault::Primary) == ConfirmPick::None);
    CHECK(ResolveConfirmEnter(true, false, true, false, ConfirmDefault::Primary) == ConfirmPick::None);
    CHECK(ResolveConfirmEnter(true, false, true, false, ConfirmDefault::Safe) == ConfirmPick::None);
}

TEST_CASE("ResolveConfirmEnter: the Enter that opened the dialog does not also answer it") {
    // The dialog opened this frame, so it did not hold the keyboard last frame.
    CHECK(ResolveConfirmEnter(true, true, false, false, ConfirmDefault::Primary) == ConfirmPick::None);
    CHECK(ResolveConfirmEnter(true, true, false, false, ConfirmDefault::Safe) == ConfirmPick::None);
}

TEST_CASE("GlobalShortcutAllowedUnderModal: zoom and fullscreen still run under a modal") {
    CHECK(GlobalShortcutAllowedUnderModal("ui.zoom.in"));
    CHECK(GlobalShortcutAllowedUnderModal("ui.zoom.out"));
    CHECK(GlobalShortcutAllowedUnderModal("ui.zoom.reset"));
    CHECK(GlobalShortcutAllowedUnderModal("app.fullscreen.toggle"));
}

TEST_CASE("GlobalShortcutAllowedUnderModal: the focus-scoped gesture commands still run under a modal") {
    CHECK(GlobalShortcutAllowedUnderModal("ui.list.move_up"));
    CHECK(GlobalShortcutAllowedUnderModal("ui.list.remove"));
    CHECK(GlobalShortcutAllowedUnderModal("ui.splitter.grow"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("view.panel.grow")); // names a panel: not what a dialog serves
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.listing.other"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal(nullptr));
}

TEST_CASE("GlobalShortcutAllowedUnderModal: everything that changes state is muted, the palette too") {
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.command_palette"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.preferences.toggle"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("view.delete"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("grid.search.focus"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal(""));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal(nullptr));
}

TEST_CASE("GlobalShortcutAllowedUnderModal: the zoom match needs the whole 'ui.zoom.' segment") {
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.zoom"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("ui.zoomer.in"));
    CHECK_FALSE(GlobalShortcutAllowedUnderModal("app.fullscreen.toggle.extra"));
}
