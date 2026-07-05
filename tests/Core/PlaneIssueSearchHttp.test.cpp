// TEST_COVERAGE_GAP_MAP.md Tier 2 (coverage-gap-tier2-backend-shell-fixtures.md Slice 1) —
// fixture coverage for the audit-flagged `PlaneIssueSearch.cpp` HTTP shell. A real
// `PlaneClient` is driven at an in-process httplib loopback over real cpr HTTP (same
// pattern as `TrackerCatalogBuild.test.cpp` for Jira), pinning the orchestration the
// pure-mapper suites cannot see: cursor pagination + the bounded-loop page cap, response
// classification (non-200 / HTML / invalid JSON / empty body / missing results array),
// project-resolve failure surfaces, the structured-query key filter, streamed-batch
// cancellation, and the `ProbeReachability` status→kind matrix. These are
// characterization tests — they pin CURRENT behaviour of the shipped shell.

#include "JiraCatalogHttpFixture.h"
#include "PlaneClient.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* kProjectUuid = "11111111-aaaa-bbbb-cccc-000000000001";

// A TrackerConfig pointed at the loopback with the Plane fields populated. The
// structured query carries the project UUID so ResolvePlaneOperationProject never
// falls back to ListProjects() (which reads ConfigManager::Load()).
TrackerConfig PlaneConfig(const JiraCatalogHttpFixture& fx, const std::string& jqlQuery = std::string()) {
    TrackerConfig cfg;
    cfg.TrackerType = "Plane";
    cfg.PlaneUrl = "http://127.0.0.1:" + std::to_string(fx.Port());
    cfg.PlaneWorkspaceSlug = "ws";
    cfg.PlaneApiKey = "fixture-key";
    cfg.JqlQuery = jqlQuery.empty() ? std::string(R"({"project_id":")") + kProjectUuid + "\"}" : jqlQuery;
    return cfg;
}

nlohmann::json ProjectsBody() {
    return nlohmann::json{{"results", {{{"id", kProjectUuid}, {"identifier", "SMT"}, {"name", "Smatchet"}}}}};
}

nlohmann::json StatesBody() { return nlohmann::json{{"results", {{{"id", "state-1"}, {"name", "Backlog"}}}}}; }

// One minimal work-item row the pure mapper accepts (visual key = SMT-<sequence_id>).
nlohmann::json WorkItem(int sequenceId, const std::string& name) {
    return nlohmann::json{{"id", std::string("uuid-") + std::to_string(sequenceId)},
                          {"sequence_id", sequenceId},
                          {"name", name},
                          {"state_detail", {{"id", "state-1"}, {"name", "Backlog"}}}};
}

const std::string kProjectsPath = "/api/v1/workspaces/ws/projects/";
const std::string kStatesPath = std::string("/api/v1/workspaces/ws/projects/") + kProjectUuid + "/states/";
const std::string kItemsPath = std::string("/api/v1/workspaces/ws/projects/") + kProjectUuid + "/work-items/";

// Script the happy-path resolve + states routes shared by most cases.
void ScriptResolveAndStates(JiraCatalogHttpFixture& fx) {
    fx.ScriptJson(kProjectsPath, ProjectsBody());
    fx.ScriptJson(kStatesPath, StatesBody());
}

} // namespace

TEST_CASE("Plane fetch — two-page cursor pagination streams batches and completes the sync") {
    JiraCatalogHttpFixture fx;
    ScriptResolveAndStates(fx);
    fx.ScriptHandler(kItemsPath, [](const httplib::Request& req) {
        const std::string cursor = req.get_param_value("cursor");
        if (cursor.empty()) {
            return nlohmann::json{{"results", {WorkItem(1, "first"), WorkItem(2, "second")}},
                                  {"next_page_results", true},
                                  {"next_cursor", "cursor-page-2"}};
        }
        // Serve page 2 only for the advertised cursor; anything else falls out as an
        // empty page (assertions stay on the test thread — doctest is not thread-safe).
        if (cursor != "cursor-page-2") {
            return nlohmann::json{{"results", nlohmann::json::array()}, {"next_page_results", false}};
        }
        return nlohmann::json{{"results", {WorkItem(3, "third")}}, {"next_page_results", false}, {"next_cursor", ""}};
    });

    PlaneClient client;
    const TrackerConfig cfg = PlaneConfig(fx);
    bool fullSync = false;
    std::string fetchError;
    std::string warning;
    const std::vector<CachedTicket> tickets = client.FetchIssues(&fullSync, &cfg, nullptr, &fetchError, &warning);

    CHECK(fetchError.empty());
    CHECK(warning.empty());
    CHECK(fullSync);
    REQUIRE(tickets.size() == 3);
    CHECK(tickets[0].id == "SMT-1");
    CHECK(tickets[1].id == "SMT-2");
    CHECK(tickets[2].id == "SMT-3");
    CHECK(fx.RequestCount(kItemsPath) == 2);
    CHECK(fx.RequestCount(kProjectsPath) == 1);
    CHECK(fx.RequestCount(kStatesPath) == 1);
}

TEST_CASE("Plane fetch — self-referential cursor is bounded by the 50-page cap (soft warning)") {
    JiraCatalogHttpFixture fx;
    ScriptResolveAndStates(fx);
    // A misbehaving server that always advertises the same next cursor. Without the
    // outer page cap this loop never terminates — the bounded-loop guard the audit
    // class calls for.
    fx.ScriptHandler(kItemsPath, [](const httplib::Request&) {
        return nlohmann::json{
            {"results", {WorkItem(1, "loop")}}, {"next_page_results", true}, {"next_cursor", "same-cursor"}};
    });

    PlaneClient client;
    const TrackerConfig cfg = PlaneConfig(fx);
    bool fullSync = true;
    std::string fetchError;
    std::string warning;
    const std::vector<CachedTicket> tickets = client.FetchIssues(&fullSync, &cfg, nullptr, &fetchError, &warning);

    CHECK(fetchError.empty()); // partial success, not a fetch failure
    CHECK(warning.find("outer page cap (50)") != std::string::npos);
    CHECK_FALSE(fullSync);
    CHECK(tickets.size() == 50); // one ticket per page, capped
    CHECK(fx.RequestCount(kItemsPath) == 50);
}

TEST_CASE("Plane fetch — project-resolve failures surface before any work-items request") {
    JiraCatalogHttpFixture fx;
    PlaneClient client;
    const TrackerConfig cfg = PlaneConfig(fx);
    std::string fetchError;

    SUBCASE("project missing from the workspace list names the available identifiers") {
        fx.ScriptJson(kProjectsPath,
                      nlohmann::json{{"results", {{{"id", "other-uuid"}, {"identifier", "OTH"}, {"name", "Other"}}}}});
        client.FetchIssues(nullptr, &cfg, nullptr, &fetchError, nullptr);
        CHECK(fetchError.find("was not found in workspace 'ws'") != std::string::npos);
        CHECK(fetchError.find("OTH") != std::string::npos);
    }
    SUBCASE("non-200 on the project list is a resolve error") {
        // 403 (not 5xx) so TrackerGetLogged's retry engine doesn't back off — the
        // retry semantics themselves are pinned by TrackerHttpRetry/Faults.test.cpp.
        fx.ScriptStatus(kProjectsPath, 403);
        client.FetchIssues(nullptr, &cfg, nullptr, &fetchError, nullptr);
        CHECK(fetchError.find("Plane API error 403 resolving project") != std::string::npos);
    }
    CHECK(fx.RequestCount(kItemsPath) == 0);
}

TEST_CASE("Plane fetch — work-items response classification") {
    JiraCatalogHttpFixture fx;
    ScriptResolveAndStates(fx);
    PlaneClient client;
    const TrackerConfig cfg = PlaneConfig(fx);
    bool fullSync = true;
    std::string fetchError;

    SUBCASE("non-200 extracts the JSON detail and appends the 404 config hint") {
        fx.ScriptRaw(kItemsPath, 404, R"({"detail":"Project not found."})", "application/json");
        client.FetchIssues(&fullSync, &cfg, nullptr, &fetchError, nullptr);
        CHECK(fetchError.find("Plane API error 404 fetching issues") != std::string::npos);
        CHECK(fetchError.find("[detail: Project not found.]") != std::string::npos);
        CHECK(fetchError.find("Check Workspace Slug and Project ID") != std::string::npos);
    }
    SUBCASE("HTML body on HTTP 200 is rejected with the api-origin guidance") {
        fx.ScriptRaw(kItemsPath, 200, "<!DOCTYPE html><html><body>login</body></html>", "text/html");
        client.FetchIssues(&fullSync, &cfg, nullptr, &fetchError, nullptr);
        CHECK(fetchError.find("Plane returned HTML instead of JSON (HTTP 200)") != std::string::npos);
    }
    SUBCASE("empty body on HTTP 200 is rejected") {
        fx.ScriptRaw(kItemsPath, 200, "", "application/json");
        client.FetchIssues(&fullSync, &cfg, nullptr, &fetchError, nullptr);
        CHECK(fetchError.find("empty response body") != std::string::npos);
    }
    SUBCASE("invalid JSON on HTTP 200 is rejected") {
        fx.ScriptRaw(kItemsPath, 200, "{\"results\": [truncated", "application/json");
        client.FetchIssues(&fullSync, &cfg, nullptr, &fetchError, nullptr);
        CHECK(fetchError.find("invalid JSON") != std::string::npos);
    }
    SUBCASE("object without a results array reports the top-level keys") {
        fx.ScriptJson(kItemsPath, nlohmann::json{{"count", 3}, {"detail", "wrong endpoint"}});
        client.FetchIssues(&fullSync, &cfg, nullptr, &fetchError, nullptr);
        CHECK(fetchError.find("no results array") != std::string::npos);
        CHECK(fetchError.find("count") != std::string::npos);
    }
    CHECK_FALSE(fullSync);
    CHECK_FALSE(fetchError.empty());
}

TEST_CASE("Plane fetch — structured-query key filter narrows the stream client-side") {
    JiraCatalogHttpFixture fx;
    ScriptResolveAndStates(fx);
    fx.ScriptJson(kItemsPath, nlohmann::json{{"results", {WorkItem(1, "first"), WorkItem(2, "second")}},
                                             {"next_page_results", false},
                                             {"next_cursor", ""}});

    PlaneClient client;
    const TrackerConfig cfg = PlaneConfig(fx, std::string(R"({"project_id":")") + kProjectUuid + R"(","key":"SMT-2"})");
    std::string fetchError;
    const std::vector<CachedTicket> tickets = client.FetchIssues(nullptr, &cfg, nullptr, &fetchError, nullptr);

    CHECK(fetchError.empty());
    REQUIRE(tickets.size() == 1);
    CHECK(tickets[0].id == "SMT-2");
}

TEST_CASE("Plane fetch — FetchIssuesForKeys cancels pagination once every key is found") {
    JiraCatalogHttpFixture fx;
    ScriptResolveAndStates(fx);
    fx.ScriptHandler(kItemsPath, [](const httplib::Request& req) {
        const std::string cursor = req.get_param_value("cursor");
        if (cursor.empty()) {
            return nlohmann::json{{"results", {WorkItem(1, "first"), WorkItem(2, "second")}},
                                  {"next_page_results", true},
                                  {"next_cursor", "cursor-page-2"}};
        }
        return nlohmann::json{{"results", {WorkItem(3, "third")}}, {"next_page_results", false}, {"next_cursor", ""}};
    });

    PlaneClient client;
    const TrackerConfig cfg = PlaneConfig(fx);
    const ViewsStore views;
    const auto result = client.FetchIssuesForKeys(cfg, {"SMT-1"}, views);

    REQUIRE(result.has_value());
    REQUIRE(result.value().size() == 1);
    CHECK(result.value()[0].id == "SMT-1");
    // The wanted key lives on page 1; the early-exit cancel must stop before page 2.
    CHECK(fx.RequestCount(kItemsPath) == 1);
}

TEST_CASE("Plane probe — status → reachability-kind classification matrix") {
    JiraCatalogHttpFixture fx;
    PlaneClient client;
    TrackerConfig cfg = PlaneConfig(fx);
    const std::string mePath = "/api/v1/users/me/";

    SUBCASE("200 is AuthenticatedReachable") {
        fx.ScriptJson(mePath, nlohmann::json{{"id", "me"}});
        const TrackerReachabilityProbeResult r = client.ProbeReachability(cfg);
        CHECK(r.Kind == TrackerReachabilityProbeKind::AuthenticatedReachable);
        CHECK(r.Diagnostic == "HTTP 200");
    }
    SUBCASE("401 is a reachable auth/config error") {
        fx.ScriptStatus(mePath, 401);
        const TrackerReachabilityProbeResult r = client.ProbeReachability(cfg);
        CHECK(r.Kind == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
        CHECK(r.Diagnostic.find("401") != std::string::npos);
    }
    SUBCASE("404 from a stale base URL is auth/config, NOT TransportDown (the §2.1 P1 fix)") {
        fx.ScriptStatus(mePath, 404);
        const TrackerReachabilityProbeResult r = client.ProbeReachability(cfg);
        CHECK(r.Kind == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
    }
    SUBCASE("empty URL/key is auth/config without any HTTP") {
        cfg.PlaneApiKey.clear();
        const TrackerReachabilityProbeResult r = client.ProbeReachability(cfg);
        CHECK(r.Kind == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
        CHECK(fx.RequestCount(mePath) == 0);
    }
}
