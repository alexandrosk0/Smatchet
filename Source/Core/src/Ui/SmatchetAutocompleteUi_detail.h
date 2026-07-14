#ifndef SMATCHET_AUTOCOMPLETE_UI_DETAIL_H
#define SMATCHET_AUTOCOMPLETE_UI_DETAIL_H

// SmatchetAutocompleteUi pure list-navigation / completion-resolution helpers —
// the JQL/query-autocomplete InputText callback's Up/Down arrow index walk and
// Enter/Tab commit decision, as plain integer math needing no ImGui context, so
// the callback stays under the branch cap and the math is bucket-A testable.
// The caret-sentinel string helper below shares the same contract: a plain
// std::string transform, no ImGui. (Value-quoting lives in the shared
// tracker_query_suggest helpers; this TU calls those directly.)

#include <string>

namespace SmatchetAutocompleteDetail {

// Result of an Up/Down arrow press over the suggestion list (CallbackHistory).
struct AcpHistoryNav {
    int selected = -1;             // new selected index
    bool scrollToSelected = false; // request SetScrollHereY next frame
};

// Compute the new selected index after an arrow press.
//   n        — number of suggestions (caller guarantees n > 0 for arrow walk)
//   selected — current selected index (-1 = none)
//   isDown   — DownArrow pressed
//   isUp     — UpArrow pressed
// Mirrors the CallbackHistory arrow logic byte-for-byte: Down from -1 selects 0
// and always scrolls; Down otherwise advances (clamped to n-1) and scrolls; Up
// retreats (clamped to 0) and scrolls only when something was selected; neither
// key clamps the existing selection into range without scrolling.
AcpHistoryNav ResolveAcpHistoryNav(int n, int selected, bool isDown, bool isUp);

// Result of an Enter / Tab evaluation against the live suggestion list
// (CallbackAlways).
struct AcpCommitDecision {
    int selected = -1;                // selected index after range-clamp
    bool queueApply = false;          // queue the highlighted suggestion as a replacement
    bool wantsApplyFromEnter = false; // raw-Enter apply (no highlighted suggestion)
};

// Decide what an Enter/Tab keypress should do given the current list.
//   n         — number of suggestions
//   selected  — current selected index (-1 = none)
//   enterDown — Enter or KeypadEnter pressed this frame
//   tabDown   — Tab pressed this frame
// Mirrors the CallbackAlways commit logic: with items, a clamped highlighted
// selection + (Enter|Tab) queues an apply; a bare Enter with no highlight sets
// wantsApplyFromEnter. With no items, selected resets to -1 and Enter sets
// wantsApplyFromEnter.
AcpCommitDecision ResolveAcpCommit(int n, int selected, bool enterDown, bool tabDown);

// Strip the \x7F caret-anchor sentinel from @p text in place (JQL function
// suggestions like `membersOf("\x7F")` mark where the caret should land on
// insert). Returns the byte offset the sentinel occupied, or -1 if absent.
int StripCaretAnchorSentinel(std::string& text);

} // namespace SmatchetAutocompleteDetail

#endif // SMATCHET_AUTOCOMPLETE_UI_DETAIL_H
