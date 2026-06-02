#ifndef SMATCHET_AUTOCOMPLETE_UI_DETAIL_H
#define SMATCHET_AUTOCOMPLETE_UI_DETAIL_H

// SmatchetAutocompleteUi pure list-navigation / completion-resolution helpers.
//
// The JQL/query-autocomplete InputText callback drives its keyboard list
// navigation (Up/Down arrow index walk) and its commit decision (Enter / Tab /
// apply-from-enter) through plain integer math that needs no ImGui context.
// These helpers carry exactly that logic so the callback's ImGui-touching half
// stays thin and under the function-size branch cap, and so the math is
// bucket-A testable without an ImGui frame.

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

} // namespace SmatchetAutocompleteDetail

#endif // SMATCHET_AUTOCOMPLETE_UI_DETAIL_H
