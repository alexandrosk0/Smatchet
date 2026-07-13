// github-tracker-backend.md UpdateIssueFields slice — fixture coverage for the GitHub WRITE shell:
// UpdateIssueFields / UpdateField (PATCH + the label set-replace reconcile) driven at
// the in-process httplib loopback over real cpr HTTP, plus the BuildFieldPayload
// catalog-id keying. The cfg-less mutation paths load ConfigManager::Load() per
// request, so cases run under TestEnvGuard with the loopback config saved into the
// guard's private dir (JiraIssueMutationHttp / PlaneIssueMutationHttp precedent).

#include "GitHubClient.h"
#include "HttpRequestCapture.h"
#include "JiraCatalogHttpFixture.h"
#include "TestEnvGuard.h"
#include "TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet_tests::HttpRequestCapture;

namespace {

const std::string kIssuePath = "/repos/o/r/issues/7";
const std::string kLabelsPath = "/repos/o/r/issues/7/labels";
const std::string kMilestonesPath = "/repos/o/r/milestones";

// The cfg-less mutation paths read ConfigManager::Load() per request — persist the
// loopback config into the TestEnvGuard-redirected dir so they resolve to the fixture.
void SaveLoopbackGitHubConfig(const JiraCatalogHttpFixture& fx) {
    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = "github";
    cfg.GitHubBaseUrl = "http://127.0.0.1:" + std::to_string(fx.Port());
    cfg.GitHubPat = "loopback-pat";
    cfg.GitHubOwner = "o";
    cfg.GitHubRepo = "r";
    ConfigManager::Save(cfg);
}

TrackerField FieldWithId(const char* id) {
    TrackerField f;
    f.Id = id;
    return f;
}

nlohmann::json IssueWithLabels(const std::vector<std::string>& names) {
    nlohmann::json labels = nlohmann::json::array();
    for (const auto& n : names) {
        labels.push_back(nlohmann::json{{"name", n}});
    }
    return nlohmann::json{{"number", 7}, {"labels", labels}};
}

} // namespace

TEST_CASE("GitHub UpdateIssueFields — PATCH wire shape carries REST names, not catalog ids") {
    smatchet_tests::TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    SaveLoopbackGitHubConfig(fx);
    GitHubClient client("http://bogus.invalid", "ctor-pat");

    HttpRequestCapture capture;
    fx.ScriptHandler(
        kIssuePath,
        [&capture](const httplib::Request& req) {
            capture.Push(req.body);
            return nlohmann::json{{"number", 7}};
        },
        "PATCH");
    const TrackerError err = client.UpdateIssueFields(
        "o/r#7", nlohmann::json{{"summary", "[PR] New title"}, {"status", "Closed"}, {"description", "b"}});
    CHECK(err.IsOk());
    const std::vector<std::string> bodies = capture.Snapshot();
    REQUIRE(bodies.size() == 1);
    const nlohmann::json body = nlohmann::json::parse(bodies[0]);
    CHECK(body["title"].get<std::string>() == "New title"); // [PR] display prefix stripped
    CHECK(body["state"].get<std::string>() == "closed");
    CHECK(body["body"].get<std::string>() == "b");
    CHECK(body.find("summary") == body.end());
    CHECK(body.find("status") == body.end());
}

TEST_CASE("GitHub UpdateField labels — pre-fetch + diff issues one batch POST and per-name DELETE") {
    smatchet_tests::TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    SaveLoopbackGitHubConfig(fx);
    GitHubClient client("http://bogus.invalid", "ctor-pat");

    SUBCASE("adds the missing labels, removes the stale one, leaves the overlap alone") {
        fx.ScriptJson(kIssuePath, IssueWithLabels({"bug", "stale"}));
        HttpRequestCapture capture;
        fx.ScriptHandler(
            kLabelsPath,
            [&capture](const httplib::Request& req) {
                capture.Push(req.body);
                return nlohmann::json::array();
            },
            "POST");
        fx.ScriptJson(kLabelsPath + "/stale", nlohmann::json::array(), "DELETE");

        const TrackerError err = client.UpdateField("o/r#7", FieldWithId("labels"), {"bug, p0"});
        CHECK(err.IsOk());
        const std::vector<std::string> bodies = capture.Snapshot();
        REQUIRE(bodies.size() == 1);
        CHECK(nlohmann::json::parse(bodies[0]) == nlohmann::json{{"labels", nlohmann::json::array({"p0"})}});
        CHECK(fx.RequestCount(kLabelsPath + "/stale") == 1);
        CHECK(fx.RequestCount(kIssuePath) == 1); // the pre-fetch GET; no PATCH for a labels-only edit
    }
    SUBCASE("a stale label already removed server-side (DELETE 404) still converges") {
        fx.ScriptJson(kIssuePath, IssueWithLabels({"bug", "stale"}));
        fx.ScriptStatus(kLabelsPath + "/stale", 404, "DELETE");
        const TrackerError err = client.UpdateField("o/r#7", FieldWithId("labels"), {"bug"});
        CHECK(err.IsOk());
        CHECK(fx.RequestCount(kLabelsPath + "/stale") == 1);
    }
    SUBCASE("a no-op edit (current == intended) makes zero write calls") {
        fx.ScriptJson(kIssuePath, IssueWithLabels({"bug", "p0"}));
        const TrackerError err = client.UpdateField("o/r#7", FieldWithId("labels"), {"p0, bug"});
        CHECK(err.IsOk());
        CHECK(fx.RequestCount(kLabelsPath) == 0);
    }
    SUBCASE("label pre-fetch failure classifies from the GET status") {
        fx.ScriptStatus(kIssuePath, 503);
        const TrackerError err = client.UpdateField("o/r#7", FieldWithId("labels"), {"bug"});
        CHECK_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::ServerError);
        CHECK(err.IsRetryable());
    }
}

TEST_CASE("GitHub UpdateIssueFields — milestone title resolves to its number before the PATCH") {
    smatchet_tests::TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    SaveLoopbackGitHubConfig(fx);
    GitHubClient client("http://bogus.invalid", "ctor-pat");

    SUBCASE("resolved number lands in the PATCH body") {
        fx.ScriptJson(kMilestonesPath, nlohmann::json::array({nlohmann::json{{"number", 3}, {"title", "v1.0"}}}));
        HttpRequestCapture capture;
        fx.ScriptHandler(
            kIssuePath,
            [&capture](const httplib::Request& req) {
                capture.Push(req.body);
                return nlohmann::json{{"number", 7}};
            },
            "PATCH");
        const TrackerError err = client.UpdateIssueFields("o/r#7", nlohmann::json{{"milestone", "v1.0"}});
        CHECK(err.IsOk());
        const std::vector<std::string> bodies = capture.Snapshot();
        REQUIRE(bodies.size() == 1);
        CHECK(nlohmann::json::parse(bodies[0]) == nlohmann::json{{"milestone", 3}});
    }
    SUBCASE("an unknown title is InvalidRequest and the PATCH never fires") {
        fx.ScriptJson(kMilestonesPath, nlohmann::json::array());
        const TrackerError err = client.UpdateIssueFields("o/r#7", nlohmann::json{{"milestone", "v9.9"}});
        CHECK_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail.find("v9.9") != std::string::npos);
        CHECK(fx.RequestCount(kIssuePath) == 0);
    }
}

TEST_CASE("GitHub UpdateIssueFields — failure statuses classify into the TrackerError kind") {
    smatchet_tests::TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    SaveLoopbackGitHubConfig(fx);
    GitHubClient client("http://bogus.invalid", "ctor-pat");
    const nlohmann::json fields = nlohmann::json{{"summary", "t"}};

    SUBCASE("HTTP 400 is InvalidRequest — non-retryable, never offline-queued") {
        fx.ScriptStatus(kIssuePath, 400, "PATCH");
        const TrackerError err = client.UpdateIssueFields("o/r#7", fields);
        CHECK_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK_FALSE(err.IsRetryable());
    }
    SUBCASE("HTTP 503 is ServerError — retryable, so the offline-queue fallback stays reachable") {
        fx.ScriptStatus(kIssuePath, 503, "PATCH");
        const TrackerError err = client.UpdateIssueFields("o/r#7", fields);
        CHECK_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::ServerError);
        CHECK(err.IsRetryable());
    }
    SUBCASE("commit keys are immutable — rejected with zero HTTP calls") {
        const TrackerError err = client.UpdateIssueFields("o/r@a1b2c3d", fields);
        CHECK_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail.find("immutable") != std::string::npos);
        CHECK(fx.RequestCount(kIssuePath) == 0);
    }
    SUBCASE("malformed issue key is InvalidRequest with zero HTTP calls") {
        const TrackerError err = client.UpdateIssueFields("TICKET-7", fields);
        CHECK_FALSE(err.IsOk());
        CHECK(err.Detail.find("owner/repo#N") != std::string::npos);
        CHECK(fx.RequestCount(kIssuePath) == 0);
    }
}

TEST_CASE("GitHub BuildFieldPayload — catalog-id-keyed payloads for the edit pipeline") {
    GitHubClient client("http://bogus.invalid", "ctor-pat");

    SUBCASE("labels / assignee split the comma-joined grid cell into a string array") {
        const auto r = client.BuildFieldPayload(FieldWithId("labels"), {"bug, p0"});
        REQUIRE(r);
        CHECK(r.value() == nlohmann::json{{"labels", nlohmann::json::array({"bug", "p0"})}});
        const auto a = client.BuildFieldPayload(FieldWithId("assignee"), {"alice"});
        REQUIRE(a);
        CHECK(a.value() == nlohmann::json{{"assignee", nlohmann::json::array({"alice"})}});
    }
    SUBCASE("scalar ids key the first value; empty values clear") {
        const auto r = client.BuildFieldPayload(FieldWithId("milestone"), {});
        REQUIRE(r);
        CHECK(r.value() == nlohmann::json{{"milestone", ""}});
    }
    SUBCASE("read-only ids are rejected before any audit/HTTP work") {
        const auto r = client.BuildFieldPayload(FieldWithId("comments"), {"3"});
        REQUIRE_FALSE(r);
        CHECK(r.error().Kind == TrackerErrorKind::InvalidRequest);
    }
}
