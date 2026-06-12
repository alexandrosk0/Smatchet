// PaneAddRequest.test.cpp — bucket-A coverage of pane-backend-picker Slice 1:
//   * PaneAddRequest sentinel (sourceId.empty() = no request).
//   * Same-backend duplicate: backendKey/viewId/snapshot inherited from source.
//   * Cross-backend create: targetBackendKey used, no snapshot inherit, viewId
//     resolved via ResolveNewPaneView (requested → active → first → empty).
//   * ResolveNewPaneView resolution order: requested-id-if-valid → ActiveViewId
//     → first view → empty when bucket absent/empty.
//
// Pure data-level tests — no ImGui, no UiDrawSession, no disk I/O.

#include "Config/ConfigManager.h"
#include "GridPane.h"
#include "SmatchetGridPaneWindows.h"

#include <doctest/doctest.h>

#include <memory>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

using SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore;
using SmatchetGridPaneWindows::detail::ResolveNewPaneView;

namespace {

GridPane MakePane(const std::string& id, const std::string& backendKey, const std::string& viewId) {
    GridPane p;
    p.id = id;
    p.backendKey = backendKey;
    p.viewId = viewId;
    return p;
}

ViewDefinition MakeView(const std::string& id) {
    ViewDefinition v;
    v.Id = id;
    v.Name = id;
    return v;
}

ViewWorkspaceState MakeBucket(const std::string& activeViewId,
                               const std::vector<std::string>& viewIds) {
    ViewWorkspaceState ws;
    ws.ActiveViewId = activeViewId;
    for (const auto& vid : viewIds) {
        ws.Views.push_back(MakeView(vid));
    }
    return ws;
}

} // namespace

// ---------------------------------------------------------------------------
// ResolveNewPaneView
// ---------------------------------------------------------------------------

TEST_CASE("ResolveNewPaneView: bucket absent → empty string") {
    const std::unordered_map<std::string, ViewWorkspaceState> empty;
    CHECK(ResolveNewPaneView("Jira", "", empty) == "");
    CHECK(ResolveNewPaneView("Jira", "v1", empty) == "");
}

TEST_CASE("ResolveNewPaneView: empty bucket → empty string") {
    std::unordered_map<std::string, ViewWorkspaceState> buckets;
    buckets["Jira"] = ViewWorkspaceState{};
    CHECK(ResolveNewPaneView("Jira", "v1", buckets) == "");
}

TEST_CASE("ResolveNewPaneView: requested id valid → returned") {
    std::unordered_map<std::string, ViewWorkspaceState> buckets;
    buckets["Jira"] = MakeBucket("v2", {"v1", "v2", "v3"});
    CHECK(ResolveNewPaneView("Jira", "v3", buckets) == "v3");
}

TEST_CASE("ResolveNewPaneView: requested id invalid → falls back to ActiveViewId") {
    std::unordered_map<std::string, ViewWorkspaceState> buckets;
    buckets["Jira"] = MakeBucket("v2", {"v1", "v2"});
    CHECK(ResolveNewPaneView("Jira", "no-such-view", buckets) == "v2");
}

TEST_CASE("ResolveNewPaneView: empty requested + empty ActiveViewId → first view") {
    std::unordered_map<std::string, ViewWorkspaceState> buckets;
    buckets["Plane"] = MakeBucket("", {"plane_default_view"});
    CHECK(ResolveNewPaneView("Plane", "", buckets) == "plane_default_view");
}

TEST_CASE("ResolveNewPaneView: empty requested + ActiveViewId → ActiveViewId") {
    std::unordered_map<std::string, ViewWorkspaceState> buckets;
    buckets["GitHub"] = MakeBucket("github_default_view", {"github_default_view", "v2"});
    CHECK(ResolveNewPaneView("GitHub", "", buckets) == "github_default_view");
}

// ---------------------------------------------------------------------------
// ApplyPaneAddAndCloseRequestsCore — PaneAddRequest sentinel + same-backend
// ---------------------------------------------------------------------------

TEST_CASE("ApplyPaneAddAndCloseRequestsCore: empty sourceId → no pane added") {
    std::vector<GridPane> panes = {MakePane("main", "Jira", "v1")};
    std::string focused = "main";
    PaneAddRequest req;
    const std::unordered_map<std::string, ViewWorkspaceState> empty;
    const auto out = ApplyPaneAddAndCloseRequestsCore(panes, focused, req, empty);
    CHECK(!out.Changed);
    CHECK(!out.FocusReassigned);
    CHECK(panes.size() == 1);
}

TEST_CASE("ApplyPaneAddAndCloseRequestsCore: same-backend duplicate inherits backend/view/snapshot") {
    auto snap = std::make_shared<const std::vector<CachedTicket>>();
    GridPane src = MakePane("main", "Jira", "v1");
    src.ticketsSnapshot = snap;
    src.snapshotRevision = 42;
    std::vector<GridPane> panes = {src};
    std::string focused = "main";
    PaneAddRequest req;
    req.sourceId = "main";
    // targetBackendKey empty → same-backend duplicate
    const std::unordered_map<std::string, ViewWorkspaceState> empty;
    const auto out = ApplyPaneAddAndCloseRequestsCore(panes, focused, req, empty);
    CHECK(out.Changed);
    CHECK(out.FocusReassigned);
    REQUIRE(panes.size() == 2);
    const GridPane& dup = panes.back();
    CHECK(dup.backendKey == "Jira");
    CHECK(dup.viewId == "v1");
    CHECK(dup.ticketsSnapshot.get() == snap.get()); // shared_ptr identity, not printed by doctest
    CHECK(dup.snapshotRevision == 42);
    CHECK(req.sourceId.empty()); // request consumed
}

// ---------------------------------------------------------------------------
// ApplyPaneAddAndCloseRequestsCore — cross-backend create
// ---------------------------------------------------------------------------

TEST_CASE("ApplyPaneAddAndCloseRequestsCore: cross-backend sets targetBackendKey, no snapshot inherit") {
    auto snap = std::make_shared<const std::vector<CachedTicket>>();
    GridPane src = MakePane("main", "Jira", "v1");
    src.ticketsSnapshot = snap;
    src.snapshotRevision = 7;
    std::vector<GridPane> panes = {src};
    std::string focused = "main";
    PaneAddRequest req;
    req.sourceId = "main";
    req.targetBackendKey = "Plane";
    req.targetViewId = "plane_default_view";

    std::unordered_map<std::string, ViewWorkspaceState> buckets;
    buckets["Plane"] = MakeBucket("plane_default_view", {"plane_default_view"});

    const auto out = ApplyPaneAddAndCloseRequestsCore(panes, focused, req, buckets);
    CHECK(out.Changed);
    CHECK(out.FocusReassigned);
    REQUIRE(panes.size() == 2);
    const GridPane& dup = panes.back();
    CHECK(dup.backendKey == "Plane");
    CHECK(dup.viewId == "plane_default_view");
    CHECK(dup.ticketsSnapshot.get() == nullptr); // cross-backend: no snapshot inherited
    CHECK(dup.snapshotRevision == 0);
}

TEST_CASE("ApplyPaneAddAndCloseRequestsCore: cross-backend with empty targetViewId resolves default") {
    std::vector<GridPane> panes = {MakePane("main", "Jira", "v1")};
    std::string focused = "main";
    PaneAddRequest req;
    req.sourceId = "main";
    req.targetBackendKey = "GitHub";
    // targetViewId empty → ResolveNewPaneView picks active view

    std::unordered_map<std::string, ViewWorkspaceState> buckets;
    buckets["GitHub"] = MakeBucket("github_default_view", {"github_default_view"});

    const auto out = ApplyPaneAddAndCloseRequestsCore(panes, focused, req, buckets);
    REQUIRE(panes.size() == 2);
    CHECK(panes.back().backendKey == "GitHub");
    CHECK(panes.back().viewId == "github_default_view");
}

TEST_CASE("ApplyPaneAddAndCloseRequestsCore: cross-backend with empty bucket → empty viewId") {
    std::vector<GridPane> panes = {MakePane("main", "Jira", "v1")};
    std::string focused = "main";
    PaneAddRequest req;
    req.sourceId = "main";
    req.targetBackendKey = "Plane";

    const std::unordered_map<std::string, ViewWorkspaceState> empty;

    const auto out = ApplyPaneAddAndCloseRequestsCore(panes, focused, req, empty);
    REQUIRE(panes.size() == 2);
    CHECK(panes.back().backendKey == "Plane");
    CHECK(panes.back().viewId == ""); // bucket absent → steady-state resolves
}

TEST_CASE("ApplyPaneAddAndCloseRequestsCore: same targetBackendKey as source is NOT cross-backend") {
    auto snap = std::make_shared<const std::vector<CachedTicket>>();
    GridPane src = MakePane("main", "Jira", "v1");
    src.ticketsSnapshot = snap;
    src.snapshotRevision = 5;
    std::vector<GridPane> panes = {src};
    std::string focused = "main";
    PaneAddRequest req;
    req.sourceId = "main";
    req.targetBackendKey = "Jira"; // same as source → NOT cross-backend
    req.targetViewId = "v2";

    const std::unordered_map<std::string, ViewWorkspaceState> empty;
    ApplyPaneAddAndCloseRequestsCore(panes, focused, req, empty);
    REQUIRE(panes.size() == 2);
    // Same-backend branch: copies source viewId, not targetViewId
    CHECK(panes.back().backendKey == "Jira");
    CHECK(panes.back().viewId == "v1");
    CHECK(panes.back().ticketsSnapshot.get() == snap.get());
}

// ---------------------------------------------------------------------------
// Slice 3 — BackendCredentialsPresent + KnownBackendKeys (pane.new backend/view params)
// ---------------------------------------------------------------------------


namespace {

TrackerConfig MakeCfg(const std::string& domain, const std::string& apiToken,
                      const std::string& planeUrl, const std::string& planeApiKey,
                      const std::string& planeWorkspaceSlug,
                      const std::string& ghPat, const std::string& ghOwner,
                      const std::string& ghRepo) {
    TrackerConfig cfg;
    cfg.Domain   = domain;   cfg.ApiToken   = apiToken;
    cfg.PlaneUrl = planeUrl; cfg.PlaneApiKey = planeApiKey;
    cfg.PlaneWorkspaceSlug = planeWorkspaceSlug;
    cfg.GitHubPat = ghPat;   cfg.GitHubOwner = ghOwner; cfg.GitHubRepo = ghRepo;
    return cfg;
}

} // namespace

TEST_CASE("BackendCredentialsPresent: Jira — all fields present") {
    const TrackerConfig cfg = MakeCfg("my.jira.net", "tok", "", "", "", "", "", "");
    CHECK(ConfigManager::BackendCredentialsPresent(cfg, "Jira"));
}

TEST_CASE("BackendCredentialsPresent: Jira — domain empty → false") {
    const TrackerConfig cfg = MakeCfg("", "tok", "", "", "", "", "", "");
    CHECK_FALSE(ConfigManager::BackendCredentialsPresent(cfg, "Jira"));
}

TEST_CASE("BackendCredentialsPresent: Jira — apiToken empty → false") {
    const TrackerConfig cfg = MakeCfg("my.jira.net", "", "", "", "", "", "", "");
    CHECK_FALSE(ConfigManager::BackendCredentialsPresent(cfg, "Jira"));
}

TEST_CASE("BackendCredentialsPresent: Plane — all fields present") {
    const TrackerConfig cfg = MakeCfg("", "", "https://plane.so", "plane_tok", "my-ws", "", "", "");
    CHECK(ConfigManager::BackendCredentialsPresent(cfg, "Plane"));
}

TEST_CASE("BackendCredentialsPresent: Plane — url empty → false") {
    const TrackerConfig cfg = MakeCfg("", "", "", "plane_tok", "my-ws", "", "", "");
    CHECK_FALSE(ConfigManager::BackendCredentialsPresent(cfg, "Plane"));
}

TEST_CASE("BackendCredentialsPresent: Plane — apiKey empty → false") {
    const TrackerConfig cfg = MakeCfg("", "", "https://plane.so", "", "my-ws", "", "", "");
    CHECK_FALSE(ConfigManager::BackendCredentialsPresent(cfg, "Plane"));
}

TEST_CASE("BackendCredentialsPresent: Plane — workspace slug empty → false") {
    const TrackerConfig cfg = MakeCfg("", "", "https://plane.so", "plane_tok", "", "", "", "");
    CHECK_FALSE(ConfigManager::BackendCredentialsPresent(cfg, "Plane"));
}

TEST_CASE("BackendCredentialsPresent: GitHub — all fields present") {
    const TrackerConfig cfg = MakeCfg("", "", "", "", "", "ghp_tok", "owner", "repo");
    CHECK(ConfigManager::BackendCredentialsPresent(cfg, "GitHub"));
}

TEST_CASE("BackendCredentialsPresent: GitHub — pat empty → false") {
    const TrackerConfig cfg = MakeCfg("", "", "", "", "", "", "owner", "repo");
    CHECK_FALSE(ConfigManager::BackendCredentialsPresent(cfg, "GitHub"));
}

TEST_CASE("BackendCredentialsPresent: unknown key → false (Jira fallback check fails)") {
    // Unknown backend key falls through to the Jira branch (non-empty domain+token check).
    // With empty domain → false regardless.
    const TrackerConfig cfg = MakeCfg("", "", "", "", "", "", "", "");
    CHECK_FALSE(ConfigManager::BackendCredentialsPresent(cfg, "Linear"));
}

TEST_CASE("KnownBackendKeys: contains Jira, Plane, GitHub") {
    const std::vector<std::string>& keys = ConfigManager::KnownBackendKeys();
    const bool hasJira   = std::find(keys.begin(), keys.end(), "Jira")   != keys.end();
    const bool hasPlane  = std::find(keys.begin(), keys.end(), "Plane")  != keys.end();
    const bool hasGitHub = std::find(keys.begin(), keys.end(), "GitHub") != keys.end();
    CHECK(hasJira);
    CHECK(hasPlane);
    CHECK(hasGitHub);
}

TEST_CASE("PaneAddRequest: backend + view fields round-trip") {
    PaneAddRequest req;
    req.sourceId          = "pane-1";
    req.targetBackendKey  = "GitHub";
    req.targetViewId      = "view-42";
    CHECK(req.sourceId == "pane-1");
    CHECK(req.targetBackendKey == "GitHub");
    CHECK(req.targetViewId == "view-42");
}
