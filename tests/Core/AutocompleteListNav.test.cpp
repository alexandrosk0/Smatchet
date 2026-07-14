// AutocompleteListNav bucket-A doctest — pure list-navigation +
// completion-resolution helpers extracted from
// TrackerQueryAcp_InputTextCallback (JQL/query autocomplete). No ImGui context
// required; these are plain integer-decision functions.

#include "SmatchetAutocompleteUi_detail.h"

#include <doctest/doctest.h>

#include <string>

namespace {

using SmatchetAutocompleteDetail::AcpCommitDecision;
using SmatchetAutocompleteDetail::AcpHistoryNav;
using SmatchetAutocompleteDetail::ResolveAcpCommit;
using SmatchetAutocompleteDetail::ResolveAcpHistoryNav;
using SmatchetAutocompleteDetail::StripCaretAnchorSentinel;

} // namespace

TEST_CASE("ResolveAcpHistoryNav DownArrow from none selects 0 and scrolls") {
    const AcpHistoryNav nav = ResolveAcpHistoryNav(5, -1, /*isDown=*/true, /*isUp=*/false);
    CHECK(nav.selected == 0);
    CHECK(nav.scrollToSelected);
}

TEST_CASE("ResolveAcpHistoryNav DownArrow advances and clamps to n-1") {
    const AcpHistoryNav mid = ResolveAcpHistoryNav(5, 2, true, false);
    CHECK(mid.selected == 3);
    CHECK(mid.scrollToSelected);
    // At the bottom already — stays clamped at n-1.
    const AcpHistoryNav bot = ResolveAcpHistoryNav(5, 4, true, false);
    CHECK(bot.selected == 4);
    CHECK(bot.scrollToSelected);
}

TEST_CASE("ResolveAcpHistoryNav UpArrow retreats, clamps to 0, scrolls only when selected") {
    const AcpHistoryNav up = ResolveAcpHistoryNav(5, 3, false, true);
    CHECK(up.selected == 2);
    CHECK(up.scrollToSelected);
    // UpArrow with nothing selected — no change, no scroll.
    const AcpHistoryNav none = ResolveAcpHistoryNav(5, -1, false, true);
    CHECK(none.selected == -1);
    CHECK_FALSE(none.scrollToSelected);
    // UpArrow at top — clamps to 0, still scrolls.
    const AcpHistoryNav top = ResolveAcpHistoryNav(5, 0, false, true);
    CHECK(top.selected == 0);
    CHECK(top.scrollToSelected);
}

TEST_CASE("ResolveAcpHistoryNav non-arrow clamps stale selection without scroll") {
    // selected past the (shrunk) list length, neither arrow pressed.
    const AcpHistoryNav nav = ResolveAcpHistoryNav(3, 9, false, false);
    CHECK(nav.selected == 2); // clamped to n-1
    CHECK_FALSE(nav.scrollToSelected);
    // selected already valid, neither arrow — unchanged, no scroll.
    const AcpHistoryNav keep = ResolveAcpHistoryNav(3, 1, false, false);
    CHECK(keep.selected == 1);
    CHECK_FALSE(keep.scrollToSelected);
}

TEST_CASE("ResolveAcpHistoryNav empty list resets selection to -1") {
    const AcpHistoryNav nav = ResolveAcpHistoryNav(0, 2, true, false);
    CHECK(nav.selected == -1);
    CHECK_FALSE(nav.scrollToSelected);
}

TEST_CASE("ResolveAcpCommit highlighted Enter queues apply") {
    const AcpCommitDecision d = ResolveAcpCommit(5, 2, /*enter=*/true, /*tab=*/false);
    CHECK(d.selected == 2);
    CHECK(d.queueApply);
    CHECK_FALSE(d.wantsApplyFromEnter);
}

TEST_CASE("ResolveAcpCommit highlighted Tab queues apply") {
    const AcpCommitDecision d = ResolveAcpCommit(5, 0, false, /*tab=*/true);
    CHECK(d.queueApply);
    CHECK_FALSE(d.wantsApplyFromEnter);
}

TEST_CASE("ResolveAcpCommit clamps stale selection before applying") {
    const AcpCommitDecision d = ResolveAcpCommit(3, 9, true, false);
    CHECK(d.selected == 2); // clamped to n-1
    CHECK(d.queueApply);
}

TEST_CASE("ResolveAcpCommit Enter with no highlight wants apply-from-enter") {
    const AcpCommitDecision d = ResolveAcpCommit(5, -1, /*enter=*/true, false);
    CHECK(d.selected == -1);
    CHECK_FALSE(d.queueApply);
    CHECK(d.wantsApplyFromEnter);
}

TEST_CASE("ResolveAcpCommit Tab with no highlight does nothing") {
    const AcpCommitDecision d = ResolveAcpCommit(5, -1, false, /*tab=*/true);
    CHECK_FALSE(d.queueApply);
    CHECK_FALSE(d.wantsApplyFromEnter);
}

TEST_CASE("ResolveAcpCommit empty list resets selection and Enter wants apply") {
    const AcpCommitDecision d = ResolveAcpCommit(0, 3, /*enter=*/true, false);
    CHECK(d.selected == -1);
    CHECK_FALSE(d.queueApply);
    CHECK(d.wantsApplyFromEnter);
}

TEST_CASE("ResolveAcpCommit empty list no Enter is inert") {
    const AcpCommitDecision d = ResolveAcpCommit(0, 3, false, /*tab=*/true);
    CHECK(d.selected == -1);
    CHECK_FALSE(d.queueApply);
    CHECK_FALSE(d.wantsApplyFromEnter);
}

TEST_CASE("StripCaretAnchorSentinel — removes the \\x7F marker and returns its offset") {
    std::string withSentinel = "membersOf(\x7F)";
    const int at = StripCaretAnchorSentinel(withSentinel);
    CHECK(at == 10); // byte offset of the sentinel
    CHECK(withSentinel == "membersOf()");
    // No sentinel — text untouched, returns -1.
    std::string plain = "assignee = currentUser()";
    const std::string before = plain;
    CHECK(StripCaretAnchorSentinel(plain) == -1);
    CHECK(plain == before);
}
