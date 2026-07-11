// TEST_COVERAGE_GAP_MAP.md Tier 2 (coverage-gap-tier2-backend-shell-fixtures.md Slice 3) —
// fixture coverage for the `LinearIssueMutation.cpp` write shell: the identifier→UUID
// resolve hop that fronts every write, issueUpdate / issueCreate / commentCreate
// orchestration, and the error taxonomy. A real `LinearClient` is driven at an
// in-process httplib loopback over real cpr HTTP. The cfg-less paths (UpdateIssueFields /
// CreateIssue) resolve from on-disk ConfigManager, so those cases run under TestEnvGuard
// with the loopback config saved to the guard's private dir. Characterization tests —
// they pin CURRENT behaviour of the shipped shell.

#include "HttpRequestCapture.h"
#include "JiraCatalogHttpFixture.h"
#include "LinearClient.h"
#include "TestEnvGuard.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet_tests::HttpRequestCapture;

namespace {

const std::string kGraphQlPath = "/graphql";
constexpr const char* kIssueUuid = "33333333-aaaa-bbbb-cccc-000000000003";

std::string LoopbackGraphQlUrl(const JiraCatalogHttpFixture& fx) {
    return "http://127.0.0.1:" + std::to_string(fx.Port()) + kGraphQlPath;
}

// The cfg-less mutation paths read ConfigManager::Load() per request — persist the
// loopback config into the TestEnvGuard-redirected dir so they resolve to the fixture.
void SaveLoopbackLinearConfig(const JiraCatalogHttpFixture& fx) {
    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = "Linear";
    cfg.LinearBaseUrl = LoopbackGraphQlUrl(fx);
    cfg.LinearApiKey = "fixture-key";
    ConfigManager::Save(cfg);
}

// One handler for the whole GraphQL surface: branch on the posted document so the
// resolve hop and the mutation land distinct scripted responses. Captures every body
// for on-test-thread wire-shape assertions.
struct LinearGraphQlScript {
    HttpRequestCapture Capture;
    nlohmann::json ResolveResponse = nlohmann::json{{"data", {{"issue", {{"id", kIssueUuid}}}}}};
    nlohmann::json MutationResponse;

    void Install(JiraCatalogHttpFixture& fx) {
        fx.ScriptHandler(
            kGraphQlPath,
            [this](const httplib::Request& req) {
                Capture.Push(req.body);
                const nlohmann::json body = nlohmann::json::parse(req.body, nullptr, false);
                const std::string doc = body.is_object() ? body.value("query", std::string()) : std::string();
                if (doc.find("issueUpdate") != std::string::npos || doc.find("issueCreate") != std::string::npos ||
                    doc.find("commentCreate") != std::string::npos) {
                    return MutationResponse;
                }
                return ResolveResponse;
            },
            "POST");
    }
};

nlohmann::json MutationOk(const char* mutationName, const nlohmann::json& issue = nlohmann::json::object()) {
    return nlohmann::json{{"data", {{mutationName, {{"success", true}, {"issue", issue}}}}}};
}

} // namespace

TEST_CASE("Linear update — resolve hop + issueUpdate wire shape and error taxonomy") {
    JiraCatalogHttpFixture fx;
    smatchet_tests::TestEnvGuard env;

    SUBCASE("missing API key fails fast with zero HTTP calls (TestEnvGuard default config)") {
        LinearClient client(LoopbackGraphQlUrl(fx), "");
        const TrackerError err = client.UpdateIssueFields("ENG-123", nlohmann::json{{"title", "x"}});
        CHECK(err.Kind == TrackerErrorKind::Auth);
        CHECK(fx.RequestCount(kGraphQlPath) == 0);
    }

    SaveLoopbackLinearConfig(fx);
    LinearClient client(LoopbackGraphQlUrl(fx), "fixture-key");

    SUBCASE("empty IssueUpdateInput is InvalidRequest before any HTTP") {
        const TrackerError err = client.UpdateIssueFields("ENG-123", nlohmann::json::object());
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(fx.RequestCount(kGraphQlPath) == 0);
    }
    SUBCASE("happy path: identifier resolves to the UUID, issueUpdate carries it + the input") {
        LinearGraphQlScript script;
        script.MutationResponse = MutationOk("issueUpdate");
        script.Install(fx);
        const nlohmann::json input = {{"title", "renamed"}};
        const TrackerError err = client.UpdateIssueFields("ENG-123", input);
        CHECK(err.IsOk());
        CHECK(fx.RequestCount(kGraphQlPath) == 2); // resolve + mutation, in that order
        const std::vector<std::string> bodies = script.Capture.Snapshot();
        REQUIRE(bodies.size() == 2);
        const nlohmann::json resolve = nlohmann::json::parse(bodies[0]);
        CHECK(resolve["variables"]["id"].get<std::string>() == "ENG-123");
        const nlohmann::json update = nlohmann::json::parse(bodies[1]);
        CHECK(update["variables"]["id"].get<std::string>() == kIssueUuid);
        CHECK(update["variables"]["input"] == input);
    }
    SUBCASE("unknown identifier: resolve returns no issue → InvalidRequest naming it") {
        LinearGraphQlScript script;
        script.ResolveResponse = nlohmann::json{{"data", {{"issue", nullptr}}}};
        script.Install(fx);
        const TrackerError err = client.UpdateIssueFields("ENG-999", nlohmann::json{{"title", "x"}});
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail.find("ENG-999") != std::string::npos);
        CHECK(fx.RequestCount(kGraphQlPath) == 1); // the mutation never fires
    }
    SUBCASE("issueUpdate success=false is surfaced as the stable message") {
        LinearGraphQlScript script;
        script.MutationResponse = nlohmann::json{{"data", {{"issueUpdate", {{"success", false}}}}}};
        script.Install(fx);
        const TrackerError err = client.UpdateIssueFields("ENG-123", nlohmann::json{{"title", "x"}});
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail == "Linear issueUpdate returned success=false");
    }
    SUBCASE("GraphQL errors[] on the mutation is surfaced with the joined message") {
        LinearGraphQlScript script;
        script.MutationResponse = nlohmann::json{{"errors", {{{"message", "Field input is invalid"}}}}};
        script.Install(fx);
        const TrackerError err = client.UpdateIssueFields("ENG-123", nlohmann::json{{"title", "x"}});
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail == "Field input is invalid");
    }
}

TEST_CASE("Linear create — payload guard, identifier extraction, created-key-unknown") {
    JiraCatalogHttpFixture fx;
    smatchet_tests::TestEnvGuard env;
    SaveLoopbackLinearConfig(fx);
    LinearClient client(LoopbackGraphQlUrl(fx), "fixture-key");

    SUBCASE("payload without teamId/title is InvalidRequest before any HTTP") {
        const auto created = client.CreateIssue(nlohmann::json{{"title", "no team"}});
        REQUIRE_FALSE(created.has_value());
        CHECK(created.error().Kind == TrackerErrorKind::InvalidRequest);
        CHECK(fx.RequestCount(kGraphQlPath) == 0);
    }
    SUBCASE("issueCreate returns the identifier the grid keys by") {
        LinearGraphQlScript script;
        script.MutationResponse =
            MutationOk("issueCreate", nlohmann::json{{"id", kIssueUuid}, {"identifier", "ENG-42"}});
        script.Install(fx);
        const nlohmann::json input = {{"teamId", "team-uuid"}, {"title", "new issue"}};
        const auto created = client.CreateIssue(input);
        REQUIRE(created.has_value());
        CHECK(created.value() == "ENG-42");
        // issueCreate needs no resolve hop — exactly one POST, carrying the input.
        CHECK(fx.RequestCount(kGraphQlPath) == 1);
        const std::vector<std::string> bodies = script.Capture.Snapshot();
        REQUIRE(bodies.size() == 1);
        CHECK(nlohmann::json::parse(bodies[0])["variables"]["input"] == input);
    }
    SUBCASE("issueCreate success with no identifier is Ok(empty) — created-key-unknown") {
        LinearGraphQlScript script;
        script.MutationResponse = MutationOk("issueCreate");
        script.Install(fx);
        const auto created = client.CreateIssue(nlohmann::json{{"teamId", "t"}, {"title", "x"}});
        REQUIRE(created.has_value());
        CHECK(created.value().empty());
    }
}

TEST_CASE("Linear comment — resolve hop + commentCreate wire shape (markdown verbatim)") {
    JiraCatalogHttpFixture fx;
    TrackerConfig cfg;
    cfg.LinearBaseUrl = LoopbackGraphQlUrl(fx);
    cfg.LinearApiKey = "live-key";
    // cfg-carrying path: ctor'd with a bogus URL to prove the live cfg wins (#979).
    LinearClient client("http://bogus.invalid", "ctor-key");

    SUBCASE("happy path posts commentCreate with the resolved UUID and the raw markdown") {
        LinearGraphQlScript script;
        script.MutationResponse = MutationOk("commentCreate");
        script.Install(fx);
        const TrackerError err = client.AddIssueCommentPlain(cfg, "ENG-123", "some **bold** text");
        CHECK(err.IsOk());
        CHECK(fx.RequestCount(kGraphQlPath) == 2);
        const std::vector<std::string> bodies = script.Capture.Snapshot();
        REQUIRE(bodies.size() == 2);
        const nlohmann::json comment = nlohmann::json::parse(bodies[1]);
        CHECK(comment["variables"]["input"]["issueId"].get<std::string>() == kIssueUuid);
        CHECK(comment["variables"]["input"]["body"].get<std::string>() == "some **bold** text");
    }
    SUBCASE("cleared live key is Auth even though the ctor snapshot had one (#979)") {
        cfg.LinearApiKey.clear();
        const TrackerError err = client.AddIssueCommentPlain(cfg, "ENG-123", "text");
        CHECK(err.Kind == TrackerErrorKind::Auth);
        CHECK(fx.RequestCount(kGraphQlPath) == 0);
    }
}
