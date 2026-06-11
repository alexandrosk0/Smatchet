#include <doctest/doctest.h>

#include "TicketFieldEditorCommitPolicyPure.h"

using TicketFieldEditorCommitPolicyPure::ShouldCommitInlineFieldEdit;
using TicketFieldEditorCommitPolicyPure::ShouldEndInlineEdit;

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
