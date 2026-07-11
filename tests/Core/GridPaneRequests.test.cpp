// GridPaneRequests.test.cpp — bucket-A coverage of the pure pane-request core
// (SmatchetGridPaneWindows_detail.cpp), covering the pane-request review fixes:
//
//   * HIGH-2 — closing the FOCUSED pane reassigns focus to the survivor AND
//     reports FocusReassigned so the host replays it as a real focus switch
//     (activating the survivor's saved view); the survivor's own identity
//     (viewId / backendKey) is never touched by the sweep.
//   * HIGH-1 — PaneViewSelfRepairAllowed: a cross-backend pane's dangling
//     viewId must NOT be self-repaired (rebound) to the focused backend's
//     active view; same-backend (and pre-bootstrap empty-key) panes may.
//   * "+" duplicate request reassigns focus to the new pane and reports it.
//   * Min-1 invariant: a close request against the last pane is ignored and
//     the pane is re-armed open.
//
// Pure data-level tests — no ImGui, no UiDrawSession, no UI loop.

#include "GridPane.h"
#include "SmatchetGridPaneWindows.h"

#include <doctest/doctest.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace {

GridPane MakePane(const std::string& id, const std::string& backendKey, const std::string& viewId) {
    GridPane pane;
    pane.id = id;
    pane.title = id;
    pane.backendKey = backendKey;
    pane.viewId = viewId;
    return pane;
}

} // namespace

TEST_CASE("Pane requests: closing the focused pane reassigns focus + reports it; survivor identity untouched") {
    std::vector<GridPane> panes;
    panes.push_back(MakePane("main", "Jira", "jira_view_a"));
    panes.push_back(MakePane("pane-2", "Jira", "jira_view_b"));
    std::string focusedPaneId = "pane-2";
    PaneAddRequest addRequest;
    const std::unordered_map<std::string, ViewWorkspaceState> emptyBuckets;

    panes[1].open = false; // tab X on the focused pane

    const auto outcome =
        SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focusedPaneId, addRequest,
                                                                          emptyBuckets);

    CHECK(outcome.Changed);
    CHECK(outcome.FocusReassigned); // HIGH-2: host must replay as a focus switch
    REQUIRE(panes.size() == 1);
    CHECK(focusedPaneId == "main");
    // Survivor keeps ITS OWN saved view — the sweep never rebinds identity.
    CHECK(panes[0].viewId == "jira_view_a");
    CHECK(panes[0].backendKey == "Jira");
    CHECK(panes[0].open);
}

TEST_CASE("Pane requests: closing a NON-focused pane does not report a focus reassignment") {
    std::vector<GridPane> panes;
    panes.push_back(MakePane("main", "Jira", "jira_view_a"));
    panes.push_back(MakePane("pane-2", "Plane", "plane_view"));
    std::string focusedPaneId = "main";
    PaneAddRequest addRequest;
    const std::unordered_map<std::string, ViewWorkspaceState> emptyBuckets;

    panes[1].open = false;

    const auto outcome =
        SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focusedPaneId, addRequest,
                                                                          emptyBuckets);

    CHECK(outcome.Changed);
    CHECK_FALSE(outcome.FocusReassigned);
    REQUIRE(panes.size() == 1);
    CHECK(focusedPaneId == "main");
}

TEST_CASE("Pane requests: min-1 invariant — last pane survives a close request and re-arms open") {
    std::vector<GridPane> panes;
    panes.push_back(MakePane("main", "Jira", "jira_view_a"));
    std::string focusedPaneId = "main";
    PaneAddRequest addRequest;
    const std::unordered_map<std::string, ViewWorkspaceState> emptyBuckets;

    panes[0].open = false;

    const auto outcome =
        SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focusedPaneId, addRequest,
                                                                          emptyBuckets);

    CHECK_FALSE(outcome.Changed);
    CHECK_FALSE(outcome.FocusReassigned);
    REQUIRE(panes.size() == 1);
    CHECK(panes[0].open);
    CHECK(focusedPaneId == "main");
}

TEST_CASE("Pane requests: '+' duplicate consumes the request, focuses the new pane, reports reassignment") {
    std::vector<GridPane> panes;
    panes.push_back(MakePane("main", "Jira", "jira_view_a"));
    std::string focusedPaneId = "main";
    PaneAddRequest addRequest;
    addRequest.sourceId = "main";
    const std::unordered_map<std::string, ViewWorkspaceState> emptyBuckets;

    const auto outcome =
        SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focusedPaneId, addRequest,
                                                                          emptyBuckets);

    CHECK(outcome.Changed);
    CHECK(outcome.FocusReassigned);
    CHECK(addRequest.sourceId.empty()); // consume-once
    REQUIRE(panes.size() == 2);
    CHECK(panes[1].id == "pane-2");
    CHECK(panes[1].viewId == "jira_view_a"); // dup shares the source's view
    CHECK(panes[1].backendKey == "Jira");
    CHECK(focusedPaneId == "pane-2");
}

TEST_CASE("Pane cycling: NextPaneId / PrevPaneId wrap around the render order (Slice 4)") {
    std::vector<GridPane> panes;
    panes.push_back(MakePane("main", "Jira", "v_a"));
    panes.push_back(MakePane("pane-2", "Jira", "v_b"));
    panes.push_back(MakePane("pane-3", "Jira", "v_c"));

    // Forward steps + wrap past the last back to the first.
    CHECK(SmatchetGridPaneWindows::NextPaneId(panes, "main") == "pane-2");
    CHECK(SmatchetGridPaneWindows::NextPaneId(panes, "pane-2") == "pane-3");
    CHECK(SmatchetGridPaneWindows::NextPaneId(panes, "pane-3") == "main");

    // Backward steps + wrap past the front round to the last.
    CHECK(SmatchetGridPaneWindows::PrevPaneId(panes, "pane-3") == "pane-2");
    CHECK(SmatchetGridPaneWindows::PrevPaneId(panes, "pane-2") == "main");
    CHECK(SmatchetGridPaneWindows::PrevPaneId(panes, "main") == "pane-3");
}

TEST_CASE("Pane cycling: single pane next/prev resolve to itself; unknown current → first pane") {
    std::vector<GridPane> one;
    one.push_back(MakePane("main", "Jira", "v_a"));
    CHECK(SmatchetGridPaneWindows::NextPaneId(one, "main") == "main");
    CHECK(SmatchetGridPaneWindows::PrevPaneId(one, "main") == "main");

    std::vector<GridPane> two;
    two.push_back(MakePane("main", "Jira", "v_a"));
    two.push_back(MakePane("pane-2", "Jira", "v_b"));
    // An unknown current id falls back to the first pane (safe focus fallback).
    CHECK(SmatchetGridPaneWindows::NextPaneId(two, "ghost") == "main");
    CHECK(SmatchetGridPaneWindows::PrevPaneId(two, "ghost") == "main");

    // Empty pane set → empty string (no crash).
    std::vector<GridPane> none;
    CHECK(SmatchetGridPaneWindows::NextPaneId(none, "main").empty());
    CHECK(SmatchetGridPaneWindows::PrevPaneId(none, "main").empty());
}

TEST_CASE("Pane columns source: own active view renders the shared build (and captures it)") {
    using SmatchetGridPaneWindows::detail::ChoosePaneColumnsSource;
    using SmatchetGridPaneWindows::detail::PaneColumnsSource;
    // Focused pane steady state: pane.viewId == resolved == active.
    CHECK(ChoosePaneColumnsSource("v1", "v1", "v1", /*cachedColumnsValid=*/false, "") ==
          PaneColumnsSource::SharedActive);
}

TEST_CASE("Pane columns source: own non-active view uses the per-pane build (same-backend unfocused pane)") {
    using SmatchetGridPaneWindows::detail::ChoosePaneColumnsSource;
    using SmatchetGridPaneWindows::detail::PaneColumnsSource;
    // Unfocused same-backend pane: its view resolved from the loaded bucket, but a
    // different view is active.
    CHECK(ChoosePaneColumnsSource("v1", "v1", "v2", /*cachedColumnsValid=*/true, "v1") ==
          PaneColumnsSource::OwnViewBuild);
}

TEST_CASE("Pane columns source: fallback-resolved view never leaks the focused field set (user defect)") {
    using SmatchetGridPaneWindows::detail::ChoosePaneColumnsSource;
    using SmatchetGridPaneWindows::detail::PaneColumnsSource;
    // Cross-backend unfocused pane: its viewId is unresolvable in the loaded bucket,
    // so resolvePaneView handed back the ACTIVE view ("v2") as a render fallback.
    // With a bind-time capture for the pane's own view, the frozen capture wins —
    // the pane must NOT render v2's columns.
    CHECK(ChoosePaneColumnsSource("plane_v1", "v2", "v2", /*cachedColumnsValid=*/true, "plane_v1") ==
          PaneColumnsSource::CachedFrozen);

    // A stale capture for a DIFFERENT view does not qualify — fall back to shared.
    CHECK(ChoosePaneColumnsSource("plane_v1", "v2", "v2", /*cachedColumnsValid=*/true, "plane_v0") ==
          PaneColumnsSource::SharedFallback);

    // Cold start (restored from disk, never focused, nothing captured): shared fallback.
    CHECK(ChoosePaneColumnsSource("plane_v1", "v2", "v2", /*cachedColumnsValid=*/false, "") ==
          PaneColumnsSource::SharedFallback);

    // No view resolvable at all (empty bucket) but a capture exists: keep the capture.
    CHECK(ChoosePaneColumnsSource("plane_v1", "", "", /*cachedColumnsValid=*/true, "plane_v1") ==
          PaneColumnsSource::CachedFrozen);
}

TEST_CASE("Pane columns source: empty pane viewId never claims ownership or a capture") {
    using SmatchetGridPaneWindows::detail::ChoosePaneColumnsSource;
    using SmatchetGridPaneWindows::detail::PaneColumnsSource;
    // Pre-bootstrap placeholder pane (empty viewId, nothing resolved/active yet).
    CHECK(ChoosePaneColumnsSource("", "", "", /*cachedColumnsValid=*/false, "") == PaneColumnsSource::SharedFallback);
    // Even a (bogus) valid cache with an empty key must not match an empty pane id.
    CHECK(ChoosePaneColumnsSource("", "v1", "v1", /*cachedColumnsValid=*/true, "") ==
          PaneColumnsSource::SharedFallback);
}

TEST_CASE("Cold-start upgrade: a pane's OWN context-resolved view rebuilds its columns past SharedFallback (Slice 4)") {
    using SmatchetGridPaneWindows::detail::PaneColumnsSource;
    using SmatchetGridPaneWindows::detail::ShouldBuildColumnsFromOwnResolvedView;
    // The user defect this closes: after a restart a cross-backend pane has no session
    // capture (SharedFallback) and would render the focused view's columns. Its context's
    // own resolved view ("plane_v1", published by the first-sync worker) matches the
    // pane's own viewId → rebuild from the pane's REAL view, not the fallback.
    CHECK(ShouldBuildColumnsFromOwnResolvedView(PaneColumnsSource::SharedFallback, "plane_v1", "plane_v1"));
    // A context-resolved view that is NOT the pane's own must never drive the build.
    CHECK_FALSE(ShouldBuildColumnsFromOwnResolvedView(PaneColumnsSource::SharedFallback, "plane_v1", "jira_v2"));
    // Only the fallback path upgrades — an own/active or already-frozen source is left alone.
    CHECK_FALSE(ShouldBuildColumnsFromOwnResolvedView(PaneColumnsSource::SharedActive, "plane_v1", "plane_v1"));
    CHECK_FALSE(ShouldBuildColumnsFromOwnResolvedView(PaneColumnsSource::CachedFrozen, "plane_v1", "plane_v1"));
    CHECK_FALSE(ShouldBuildColumnsFromOwnResolvedView(PaneColumnsSource::OwnViewBuild, "plane_v1", "plane_v1"));
    // Empty pane id never owns a resolved view (pre-bootstrap placeholder).
    CHECK_FALSE(ShouldBuildColumnsFromOwnResolvedView(PaneColumnsSource::SharedFallback, "", ""));
}

TEST_CASE("Pane view self-repair: cross-backend pane viewId is never rebound (HIGH-1)") {
    // A Plane pane rendered while Jira is the focused backend: its viewId is valid
    // in the Plane bucket — the loaded Jira slice simply can't see it. Repair must
    // be refused so the identity survives any number of frames + persists intact.
    CHECK_FALSE(SmatchetGridPaneWindows::detail::PaneViewSelfRepairAllowed("Plane", "Jira"));

    // Same-backend pane with a genuinely deleted view: repair allowed (Pillar 3).
    CHECK(SmatchetGridPaneWindows::detail::PaneViewSelfRepairAllowed("Jira", "Jira"));

    // Pre-bootstrap placeholder pane (empty key): repair allowed.
    CHECK(SmatchetGridPaneWindows::detail::PaneViewSelfRepairAllowed("", "Jira"));
}
