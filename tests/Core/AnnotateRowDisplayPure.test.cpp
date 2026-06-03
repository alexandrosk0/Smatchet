// Bucket-A coverage for the pure display-classification helpers extracted from
// AnnotateAnalysisUi::DrawContent during its function-size decomposition
// (feat/funcsize-annotate-analysis). These helpers decide per-row cell state
// (disabled styling, actionability, display text) and are ImGui-free, so the test
// rig links just the AnnotateAnalysisUi_Window_detail TU.
//
// Goldens here are the literal contract of the original inline expressions in the
// monolith (e.g. `pending ? "..." : (user.empty() ? "-" : user)`); they encode the
// behaviour the refactor must preserve, not a re-derivation of new logic.

#include <doctest/doctest.h>

#include "Ui/AnnotateAnalysisUi_Window_detail.h"

using AnnotateUiPure::LineCanAssign;
using AnnotateUiPure::PendingOrDash;
using AnnotateUiPure::RowClCellDisabled;
using AnnotateUiPure::RowUserCellDisabled;
using AnnotateUiPure::RowUserIsActionable;

TEST_CASE("RowUserIsActionable: only a real, loaded user is actionable") {
    CHECK(RowUserIsActionable(false, "alice"));
    CHECK_FALSE(RowUserIsActionable(true, "alice")); // still loading
    CHECK_FALSE(RowUserIsActionable(false, ""));     // no user
    CHECK_FALSE(RowUserIsActionable(false, "-"));    // placeholder
    CHECK_FALSE(RowUserIsActionable(false, "..."));  // loading sentinel
}

TEST_CASE("RowUserCellDisabled: dim when loading or no real user") {
    CHECK(RowUserCellDisabled(true, "alice")); // loading
    CHECK(RowUserCellDisabled(false, ""));     // empty
    CHECK(RowUserCellDisabled(false, "-"));    // placeholder
    CHECK_FALSE(RowUserCellDisabled(false, "alice"));
    // "..." is NOT in the disabled predicate (mirrors the monolith): it stays a link
    // style even though it reads as a sentinel.
    CHECK_FALSE(RowUserCellDisabled(false, "..."));
}

TEST_CASE("RowClCellDisabled: dim when loading or empty CL") {
    CHECK(RowClCellDisabled(true, "12345"));
    CHECK(RowClCellDisabled(false, ""));
    CHECK_FALSE(RowClCellDisabled(false, "12345"));
}

TEST_CASE("LineCanAssign: a real Perforce user enables the Assign menu item") {
    CHECK(LineCanAssign("bob"));
    CHECK_FALSE(LineCanAssign(""));
    CHECK_FALSE(LineCanAssign("-"));
    CHECK_FALSE(LineCanAssign("..."));
}

TEST_CASE("PendingOrDash: '...' while loading, '-' when empty, else verbatim") {
    CHECK(PendingOrDash(true, "anything") == "...");
    CHECK(PendingOrDash(true, "") == "...");
    CHECK(PendingOrDash(false, "") == "-");
    CHECK(PendingOrDash(false, "12345") == "12345");
    CHECK(PendingOrDash(false, "alice") == "alice");
}
