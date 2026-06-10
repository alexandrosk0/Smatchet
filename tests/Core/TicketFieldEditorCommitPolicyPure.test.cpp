#include <doctest/doctest.h>

#include "TicketFieldEditorCommitPolicyPure.h"

using TicketFieldEditorCommitPolicyPure::ShouldCommitInlineFieldEdit;

// WS4 item 16 (stray-PUT-on-Escape): on the mobile/touch build an inline field edit must commit
// ONLY on an explicit submit; any focus-loss deactivation cancels with no PUT to the real tracker.
// Desktop keeps committing on click-away (deactivate). Both branches exercised here on the desktop
// test build by passing isMobile explicitly.

TEST_CASE("ShouldCommitInlineFieldEdit: explicit submit always commits") {
    // Enter / IME "Done" commits regardless of platform or deactivation state.
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivatedAfterEdit=*/false, /*isMobile=*/false));
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivatedAfterEdit=*/false, /*isMobile=*/true));
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivatedAfterEdit=*/true, /*isMobile=*/false));
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/true, /*deactivatedAfterEdit=*/true, /*isMobile=*/true));
}

TEST_CASE("ShouldCommitInlineFieldEdit: desktop commits on focus-loss (click-away)") {
    // Byte-identical to the pre-fix `submitted || deactivatedAfterEdit` on desktop.
    CHECK(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivatedAfterEdit=*/true, /*isMobile=*/false));
}

TEST_CASE("ShouldCommitInlineFieldEdit: mobile does NOT commit on focus-loss (the fix)") {
    // Back / tap-away / IME dismiss deactivates the editor on-device — must cancel, never PUT.
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivatedAfterEdit=*/true, /*isMobile=*/true));
}

TEST_CASE("ShouldCommitInlineFieldEdit: no submit and no deactivation keeps editing") {
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivatedAfterEdit=*/false, /*isMobile=*/false));
    CHECK_FALSE(ShouldCommitInlineFieldEdit(/*explicitSubmit=*/false, /*deactivatedAfterEdit=*/false, /*isMobile=*/true));
}
