// PaneBackendTargets.test.cpp — bucket-A coverage of the grid header "+ ▾" new-pane picker rows
// (#2444): with two Jira sites and no other tracker the caret used to stay hidden, because the
// picker counted credentialed tracker KINDS and two Jira sites are one kind.
//
// Pure data-level tests — no ImGui, no UiDrawSession, no disk I/O.

#include "Config/ConfigManager.h"
#include "GridPane.h"
#include "SmatchetGridPaneWindows.h"
#include "Ui/PaneBackendTargetsPure.h"

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using smatchet::pane_targets::BuildNewPaneTargets;
using smatchet::pane_targets::NewPaneTarget;
using smatchet::pane_targets::ShowNewPaneCaret;

namespace {

JiraBackendInstance Site(const std::string& domain, const std::string& token) {
    JiraBackendInstance inst;
    inst.Domain = domain;
    inst.Email = "";
    inst.ApiToken = token;
    return inst;
}

// Jira only, live fields on the first site (the shape ConfigManager::Load leaves).
TrackerConfig JiraCfg(const std::vector<JiraBackendInstance>& sites) {
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.JiraBackends = sites;
    if (!sites.empty()) {
        cfg.Domain = sites[0].Domain;
        cfg.ApiToken = sites[0].ApiToken;
    }
    return cfg;
}

} // namespace

TEST_CASE("BuildNewPaneTargets: two Jira sites and nothing else show the caret (#2444)") {
    const TrackerConfig cfg = JiraCfg({Site("acme.atlassian.net", "tok"), Site("other.atlassian.net", "")});
    const std::vector<NewPaneTarget> targets = BuildNewPaneTargets(cfg);
    REQUIRE(targets.size() == 2);
    CHECK(ShowNewPaneCaret(targets));
    CHECK(targets[0].BackendKey == "Jira");
    CHECK(targets[0].Label == "New Jira \xC2\xB7 acme.atlassian.net pane");
    CHECK(targets[0].SwitchToJiraDomain.empty()); // already the active site
    CHECK(targets[1].BackendKey == "Jira");
    CHECK(targets[1].Label == "New Jira \xC2\xB7 other.atlassian.net pane");
    CHECK(targets[1].SwitchToJiraDomain == "other.atlassian.net"); // inherits the first site's token
}

TEST_CASE("BuildNewPaneTargets: the active site follows ActiveJiraDomain, not the first entry") {
    TrackerConfig cfg = JiraCfg({Site("acme.atlassian.net", "tok"), Site("https://Other.atlassian.net/", "tok2")});
    cfg.ActiveJiraDomain = "other.atlassian.net";
    const std::vector<NewPaneTarget> targets = BuildNewPaneTargets(cfg);
    REQUIRE(targets.size() == 2);
    CHECK(targets[0].SwitchToJiraDomain == "acme.atlassian.net");
    CHECK(targets[1].SwitchToJiraDomain.empty());
    CHECK(targets[1].Label == "New Jira \xC2\xB7 other.atlassian.net pane"); // host, not the raw URL
}

TEST_CASE("BuildNewPaneTargets: one Jira site keeps the single unchanged row and no caret") {
    const TrackerConfig hydrated = JiraCfg({Site("acme.atlassian.net", "tok")});
    std::vector<NewPaneTarget> targets = BuildNewPaneTargets(hydrated);
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].Label == "New Jira pane");
    CHECK(targets[0].SwitchToJiraDomain.empty());
    CHECK_FALSE(ShowNewPaneCaret(targets));

    // Not yet hydrated (JiraBackends empty): the live Domain is the one site.
    TrackerConfig legacy;
    legacy.Domain = "acme.atlassian.net";
    legacy.ApiToken = "tok";
    targets = BuildNewPaneTargets(legacy);
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].Label == "New Jira pane");
}

TEST_CASE("BuildNewPaneTargets: a second site with no host or no token to inherit is not offered") {
    const TrackerConfig noHost = JiraCfg({Site("acme.atlassian.net", "tok"), Site("   ", "tok2")});
    CHECK_FALSE(ShowNewPaneCaret(BuildNewPaneTargets(noHost)));

    // The first site has no token: Jira is not credentialed at all, so no Jira row.
    TrackerConfig noToken = JiraCfg({Site("acme.atlassian.net", ""), Site("other.atlassian.net", "")});
    CHECK(BuildNewPaneTargets(noToken).empty());
}

TEST_CASE("BuildNewPaneTargets: only the active extra site has a token, so one site is usable") {
    // The first site has no token for the extra to inherit; the live fields carry the active extra's own.
    TrackerConfig cfg = JiraCfg({Site("acme.atlassian.net", ""), Site("other.atlassian.net", "tok2")});
    cfg.ActiveJiraDomain = "other.atlassian.net";
    cfg.Domain = "other.atlassian.net";
    cfg.ApiToken = "tok2";
    const std::vector<NewPaneTarget> targets = BuildNewPaneTargets(cfg);
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].Label == "New Jira pane");
    CHECK(targets[0].SwitchToJiraDomain.empty());
    CHECK_FALSE(ShowNewPaneCaret(targets));
}

TEST_CASE("BuildNewPaneTargets: Jira sites and other trackers together, in KnownBackendKeys order") {
    TrackerConfig cfg = JiraCfg({Site("acme.atlassian.net", "tok"), Site("other.atlassian.net", "tok2")});
    cfg.PlaneUrl = "https://plane.example";
    cfg.PlaneApiKey = "pk";
    cfg.PlaneWorkspaceSlug = "ws";
    const std::vector<NewPaneTarget> targets = BuildNewPaneTargets(cfg);
    REQUIRE(targets.size() == 3);
    CHECK(targets[0].BackendKey == "Jira");
    CHECK(targets[1].BackendKey == "Jira");
    CHECK(targets[2].BackendKey == "Plane");
    CHECK(targets[2].Label == "New Plane pane");
    CHECK(targets[2].SwitchToJiraDomain.empty());
}

TEST_CASE("BuildNewPaneTargets: one Jira site plus Plane is the pre-#2444 picker, byte for byte") {
    TrackerConfig cfg = JiraCfg({Site("acme.atlassian.net", "tok")});
    cfg.PlaneUrl = "https://plane.example";
    cfg.PlaneApiKey = "pk";
    cfg.PlaneWorkspaceSlug = "ws";
    const std::vector<NewPaneTarget> targets = BuildNewPaneTargets(cfg);
    REQUIRE(targets.size() == 2);
    CHECK(ShowNewPaneCaret(targets));
    CHECK(targets[0].Label == "New Jira pane");
    CHECK(targets[1].Label == "New Plane pane");
}

TEST_CASE("ApplyPaneAddAndCloseRequestsCore: a Jira site switch never inherits the source pane's rows") {
    GridPane src;
    src.id = "main";
    src.backendKey = "Jira";
    src.viewId = "v1";
    src.ticketsSnapshot = std::make_shared<const std::vector<CachedTicket>>(std::vector<CachedTicket>(3));
    src.snapshotRevision = 7;
    std::vector<GridPane> panes{src};
    std::string focused = "main";
    PaneAddRequest req;
    req.sourceId = "main";
    req.targetBackendKey = "Jira";
    req.targetJiraDomain = "other.atlassian.net";
    const std::unordered_map<std::string, ViewWorkspaceState> buckets;

    const auto outcome =
        SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focused, req, buckets);

    REQUIRE(panes.size() == 2);
    CHECK(outcome.Changed);
    CHECK(panes[1].backendKey == "Jira");
    CHECK(panes[1].viewId == "v1");
    CHECK((panes[1].ticketsSnapshot == nullptr)); // the other site's rows arrive with its own first sync
    CHECK(focused == panes[1].id);
    CHECK(req.sourceId.empty()); // consumed
    CHECK(req.targetJiraDomain.empty());

    // Control: the same request without a site switch still duplicates the snapshot.
    PaneAddRequest dup;
    dup.sourceId = "main";
    SmatchetGridPaneWindows::detail::ApplyPaneAddAndCloseRequestsCore(panes, focused, dup, buckets);
    REQUIRE(panes.size() == 3);
    CHECK((panes[2].ticketsSnapshot == src.ticketsSnapshot));
}
