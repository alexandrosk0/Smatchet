// TEST_COVERAGE_GAP_MAP.md Tier 2 (coverage-gap-tier2-backend-shell-fixtures.md Slice 2) —
// fixture coverage for the `GitHubClient.cpp` shell methods that carry an explicit
// TrackerConfig (probe, comment post, the streamed-fetch shim, per-key fetch). A real
// `GitHubClient` is driven at an in-process httplib loopback over real cpr HTTP. The
// cfg-less MUTATION paths (FetchIssueComments / CreateIssue / UpdateField) live in
// GitHubIssueMutationHttp.test.cpp; the one cfg-less READ path covered here
// (ListProjects) runs under TestEnvGuard so the rig never touches the container's
// real config file.
// Characterization tests — they pin CURRENT behaviour of the shipped shell.

#include "GitHubClient.h"
#include "HttpRequestCapture.h"
#include "JiraCatalogHttpFixture.h"
#include "TestEnvGuard.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet_tests::HttpRequestCapture;

namespace {

// A TrackerConfig pointed at the loopback. Per issue #979 the live cfg wins over the
// ctor snapshot, so tests construct the client with a bogus ctor base URL to prove the
// cfg-resolution path is the one hitting the wire.
TrackerConfig GitHubConfig(const JiraCatalogHttpFixture& fx) {
    TrackerConfig cfg;
    cfg.TrackerType = "github";
    cfg.GitHubBaseUrl = "http://127.0.0.1:" + std::to_string(fx.Port());
    cfg.GitHubPat = "live-pat";
    cfg.GitHubOwner = "o";
    cfg.GitHubRepo = "r";
    return cfg;
}

nlohmann::json SinglePageSearch() {
    return nlohmann::json{
        {"data",
         {{"search",
           {{"issueCount", 1},
            {"nodes",
             nlohmann::json::array({nlohmann::json{{"__typename", "Issue"},
                                                   {"number", 5},
                                                   {"title", "five"},
                                                   {"state", "OPEN"},
                                                   {"repository", {{"name", "r"}, {"owner", {{"login", "o"}}}}}}})},
            {"pageInfo", {{"hasNextPage", false}, {"endCursor", ""}}}}}}}};
}

} // namespace

TEST_CASE("GitHub probe — /rate_limit status → reachability-kind classification matrix") {
    JiraCatalogHttpFixture fx;
    GitHubClient client("http://bogus.invalid", "ctor-pat");
    TrackerConfig cfg = GitHubConfig(fx);

    SUBCASE("missing live PAT fails fast without HTTP (no ctor-snapshot fallback — #979)") {
        cfg.GitHubPat.clear();
        const TrackerReachabilityProbeResult r = client.ProbeReachability(cfg);
        CHECK(r.Kind == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
        CHECK(r.Diagnostic.find("PAT not configured") != std::string::npos);
        CHECK(fx.RequestCount("/rate_limit") == 0);
    }
    SUBCASE("200 is AuthenticatedReachable and surfaces the core quota") {
        fx.ScriptJson("/rate_limit", nlohmann::json{{"resources", {{"core", {{"limit", 5000}, {"remaining", 4990}}}}}});
        const TrackerReachabilityProbeResult r = client.ProbeReachability(cfg);
        CHECK(r.Kind == TrackerReachabilityProbeKind::AuthenticatedReachable);
        CHECK(r.Diagnostic == "HTTP 200 (core 4990/5000)");
    }
    SUBCASE("401 is auth with the expired-PAT hint") {
        fx.ScriptStatus("/rate_limit", 401);
        const TrackerReachabilityProbeResult r = client.ProbeReachability(cfg);
        CHECK(r.Kind == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
        CHECK(r.Diagnostic.find("invalid or expired PAT") != std::string::npos);
    }
    SUBCASE("403 is auth with the scope / secondary-rate-limit hint") {
        fx.ScriptStatus("/rate_limit", 403);
        const TrackerReachabilityProbeResult r = client.ProbeReachability(cfg);
        CHECK(r.Kind == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
        CHECK(r.Diagnostic.find("secondary rate-limit") != std::string::npos);
    }
    SUBCASE("404 is a base-URL config error, not TransportDown") {
        fx.ScriptStatus("/rate_limit", 404);
        const TrackerReachabilityProbeResult r = client.ProbeReachability(cfg);
        CHECK(r.Kind == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
        CHECK(r.Diagnostic.find("check Base URL") != std::string::npos);
    }
}

TEST_CASE("GitHub comment post — wire shape and error taxonomy") {
    JiraCatalogHttpFixture fx;
    GitHubClient client("http://bogus.invalid", "ctor-pat");
    const TrackerConfig cfg = GitHubConfig(fx);
    const std::string commentsPath = "/repos/o/r/issues/7/comments";

    SUBCASE("2xx posts {\"body\": text} to /repos/o/r/issues/N/comments") {
        HttpRequestCapture capture;
        fx.ScriptHandler(
            commentsPath,
            [&capture](const httplib::Request& req) {
                capture.Push(req.body);
                return nlohmann::json{{"id", 1}};
            },
            "POST");
        const TrackerError err = client.AddIssueCommentPlain(cfg, "o/r#7", "hello from the fixture");
        CHECK(err.IsOk());
        const std::vector<std::string> bodies = capture.Snapshot();
        REQUIRE(bodies.size() == 1);
        const nlohmann::json body = nlohmann::json::parse(bodies[0]);
        CHECK(body["body"].get<std::string>() == "hello from the fixture");
    }
    SUBCASE("malformed issue key is InvalidRequest with zero HTTP calls") {
        const TrackerError err = client.AddIssueCommentPlain(cfg, "TICKET-7", "text");
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail.find("owner/repo#N") != std::string::npos);
        CHECK(fx.RequestCount(commentsPath) == 0);
    }
    SUBCASE("cleared live PAT is Auth even though the ctor snapshot had one (#979)") {
        TrackerConfig noPat = cfg;
        noPat.GitHubPat.clear();
        const TrackerError err = client.AddIssueCommentPlain(noPat, "o/r#7", "text");
        CHECK(err.Kind == TrackerErrorKind::Auth);
        CHECK(fx.RequestCount(commentsPath) == 0);
    }
    SUBCASE("422 maps to InvalidRequest carrying the extracted GitHub message") {
        fx.ScriptRaw(commentsPath, 422, R"({"message":"Validation Failed"})", "application/json", "POST");
        const TrackerError err = client.AddIssueCommentPlain(cfg, "o/r#7", "text");
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail == "Validation Failed");
    }
}

TEST_CASE("GitHub streamed fetch — the class shim wires pages, summary, and live-cfg auth") {
    JiraCatalogHttpFixture fx;
    // Bogus ctor base URL: if the shim resolved auth from the ctor snapshot instead of
    // the live cfg, the request would never reach the loopback and this test would fail.
    GitHubClient client("http://bogus.invalid", "ctor-pat");
    const TrackerConfig cfg = GitHubConfig(fx);
    fx.ScriptJson("/graphql", SinglePageSearch(), "POST");

    std::vector<size_t> batchSizes;
    const TrackerIssueFetchSummary summary = client.FetchIssuesStreamed(
        [&batchSizes](std::vector<CachedTicket>&& batch) { batchSizes.push_back(batch.size()); },
        []() { return false; }, &cfg, nullptr);

    CHECK(summary.FetchError.empty());
    CHECK(summary.FullSyncCompleted);
    CHECK(summary.FetchedCount == 1);
    REQUIRE(batchSizes.size() == 1);
    CHECK(batchSizes[0] == 1);
    CHECK(fx.RequestCount("/graphql") == 1);
}

TEST_CASE("GitHub per-key fetch — the class shim resolves live-cfg credentials") {
    JiraCatalogHttpFixture fx;
    GitHubClient client("http://bogus.invalid", "ctor-pat");
    const ViewsStore views;

    SUBCASE("cleared live PAT is Auth (no ctor fallback)") {
        TrackerConfig cfg = GitHubConfig(fx);
        cfg.GitHubPat.clear();
        const auto r = client.FetchIssuesForKeys(cfg, {"o/r#1"}, views);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().Kind == TrackerErrorKind::Auth);
    }
    SUBCASE("good key round-trips through the loopback") {
        fx.ScriptJson("/repos/o/r/issues/4", nlohmann::json{{"number", 4}, {"title", "four"}, {"state", "open"}});
        const auto r = client.FetchIssuesForKeys(GitHubConfig(fx), {"o/r#4"}, views);
        REQUIRE(r.has_value());
        REQUIRE(r.value().size() == 1);
        CHECK(r.value()[0].id == "o/r#4");
    }
}

TEST_CASE("GitHub ListProjects — paginated /user/repos → owner/repo picker rows") {
    smatchet_tests::TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    {
        // ListProjects is cfg-less (interface has no cfg parameter) — persist the
        // loopback config into the guard's private dir so ResolveAuth finds it.
        TrackerConfig cfg = ConfigManager::Load();
        cfg.TrackerType = "github";
        cfg.GitHubBaseUrl = "http://127.0.0.1:" + std::to_string(fx.Port());
        cfg.GitHubPat = "loopback-pat";
        ConfigManager::Save(cfg);
    }
    GitHubClient client("http://bogus.invalid", "ctor-pat");

    SUBCASE("short page ends the walk; rows carry full_name as key and displayName") {
        fx.ScriptJson("/user/repos", nlohmann::json::array({nlohmann::json{{"id", 42}, {"full_name", "acme/widgets"}},
                                                            nlohmann::json{{"id", 7}, {"full_name", "acme/tools"}}}));
        const std::vector<RemoteProject> projects = client.ListProjects();
        REQUIRE(projects.size() == 2);
        CHECK(projects[0].id == "42");
        CHECK(projects[0].key == "acme/widgets"); // feeds IssueDraft::ProjectKey
        CHECK(projects[1].displayName == "acme/tools");
        CHECK(fx.RequestCount("/user/repos") == 1); // 2 < per_page → no second page requested
    }
    SUBCASE("an HTTP error degrades to an empty list (picker falls back to configured repo)") {
        fx.ScriptStatus("/user/repos", 401);
        CHECK(client.ListProjects().empty());
    }
    SUBCASE("a cleared PAT fails fast without HTTP") {
        TrackerConfig cfg = ConfigManager::Load();
        cfg.GitHubPat.clear();
        ConfigManager::Save(cfg);
        CHECK(client.ListProjects().empty());
        CHECK(fx.RequestCount("/user/repos") == 0);
    }
}
