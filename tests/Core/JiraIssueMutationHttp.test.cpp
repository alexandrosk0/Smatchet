// N12 slice 3 (retire-transport-error-text.md) — kind-classification coverage for the Jira
// mutation shell: `UpdateIssueFields` (Via* helpers) and `AddIssueToSprint` now classify their
// failures into a structured TrackerError at the failure site instead of collapsing to Unknown.
// The field-edit chain's IsRetryable() consumers (13a/13b: OfflineQueueService replay,
// FieldEditPipelineService::ErrorTransient) branch on this kind, so a transport-shaped failure
// must classify retryable and a validation failure must not. A real `JiraClient` is driven at an
// in-process httplib loopback over real cpr HTTP. The cfg-less mutation paths load
// ConfigManager::Load() per request, so cases run under TestEnvGuard with the loopback config
// saved into the guard's private dir (PlaneIssueMutationHttp precedent).

#include "JiraCatalogHttpFixture.h"
#include "JiraClient.h"
#include "TestEnvGuard.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

namespace {

const std::string kIssuePath = "/rest/api/3/issue/SMT-1";
const std::string kSprintPath = "/rest/agile/1.0/sprint/77/issue";

// The cfg-less mutation paths read ConfigManager::Load() per request — persist the loopback
// config into the TestEnvGuard-redirected dir so they resolve to the fixture.
void SaveLoopbackJiraConfig(const JiraCatalogHttpFixture& fx) {
    TrackerConfig cfg = ConfigManager::Load();
    const TrackerConfig loopback = fx.Config();
    cfg.TrackerType = loopback.TrackerType;
    cfg.Domain = loopback.Domain;
    cfg.Email = loopback.Email;
    cfg.ApiToken = loopback.ApiToken;
    ConfigManager::Save(cfg);
}

} // namespace

TEST_CASE("JiraClient::UpdateIssueFields classifies the PUT failure status into the TrackerError kind") {
    TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    SaveLoopbackJiraConfig(fx);
    JiraClient client;
    const nlohmann::json fields = {{"summary", "new title"}};

    SUBCASE("HTTP 400 is InvalidRequest — non-retryable, never offline-queued") {
        fx.ScriptStatus(kIssuePath, 400, "PUT");
        const TrackerError err = client.UpdateIssueFields("SMT-1", fields);
        CHECK_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK_FALSE(err.IsRetryable());
    }
    SUBCASE("HTTP 503 is ServerError — retryable, so the offline-queue fallback stays reachable") {
        fx.ScriptStatus(kIssuePath, 503, "PUT");
        const TrackerError err = client.UpdateIssueFields("SMT-1", fields);
        CHECK_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::ServerError);
        CHECK(err.IsRetryable());
    }
}

TEST_CASE("JiraClient::AddIssueToSprint classifies the POST failure status into the TrackerError kind") {
    TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    SaveLoopbackJiraConfig(fx);
    JiraClient client;

    SUBCASE("HTTP 400 is InvalidRequest — non-retryable") {
        fx.ScriptStatus(kSprintPath, 400, "POST");
        const TrackerError err = client.AddIssueToSprint("SMT-1", "77");
        CHECK_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK_FALSE(err.IsRetryable());
    }
    SUBCASE("empty sprint id is InvalidRequest (local validation, no HTTP)") {
        const TrackerError err = client.AddIssueToSprint("SMT-1", "");
        CHECK_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK_FALSE(err.IsRetryable());
    }
}
