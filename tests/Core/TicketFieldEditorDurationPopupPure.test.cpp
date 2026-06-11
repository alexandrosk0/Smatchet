#include <doctest/doctest.h>

#include "TicketFieldEditorDurationPopupPure.h"

#include <cstring>

using TicketFieldEditorDurationPopupPure::IsPrintableTypedChar;
using TicketFieldEditorDurationPopupPure::ShouldFinalizeOnPopupClose;
using TicketFieldEditorDurationPopupPure::ShouldPullFocusForTypedChar;
using TicketFieldEditorDurationPopupPure::ShouldRearmFocusOnPopupClose;
using TicketFieldEditorDurationPopupPure::SpliceTypedCharsIntoBuf;

// estimate-edit-ux: the duration-suggestions popup steals keyboard focus from its InputText
// (popup focus clears the parent ActiveId). These policies decide (a) when a printable keystroke
// pulls focus back into the input (type-to-edit) and splices the typed chars directly into the
// buffer (the focus pull alone cannot save them: SetKeyboardFocusHere is a next-frame nav-move,
// the char queue dies at EndFrame, and nav-activation makes InputText skip insertion) and
// (b) when a popup-close is a genuine commit-deactivation vs a click landing back on the input
// (refocus-for-editing, never a commit — the old unconditional popup-close commit queued a
// spurious empty-value PUT on an empty buffer).

TEST_CASE("IsPrintableTypedChar: printable characters land in the input") {
    CHECK(IsPrintableTypedChar(' '));
    CHECK(IsPrintableTypedChar('2'));
    CHECK(IsPrintableTypedChar('h'));
    CHECK(IsPrintableTypedChar('~'));
    CHECK(IsPrintableTypedChar(0x00E9u)); // é — non-ASCII text is typed text too
}

TEST_CASE("IsPrintableTypedChar: all WM_CHAR control codes below 0x20 plus DEL are filtered") {
    CHECK_FALSE(IsPrintableTypedChar('\r')); // Enter
    CHECK_FALSE(IsPrintableTypedChar('\n'));
    CHECK_FALSE(IsPrintableTypedChar(0x1Bu)); // Escape
    CHECK_FALSE(IsPrintableTypedChar('\b'));  // Backspace
    CHECK_FALSE(IsPrintableTypedChar('\t'));  // Tab
    CHECK_FALSE(IsPrintableTypedChar(0x01u)); // Ctrl+A queued as WM_CHAR control code
    CHECK_FALSE(IsPrintableTypedChar(0x1Fu)); // top of the C0 range
    CHECK_FALSE(IsPrintableTypedChar(0x7Fu)); // DEL
}

// The typeToEditFocus opt-in gate lives at the call site (only the grid inline-edit path opts in;
// the worklog dialog keeps stock focus behaviour) — this helper assumes the gate already passed.
TEST_CASE("ShouldPullFocusForTypedChar: pulls focus when the keystroke would otherwise drop") {
    // Popup holds focus (no active item), printable char queued -> pull.
    CHECK(ShouldPullFocusForTypedChar(/*anyItemActive=*/false, /*focusAlreadyPending=*/false,
                                      /*hasPrintableQueuedChar=*/true));
}

TEST_CASE("ShouldPullFocusForTypedChar: never steals from an active item") {
    // Another InputText (e.g. a sibling field) is consuming the queue — do not steal focus.
    CHECK_FALSE(ShouldPullFocusForTypedChar(/*anyItemActive=*/true, false, true));
}

TEST_CASE("ShouldPullFocusForTypedChar: no-ops without a printable char or with focus pending") {
    CHECK_FALSE(ShouldPullFocusForTypedChar(false, false, /*hasPrintableQueuedChar=*/false));
    CHECK_FALSE(ShouldPullFocusForTypedChar(false, /*focusAlreadyPending=*/true, true));
}

TEST_CASE("SpliceTypedCharsIntoBuf: ASCII chars append at the end, NUL-terminated") {
    char buf[16] = "1";
    const unsigned int queue[] = {'2', 'h'};
    CHECK(SpliceTypedCharsIntoBuf(queue, 2, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "12h") == 0);
}

TEST_CASE("SpliceTypedCharsIntoBuf: control chars in the queue are skipped, printables land") {
    char buf[16] = "";
    const unsigned int queue[] = {0x1Bu, '2', '\r', 'h'};
    CHECK(SpliceTypedCharsIntoBuf(queue, 4, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "2h") == 0);
}

TEST_CASE("SpliceTypedCharsIntoBuf: non-ASCII codepoint is UTF-8 encoded (2-byte)") {
    char buf[16] = "";
    const unsigned int queue[] = {0x00E9u}; // é -> 0xC3 0xA9
    CHECK(SpliceTypedCharsIntoBuf(queue, 1, buf, sizeof(buf)));
    CHECK(static_cast<unsigned char>(buf[0]) == 0xC3u);
    CHECK(static_cast<unsigned char>(buf[1]) == 0xA9u);
    CHECK(buf[2] == '\0');
}

TEST_CASE("SpliceTypedCharsIntoBuf: stops at bufSize, keeps what landed, stays terminated") {
    char buf[4] = "1";
    const unsigned int queue[] = {'2', 'h', 'm'}; // '2' and 'h' fit (3 chars + NUL), 'm' must not
    CHECK(SpliceTypedCharsIntoBuf(queue, 3, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "12h") == 0);
}

TEST_CASE("SpliceTypedCharsIntoBuf: multibyte char that would straddle bufSize is dropped whole") {
    char buf[4] = "12";
    const unsigned int queue[] = {0x00E9u}; // needs 2 bytes + NUL, only 1 byte free
    CHECK_FALSE(SpliceTypedCharsIntoBuf(queue, 1, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "12") == 0);
}

TEST_CASE("SpliceTypedCharsIntoBuf: nothing printable or degenerate args -> false, buf untouched") {
    char buf[8] = "1h";
    const unsigned int queue[] = {0x1Bu, '\r'};
    CHECK_FALSE(SpliceTypedCharsIntoBuf(queue, 2, buf, sizeof(buf)));
    CHECK(std::strcmp(buf, "1h") == 0);
    CHECK_FALSE(SpliceTypedCharsIntoBuf(queue, 0, buf, sizeof(buf)));
    CHECK_FALSE(SpliceTypedCharsIntoBuf(nullptr, 2, buf, sizeof(buf)));
    CHECK_FALSE(SpliceTypedCharsIntoBuf(queue, 2, nullptr, 8));
    CHECK_FALSE(SpliceTypedCharsIntoBuf(queue, 2, buf, 0));
}

TEST_CASE("ShouldFinalizeOnPopupClose: genuine click-away close commits (pre-fix behaviour kept)") {
    CHECK(ShouldFinalizeOnPopupClose(/*popupJustClosed=*/true, /*focusReturnPending=*/false,
                                     /*inputActive=*/false, /*inputHovered=*/false));
}

TEST_CASE("ShouldFinalizeOnPopupClose: click landing back on the input is a refocus, not a commit") {
    // Click re-activated the input naturally (caret placed by the click).
    CHECK_FALSE(ShouldFinalizeOnPopupClose(true, false, /*inputActive=*/true, /*inputHovered=*/true));
    // Popup-close ate the click: input only hovered — still a refocus (caller re-arms focus).
    CHECK_FALSE(ShouldFinalizeOnPopupClose(true, false, /*inputActive=*/false, /*inputHovered=*/true));
}

TEST_CASE("ShouldFinalizeOnPopupClose: suggestion pick (focus-return pending) never finalizes here") {
    CHECK_FALSE(ShouldFinalizeOnPopupClose(true, /*focusReturnPending=*/true, false, false));
}

TEST_CASE("ShouldFinalizeOnPopupClose: nothing closed, nothing finalized") {
    CHECK_FALSE(ShouldFinalizeOnPopupClose(/*popupJustClosed=*/false, false, false, false));
    CHECK_FALSE(ShouldFinalizeOnPopupClose(false, false, true, true));
}

TEST_CASE("ShouldRearmFocusOnPopupClose: close ate the input click (hovered, not active) -> re-arm") {
    CHECK(ShouldRearmFocusOnPopupClose(/*popupJustClosed=*/true, /*focusReturnPending=*/false,
                                       /*inputHoveredNow=*/true, /*inputActiveNow=*/false));
}

TEST_CASE("ShouldRearmFocusOnPopupClose: not the negation of ShouldFinalizeOnPopupClose") {
    // Genuine click-away (not hovered): finalize says commit, re-arm must stay false too.
    CHECK(ShouldFinalizeOnPopupClose(true, false, false, false));
    CHECK_FALSE(ShouldRearmFocusOnPopupClose(true, false, /*inputHoveredNow=*/false, false));
    // Click re-activated the input naturally: NEITHER fires — no commit, no re-arm needed.
    CHECK_FALSE(ShouldFinalizeOnPopupClose(true, false, /*inputActive=*/true, /*inputHovered=*/true));
    CHECK_FALSE(ShouldRearmFocusOnPopupClose(true, false, /*inputHoveredNow=*/true, /*inputActiveNow=*/true));
}

TEST_CASE("ShouldRearmFocusOnPopupClose: pending focus-return or no close -> no re-arm") {
    CHECK_FALSE(ShouldRearmFocusOnPopupClose(true, /*focusReturnPending=*/true, true, false));
    CHECK_FALSE(ShouldRearmFocusOnPopupClose(/*popupJustClosed=*/false, false, true, false));
}
