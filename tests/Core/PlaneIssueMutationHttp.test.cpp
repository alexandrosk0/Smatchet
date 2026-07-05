// TEST_COVERAGE_GAP_MAP.md Tier 2 (coverage-gap-tier2-backend-shell-fixtures.md Slice 3) —
// fixture coverage for the `PlaneIssueMutation.cpp` write shell (the audit's data-loss
// class: field PATCH, issue POST, comment POST). A real `PlaneClient` is driven at an
// in-process httplib loopback over real cpr HTTP. The cfg-less paths (UpdateIssueFields /
// CreateIssue / FetchIssueComments) resolve from on-disk ConfigManager, so those cases
// run under TestEnvGuard with the loopback config saved to the guard's private dir.
// Characterization tests — they pin CURRENT behaviour of the shipped shell.

#include "HttpRequestCapture.h"
#include "JiraCatalogHttpFixture.h"
#include "PlaneClient.h"
#include "TestEnvGuard.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet_tests::HttpRequestCapture;

namespace {

constexpr const char* kProjectUuid = "11111111-aaaa-bbbb-cccc-000000000001";
constexpr const char* kIssueUuid = "22222222-aaaa-bbbb-cccc-000000000002";

const std::string kProjectsPath = "/api/v1/workspaces/ws/projects/";
const std::string kItemsPath = std::string("/api/v1/workspaces/ws/projects/") + kProjectUuid + "/work-items/";
const std::string kItemPath = kItemsPath + kIssueUuid + "/";
const std::string kCommentsPath = kItemsPath + kIssueUuid + "/comments/";

TrackerConfig LoopbackPlaneConfig(const JiraCatalogHttpFixture& fx) {
    TrackerConfig cfg;
    cfg.TrackerType = "Plane";
    cfg.PlaneUrl = "http://127.0.0.1:" + std::to_string(fx.Port());
    cfg.PlaneWorkspaceSlug = "ws";
    cfg.PlaneApiKey = "fixture-key";
    cfg.JqlQuery = std::string(R"({"project_id":")") + kProjectUuid + "\"}";
    return cfg;
}

// The cfg-less mutation paths read ConfigManager::Load() per request — persist the
// loopback config into the TestEnvGuard-redirected dir so they resolve to the fixture.
void SaveLoopbackPlaneConfig(const JiraCatalogHttpFixture& fx) {
    TrackerConfig cfg = ConfigManager::Load();
    const TrackerConfig loopback = LoopbackPlaneConfig(fx);
    cfg.TrackerType = loopback.TrackerType;
    cfg.PlaneUrl = loopback.PlaneUrl;
    cfg.PlaneWorkspaceSlug = loopback.PlaneWorkspaceSlug;
    cfg.PlaneApiKey = loopback.PlaneApiKey;
    cfg.JqlQuery = loopback.JqlQuery;
    ConfigManager::Save(cfg);
}

void ScriptProjectResolve(JiraCatalogHttpFixture& fx) {
    fx.ScriptJson(kProjectsPath,
                  nlohmann::json{{"results", {{{"id", kProjectUuid}, {"identifier", "SMT"}, {"name", "Smatchet"}}}}});
}

} // namespace

TEST_CASE("Plane update — PATCH wire shape, visual-key cache, and error taxonomy") {
    JiraCatalogHttpFixture fx;
    smatchet_tests::TestEnvGuard env;
    SaveLoopbackPlaneConfig(fx);
    ScriptProjectResolve(fx);
    PlaneClient client;

    SUBCASE("UUID issue id PATCHes the work-item with the payload verbatim") {
        HttpRequestCapture capture;
        fx.ScriptHandler(
            kItemPath,
            [&capture](const httplib::Request& req) {
                capture.Push(req.body);
                return nlohmann::json{{"id", kIssueUuid}};
            },
            "PATCH");
        const nlohmann::json fields = {{"name", "renamed"}, {"priority", "urgent"}};
        const TrackerError err = client.UpdateIssueFields(kIssueUuid, fields);
        CHECK(err.IsOk());
        const std::vector<std::string> bodies = capture.Snapshot();
        REQUIRE(bodies.size() == 1);
        CHECK(nlohmann::json::parse(bodies[0]) == fields);
    }
    SUBCASE("visual key not in the key→UUID cache is InvalidRequest with the refresh hint") {
        const TrackerError err = client.UpdateIssueFields("SMT-7", nlohmann::json{{"name", "x"}});
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail.find("Could not resolve Plane visual key 'SMT-7'") != std::string::npos);
        CHECK(fx.RequestCount(kItemPath) == 0);
    }
    SUBCASE("non-2xx PATCH surfaces the Plane error body and maps the status kind") {
        fx.ScriptRaw(kItemPath, 400, R"({"error":"state is not a valid field"})", "application/json", "PATCH");
        const TrackerError err = client.UpdateIssueFields(kIssueUuid, nlohmann::json{{"state", "bogus"}});
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail.find("Plane API error: 400") != std::string::npos);
        CHECK(err.Detail.find("state is not a valid field") != std::string::npos);
    }
}

TEST_CASE("Plane create — visual key derivation, key→UUID cache write, created-key-unknown") {
    JiraCatalogHttpFixture fx;
    smatchet_tests::TestEnvGuard env;
    SaveLoopbackPlaneConfig(fx);
    ScriptProjectResolve(fx);
    PlaneClient client;

    SUBCASE("201 with id + sequence_id returns the SMT-<seq> visual key and warms the cache") {
        fx.ScriptJson(kItemsPath, nlohmann::json{{"id", kIssueUuid}, {"sequence_id", 9}}, "POST");
        const auto created = client.CreateIssue(nlohmann::json{{"name", "new item"}});
        REQUIRE(created.has_value());
        CHECK(created.value() == "SMT-9");

        // The create recorded SMT-9 → UUID, so an immediate visual-key edit resolves
        // without a grid refresh — the exact offline-replay-after-create seam.
        fx.ScriptJson(kItemPath, nlohmann::json{{"id", kIssueUuid}}, "PATCH");
        CHECK(client.UpdateIssueFields("SMT-9", nlohmann::json{{"name", "renamed"}}).IsOk());
        CHECK(fx.RequestCount(kItemPath) == 1);
    }
    SUBCASE("2xx with an unparseable body is Ok(empty) — the created-key-unknown contract") {
        fx.ScriptRaw(kItemsPath, 200, "definitely not json", "text/plain", "POST");
        const auto created = client.CreateIssue(nlohmann::json{{"name", "new item"}});
        REQUIRE(created.has_value());
        CHECK(created.value().empty());
    }
    SUBCASE("non-2xx create surfaces the field-error object and maps the status kind") {
        fx.ScriptRaw(kItemsPath, 400, R"({"name":["This field may not be blank."]})", "application/json", "POST");
        const auto created = client.CreateIssue(nlohmann::json{{"name", ""}});
        REQUIRE_FALSE(created.has_value());
        CHECK(created.error().Kind == TrackerErrorKind::InvalidRequest);
        CHECK(created.error().Detail.find("may not be blank") != std::string::npos);
    }
}

TEST_CASE("Plane comments — post converts markdown to comment_html; fetch maps the envelope") {
    JiraCatalogHttpFixture fx;
    smatchet_tests::TestEnvGuard env;
    SaveLoopbackPlaneConfig(fx);
    ScriptProjectResolve(fx);
    PlaneClient client;
    const TrackerConfig cfg = LoopbackPlaneConfig(fx);

    SUBCASE("AddIssueCommentPlain POSTs comment_html to the work-item comments route") {
        HttpRequestCapture capture;
        fx.ScriptHandler(
            kCommentsPath,
            [&capture](const httplib::Request& req) {
                capture.Push(req.body);
                return nlohmann::json{{"id", "comment-1"}};
            },
            "POST");
        const TrackerError err = client.AddIssueCommentPlain(cfg, kIssueUuid, "some **bold** text");
        CHECK(err.IsOk());
        const std::vector<std::string> bodies = capture.Snapshot();
        REQUIRE(bodies.size() == 1);
        const nlohmann::json body = nlohmann::json::parse(bodies[0]);
        REQUIRE(body.contains("comment_html"));
        // MarkdownConvert renders to Plane's HTML subset — the text and the emphasis
        // must survive; the exact tag shape is the converter's contract, not ours.
        CHECK(body["comment_html"].get<std::string>().find("bold") != std::string::npos);
    }
    SUBCASE("missing config is a fail-fast Auth error with zero HTTP calls") {
        TrackerConfig blank;
        const TrackerError err = client.AddIssueCommentPlain(blank, kIssueUuid, "text");
        CHECK(err.Kind == TrackerErrorKind::Auth);
        CHECK(fx.RequestCount(kCommentsPath) == 0);
    }
    SUBCASE("FetchIssueComments unwraps the results envelope and maps rows") {
        fx.ScriptJson(kCommentsPath, nlohmann::json{{"results",
                                                     {{{"id", "c1"}, {"comment_html", "<p>first</p>"}},
                                                      {{"id", "c2"}, {"comment_html", "<p>second</p>"}}}}});
        const auto comments = client.FetchIssueComments(kIssueUuid);
        REQUIRE(comments.has_value());
        CHECK(comments.value().size() == 2);
    }
    SUBCASE("FetchIssueComments non-200 maps the status kind") {
        fx.ScriptStatus(kCommentsPath, 404);
        const auto comments = client.FetchIssueComments(kIssueUuid);
        REQUIRE_FALSE(comments.has_value());
        CHECK(comments.error().Kind == TrackerErrorKind::NotFound);
    }
}
