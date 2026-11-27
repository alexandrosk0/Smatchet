#include <doctest/doctest.h>

#include "ITrackerConnectivity.h"
#include "JqlProjectScope.h"
#include "PaneCatalogScopePure.h"
#include "Sync/CatalogRestorePure.h"

#include <string>
#include <vector>

// The pure project-key derivation behind a grid pane's own field catalog
// (AppController::applyPaneSyncKickOnMainThread_ -> populatePaneCatalogAfterSync_). Before it, the pane
// always fetched unscoped, so Jira skipped createmeta and the pane's component / version dropdowns were
// empty, with "field catalog fetched WITHOUT project scope" logged on every kick.

namespace {

// Parses the way JiraClient::ExtractProjectFromQuery does (JqlProjectScope::ExtractSingleProject), without
// linking the HTTP client into the test rig.
class JqlConnectivity : public ITrackerConnectivity {
  public:
    std::string GetTrackerType() const override { return "Jira"; }
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& /*cfg*/) override {
        return TrackerReachabilityProbeResult();
    }
    std::string ExtractProjectFromQuery(const std::string& query) const override {
        ++Calls;
        return JqlProjectScope::ExtractSingleProject(query);
    }
    mutable int Calls = 0;
};

// A backend with no query parser (the ITrackerConnectivity default): never scoped.
class NoParserConnectivity : public ITrackerConnectivity {
  public:
    std::string GetTrackerType() const override { return "Fixture"; }
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& /*cfg*/) override {
        return TrackerReachabilityProbeResult();
    }
};

} // namespace

using smatchet::catalog::PaneCatalogProjectKey;

TEST_CASE("PaneCatalogProjectKey: a view naming one project scopes the pane's catalog to it") {
    const JqlConnectivity jira;
    CHECK(PaneCatalogProjectKey(&jira, "project = KEY") == "KEY");
    CHECK(PaneCatalogProjectKey(&jira, "project = KEY AND status = Open ORDER BY updated DESC") == "KEY");
    CHECK(PaneCatalogProjectKey(&jira, "assignee = currentUser() AND PROJECT=key2") == "key2");
    CHECK(PaneCatalogProjectKey(&jira, "project = \"Quoted Key\"") == "Quoted Key");
    CHECK(PaneCatalogProjectKey(&jira, "project in (SOLO)") == "SOLO");
}

TEST_CASE("PaneCatalogProjectKey: zero or several projects keep the documented unscoped fallback") {
    const JqlConnectivity jira;
    // The bootstrap default view names no project.
    CHECK(PaneCatalogProjectKey(&jira, "assignee = currentUser()").empty());
    CHECK(PaneCatalogProjectKey(&jira, "project in (A, B)").empty());
    CHECK(PaneCatalogProjectKey(&jira, "project = A OR project = B").empty());
    // A project clause inside a quoted string is text, not a scope.
    CHECK(PaneCatalogProjectKey(&jira, "summary ~ \"project = X\"").empty());
    // A word that merely ends in "project" is not the keyword.
    CHECK(PaneCatalogProjectKey(&jira, "myproject = X").empty());
}

TEST_CASE("PaneCatalogProjectKey: no resolved view or no backend is unscoped without parsing anything") {
    const JqlConnectivity jira;
    // No view resolved for the pane: the caller's config carries the FOCUSED view's query, which is
    // another pane's scope, so the derivation never sees it.
    CHECK(PaneCatalogProjectKey(&jira, std::string()).empty());
    CHECK(jira.Calls == 0);
    // The pane's first sync has not created its backend yet.
    CHECK(PaneCatalogProjectKey(nullptr, "project = KEY").empty());
}

TEST_CASE("PaneCatalogProjectKey: a backend with no query parser is never scoped") {
    const NoParserConnectivity fixture;
    CHECK(PaneCatalogProjectKey(&fixture, "project = KEY").empty());
}

TEST_CASE("PaneCatalogProjectKey: the derived key puts the project's own snapshot first in the offline fallback") {
    const JqlConnectivity jira;
    const std::string scoped = PaneCatalogProjectKey(&jira, "project = KEY");
    // The saved-snapshot fallback tries the snapshot built from the same project key first, and only then
    // the unscoped one, which restores as no project's (so it never stands in for a row's project list).
    const std::vector<std::string> keys = smatchet::sync::CatalogSnapshotCandidates(scoped, "jira|KEY", "jira");
    CHECK(keys == std::vector<std::string>{"jira|KEY", "jira"});
    CHECK(smatchet::sync::RestoredSnapshotScope(scoped, 0, keys.size()) == "KEY");
    CHECK(smatchet::sync::RestoredSnapshotScope(scoped, 1, keys.size()).empty());

    const std::string unscoped = PaneCatalogProjectKey(&jira, "project in (A, B)");
    CHECK(smatchet::sync::CatalogSnapshotCandidates(unscoped, "jira|", "jira") == std::vector<std::string>{"jira"});
}
