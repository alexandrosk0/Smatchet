#include <doctest/doctest.h>

#include "TicketFieldEditorCommitPolicyPure.h"

using TicketFieldEditorCommitPolicyPure::ShouldCloseTouchPopupEdit;
using TicketFieldEditorCommitPolicyPure::ShouldCommitInlineFieldEdit;
using TicketFieldEditorCommitPolicyPure::ShouldCommitTouchPopupEdit;
using TicketFieldEditorCommitPolicyPure::ShouldEndInlineEdit;
using TicketFieldEditorCommitPolicyPure::ShouldOpenCellEditorByLongPress;

// WS4 item 16 (stray-PUT-on-Escape): on the mobile/touch build an inline field edit must commit
// ONLY on an explicit submit; any focus-loss deactivation cancels with no PUT to the real tracker.
// Desktop keeps committing on click-away (deactivate). Both branches exercised here on the desktop
// test build by passing isMobile explicitly.
//
// estimate-edit-ux follow-up adds the dirty-check (valueChanged gates ALL commits) and the
// ShouldEndInlineEdit session-end decision. The four ShouldCommit args are
// (explicitSubmit, deactivated, isMobile, valueChanged).

TEST_CASE("ShouldCommitInlineFieldEdit: explicit submit commits a real change on any platform") {
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivated=*/false, /*isMobile=*/false,
                                      /*valueChanged=*/true));
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivated=*/false, /*isMobile=*/true,
                                      /*valueChanged=*/true));
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivated=*/true, /*isMobile=*/false,
                                      /*valueChanged=*/true));
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivated=*/true, /*isMobile=*/true,
                                      /*valueChanged=*/true));
}

TEST_CASE("ShouldCommitInlineFieldEdit: desktop commits a real change on focus-loss (click-away)") {
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivated=*/true, /*isMobile=*/false,
                                      /*valueChanged=*/true));
}

TEST_CASE("ShouldCommitInlineFieldEdit: mobile does NOT commit on focus-loss (the fix)") {
    // Back / tap-away / IME dismiss deactivates the editor on-device — must cancel, never PUT.
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivated=*/true, /*isMobile=*/true,
                                            /*valueChanged=*/true));
}

TEST_CASE("ShouldCommitInlineFieldEdit: no submit and no deactivation keeps editing") {
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivated=*/false, /*isMobile=*/false,
                                            /*valueChanged=*/true));
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivated=*/false, /*isMobile=*/true,
                                            /*valueChanged=*/true));
}

// --- dirty check: a no-op edit must NEVER PUT (estimate-edit-ux) -----------------------------------

TEST_CASE("ShouldCommitInlineFieldEdit: unchanged value never commits, whatever the trigger") {
    // The empty-cell-exit and click-existing-value-then-release-unchanged spurious-PUT bugs.
    // valueChanged=false overrides explicit submit, focus-loss, and platform.
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivated=*/false, /*isMobile=*/false,
                                            /*valueChanged=*/false));
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivated=*/true, /*isMobile=*/false,
                                            /*valueChanged=*/false));
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivated=*/true, /*isMobile=*/true,
                                            /*valueChanged=*/false));
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivated=*/true, /*isMobile=*/true,
                                            /*valueChanged=*/false));
}

// --- ShouldEndInlineEdit: the session ends even when no PUT fires (fixes the stuck editor) ---------

TEST_CASE("ShouldEndInlineEdit: Escape ends the edit (cancel)") {
    CHECK(ShouldEndInlineEdit(/*escapePressed=*/true, /*explicitSubmit=*/false, /*deactivated=*/false));
}

TEST_CASE("ShouldEndInlineEdit: explicit submit ends the edit") {
    CHECK(ShouldEndInlineEdit(/*escapePressed=*/false, /*explicitSubmit=*/true, /*deactivated=*/false));
}

TEST_CASE("ShouldEndInlineEdit: focus-loss ends the edit even when unchanged (stuck-editor fix)") {
    // Refocus an existing value (no change) then click a DIFFERENT cell: deactivated=true with no
    // submit / escape must still close the editor — previously nothing ended it and it stuck open.
    CHECK(ShouldEndInlineEdit(/*escapePressed=*/false, /*explicitSubmit=*/false, /*deactivated=*/true));
}

TEST_CASE("ShouldEndInlineEdit: still editing keeps the session open") {
    CHECK_FALSE(ShouldEndInlineEdit(/*escapePressed=*/false, /*explicitSubmit=*/false, /*deactivated=*/false));
}

// --- the combined contract that encodes the three reported bugs ------------------------------------

TEST_CASE("inline-edit policy: exit an unchanged EMPTY cell -> ends, no PUT (bug 2)") {
    const bool deactivated = true, explicitSubmit = false, escape = false, isMobile = false;
    const bool valueChanged = false; // empty buffer == empty original
    CHECK_FALSE(ShouldCommitInlineFieldEdit(explicitSubmit, deactivated, isMobile, valueChanged));
    CHECK(ShouldEndInlineEdit(escape, explicitSubmit, deactivated));
}

TEST_CASE("inline-edit policy: Escape on an existing value -> cancels, no PUT (bug 3 / escape)") {
    const bool deactivated = false, explicitSubmit = false, escape = true, isMobile = false;
    const bool valueChanged = false; // ImGui reverts the buffer on Escape
    CHECK_FALSE(ShouldCommitInlineFieldEdit(explicitSubmit, deactivated, isMobile, valueChanged));
    CHECK(ShouldEndInlineEdit(escape, explicitSubmit, deactivated));
}

TEST_CASE("inline-edit policy: click away after changing the value -> commits AND ends (desktop)") {
    const bool deactivated = true, explicitSubmit = false, escape = false, isMobile = false;
    const bool valueChanged = true;
    CHECK(ShouldCommitInlineFieldEdit(explicitSubmit, deactivated, isMobile, valueChanged));
    CHECK(ShouldEndInlineEdit(escape, explicitSubmit, deactivated));
}

// --- P1.3 touch open-gate: long-press opens, quick tap / drag do not (#1018 item 23) --------------
// Args are (cellHovered, primaryDown, heldSeconds, dragging, longPressThresholdSeconds). The mobile
// build defaults the threshold to 0.5 s (Android ViewConfiguration long-press timeout).

TEST_CASE("ShouldOpenCellEditorByLongPress: a stationary hold past the threshold opens the editor") {
    CHECK(ShouldOpenCellEditorByLongPress(/*cellHovered=*/true, /*primaryDown=*/true, /*heldSeconds=*/0.6f,
                                          /*dragging=*/false, /*longPressThresholdSeconds=*/0.5f));
}

TEST_CASE("ShouldOpenCellEditorByLongPress: a quick tap (held < threshold) selects, never opens") {
    // Tap-to-select / scroll-fling must not open the editor — the core touch disambiguation.
    CHECK_FALSE(ShouldOpenCellEditorByLongPress(/*cellHovered=*/true, /*primaryDown=*/true, /*heldSeconds=*/0.1f,
                                                /*dragging=*/false, /*longPressThresholdSeconds=*/0.5f));
}

TEST_CASE("ShouldOpenCellEditorByLongPress: a hold that drifts into a drag (scroll) never opens") {
    // Held past the threshold but moved past the drag slop = a scroll gesture, not an edit-open.
    CHECK_FALSE(ShouldOpenCellEditorByLongPress(/*cellHovered=*/true, /*primaryDown=*/true, /*heldSeconds=*/0.9f,
                                                /*dragging=*/true, /*longPressThresholdSeconds=*/0.5f));
}

TEST_CASE("ShouldOpenCellEditorByLongPress: a hold that left the cell (not hovered) never opens") {
    CHECK_FALSE(ShouldOpenCellEditorByLongPress(/*cellHovered=*/false, /*primaryDown=*/true, /*heldSeconds=*/0.9f,
                                                /*dragging=*/false, /*longPressThresholdSeconds=*/0.5f));
}

TEST_CASE("ShouldOpenCellEditorByLongPress: no contact (released / hovering only) never opens") {
    CHECK_FALSE(ShouldOpenCellEditorByLongPress(/*cellHovered=*/true, /*primaryDown=*/false, /*heldSeconds=*/0.9f,
                                                /*dragging=*/false, /*longPressThresholdSeconds=*/0.5f));
}

TEST_CASE("ShouldOpenCellEditorByLongPress: the threshold boundary is inclusive (>=)") {
    CHECK(ShouldOpenCellEditorByLongPress(/*cellHovered=*/true, /*primaryDown=*/true, /*heldSeconds=*/0.5f,
                                          /*dragging=*/false, /*longPressThresholdSeconds=*/0.5f));
}

// --- P1.3 touch-popup commit policy: Save=PUT (real change only), Cancel/Back/tap-away=no PUT -----
// build-out (b) (#1018 item 23). Args: ShouldCommit is (savePressed, valueChanged); ShouldClose is
// (savePressed, cancelPressed, dismissedByTapAway, backPressed).

TEST_CASE("ShouldCommitTouchPopupEdit: explicit Save of a real change PUTs") {
    CHECK(ShouldCommitTouchPopupEdit(/*savePressed=*/true, /*valueChanged=*/true));
}

TEST_CASE("ShouldCommitTouchPopupEdit: Save of an unchanged value never PUTs (no-op rule)") {
    // Mirrors the inline no-op-edit guard: opening a combo and Saving without changing the
    // selection must not fire a spurious PUT the tracker would reject.
    CHECK_FALSE(ShouldCommitTouchPopupEdit(/*savePressed=*/true, /*valueChanged=*/false));
}

TEST_CASE("ShouldCommitTouchPopupEdit: no Save never PUTs, even with a real change") {
    // Cancel / Back / tap-away leave savePressed=false — the change is discarded, never committed.
    CHECK_FALSE(ShouldCommitTouchPopupEdit(/*savePressed=*/false, /*valueChanged=*/true));
    CHECK_FALSE(ShouldCommitTouchPopupEdit(/*savePressed=*/false, /*valueChanged=*/false));
}

TEST_CASE("ShouldCloseTouchPopupEdit: Save / Cancel / tap-away / Back each close the popup") {
    CHECK(ShouldCloseTouchPopupEdit(/*save=*/true, /*cancel=*/false, /*tapAway=*/false, /*back=*/false));
    CHECK(ShouldCloseTouchPopupEdit(/*save=*/false, /*cancel=*/true, /*tapAway=*/false, /*back=*/false));
    CHECK(ShouldCloseTouchPopupEdit(/*save=*/false, /*cancel=*/false, /*tapAway=*/true, /*back=*/false));
    CHECK(ShouldCloseTouchPopupEdit(/*save=*/false, /*cancel=*/false, /*tapAway=*/false, /*back=*/true));
}

TEST_CASE("ShouldCloseTouchPopupEdit: an untouched popup stays open") {
    CHECK_FALSE(ShouldCloseTouchPopupEdit(/*save=*/false, /*cancel=*/false, /*tapAway=*/false, /*back=*/false));
}

// --- combined touch-popup contract: the three discard paths never PUT, Save does ------------------

TEST_CASE("touch-popup policy: Save a real change -> PUTs AND closes") {
    const bool save = true, cancel = false, tapAway = false, back = false, valueChanged = true;
    CHECK(ShouldCommitTouchPopupEdit(save, valueChanged));
    CHECK(ShouldCloseTouchPopupEdit(save, cancel, tapAway, back));
}

TEST_CASE("touch-popup policy: Cancel after changing -> no PUT, closes (discard guarantee)") {
    const bool save = false, cancel = true, tapAway = false, back = false, valueChanged = true;
    CHECK_FALSE(ShouldCommitTouchPopupEdit(save, valueChanged));
    CHECK(ShouldCloseTouchPopupEdit(save, cancel, tapAway, back));
}

TEST_CASE("touch-popup policy: Back after changing -> no PUT, closes (the on-device stray-PUT class)") {
    // Android Back is the focus-loss that used to PUT a stray edit on the inline path; on the popup
    // path it must discard too — savePressed stays false so nothing commits.
    const bool save = false, cancel = false, tapAway = false, back = true, valueChanged = true;
    CHECK_FALSE(ShouldCommitTouchPopupEdit(save, valueChanged));
    CHECK(ShouldCloseTouchPopupEdit(save, cancel, tapAway, back));
}

TEST_CASE("touch-popup policy: tap-away after changing -> no PUT, closes") {
    const bool save = false, cancel = false, tapAway = true, back = false, valueChanged = true;
    CHECK_FALSE(ShouldCommitTouchPopupEdit(save, valueChanged));
    CHECK(ShouldCloseTouchPopupEdit(save, cancel, tapAway, back));
}
