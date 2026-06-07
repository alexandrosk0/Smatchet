// GridPaneRequests.test.cpp — bucket-A coverage of the pure pane-request core
// (SmatchetGridPaneWindows_detail.cpp), added with the PR #962 review fixes:
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
    std::string addRequest;

    panes[1].open = false; // tab X on the focused pane

    const auto outcome =
        SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focusedPaneId, addRequest);

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
    std::string addRequest;

    panes[1].open = false;

    const auto outcome =
        SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focusedPaneId, addRequest);

    CHECK(outcome.Changed);
    CHECK_FALSE(outcome.FocusReassigned);
    REQUIRE(panes.size() == 1);
    CHECK(focusedPaneId == "main");
}

TEST_CASE("Pane requests: min-1 invariant — last pane survives a close request and re-arms open") {
    std::vector<GridPane> panes;
    panes.push_back(MakePane("main", "Jira", "jira_view_a"));
    std::string focusedPaneId = "main";
    std::string addRequest;

    panes[0].open = false;

    const auto outcome =
        SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focusedPaneId, addRequest);

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
    std::string addRequest = "main";

    const auto outcome =
        SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focusedPaneId, addRequest);

    CHECK(outcome.Changed);
    CHECK(outcome.FocusReassigned);
    CHECK(addRequest.empty()); // consume-once
    REQUIRE(panes.size() == 2);
    CHECK(panes[1].id == "pane-2");
    CHECK(panes[1].viewId == "jira_view_a"); // dup shares the source's view
    CHECK(panes[1].backendKey == "Jira");
    CHECK(focusedPaneId == "pane-2");
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
