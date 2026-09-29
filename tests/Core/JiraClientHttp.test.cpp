// BACKLOG_CODE_REVIEW.md §B2 (TrackerHttpClient migration) — characterization harness for
// JiraClient::ProbeReachability's HTTP-status → TrackerReachabilityProbeKind matrix, driven at
// an in-process httplib loopback (JiraCatalogHttpFixture) over real cpr HTTP against the real
// JiraClient. These tests pin the CURRENT behaviour so the §B2 batch that routes the probe
// through ClassifyTrackerResponse (mirroring the shipped PlaneClient::ProbeReachability Phase-2A
// pattern) is provably behaviour-preserving for every common status. There was no JiraClient
// HTTP-status test before this (the JiraFakeTrackerFixture probe test uses a fake client that
// bypasses the real classification code).

#include "JiraCatalogHttpFixture.h"
#include "JiraClient.h"
#include "TestEnvGuard.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

namespace {

// Probe an empty myself-endpoint response scripted to `status`, return the classified kind.
TrackerReachabilityProbeKind ProbeWithStatus(int status) {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/myself", status, "GET");
    JiraClient client;
    return client.ProbeReachability(fx.Config()).Kind;
}

} // namespace

TEST_CASE("JiraClient::ProbeReachability — 200 is AuthenticatedReachable") {
    CHECK(ProbeWithStatus(200) == TrackerReachabilityProbeKind::AuthenticatedReachable);
}

TEST_CASE("JiraClient::ProbeReachability — 401/403 are ReachableAuthOrConfigError") {
    CHECK(ProbeWithStatus(401) == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
    CHECK(ProbeWithStatus(403) == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
}

TEST_CASE("JiraClient::ProbeReachability — 404 / 429 (reachable, non-transport 4xx) are ReachableAuthOrConfigError") {
    // A stale base URL (404) or a rate-limited probe (429) means the host answered — it must not
    // flip the connectivity banner to offline. This is the exact matrix ClassifyTrackerResponse
    // preserves (NotFound / RateLimited → auth-or-config, not TransportDown).
    CHECK(ProbeWithStatus(404) == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
    CHECK(ProbeWithStatus(429) == TrackerReachabilityProbeKind::ReachableAuthOrConfigError);
}

TEST_CASE("JiraClient::ProbeReachability — 5xx is ServiceUnavailable") {
    CHECK(ProbeWithStatus(500) == TrackerReachabilityProbeKind::ServiceUnavailable);
    CHECK(ProbeWithStatus(503) == TrackerReachabilityProbeKind::ServiceUnavailable);
}

TEST_CASE("JiraClient::ProbeReachability — no server (transport failure) is TransportDown") {
    // Point at a closed loopback port so cpr fails to connect (status_code 0 → Transport).
    TrackerConfig cfg;
    cfg.Domain = "http://127.0.0.1:9"; // discard port — nothing listening
    cfg.Email = "fixture@test.invalid";
    cfg.ApiToken = "fixture-token";
    cfg.TrackerType = "Jira";
    JiraClient client;
    CHECK(client.ProbeReachability(cfg).Kind == TrackerReachabilityProbeKind::TransportDown);
}

TEST_CASE("JiraClient::ListProjectsTyped — a failure keeps its kind instead of reading as no projects") {
    // Quality Pillar 6 (offline-first S12): every failure used to return an empty list, so the project
    // picker showed "No projects found." for an unreachable tracker. ListProjectsTyped is cfg-less, so
    // the loopback config is saved into the guard's private dir. A fresh client per case: the list is
    // cached per instance.
    smatchet_tests::TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    ConfigManager::Save(fx.Config());
    const std::string path = "/rest/api/3/project";

    SUBCASE("200 array: the rows") {
        fx.ScriptJson(path,
                      nlohmann::json::array({nlohmann::json{{"id", "10000"}, {"key", "OFF"}, {"name", "Offline"}}}));
        JiraClient client;
        const auto listed = client.ListProjectsTyped();
        REQUIRE(static_cast<bool>(listed));
        REQUIRE(listed.value().size() == 1u);
        CHECK(listed.value()[0].key == "OFF");
        CHECK(listed.value()[0].displayName == "Offline");
    }
    SUBCASE("401 is Auth, not retryable") {
        fx.ScriptStatus(path, 401);
        JiraClient client;
        const auto listed = client.ListProjectsTyped();
        REQUIRE_FALSE(static_cast<bool>(listed));
        CHECK(listed.error().Kind == TrackerErrorKind::Auth);
        CHECK_FALSE(listed.error().IsRetryable());
    }
    SUBCASE("503 is ServerError, retryable") {
        fx.ScriptStatus(path, 503);
        JiraClient client;
        const auto listed = client.ListProjectsTyped();
        REQUIRE_FALSE(static_cast<bool>(listed));
        CHECK(listed.error().Kind == TrackerErrorKind::ServerError);
        CHECK(listed.error().IsRetryable());
    }
    SUBCASE("a body that is not an array is Parse") {
        fx.ScriptJson(path, nlohmann::json{{"values", nlohmann::json::array()}});
        JiraClient client;
        const auto listed = client.ListProjectsTyped();
        REQUIRE_FALSE(static_cast<bool>(listed));
        CHECK(listed.error().Kind == TrackerErrorKind::Parse);
    }
    SUBCASE("an unreachable host is Transport, and the best-effort list is empty") {
        TrackerConfig cfg = fx.Config();
        cfg.Domain = "http://127.0.0.1:9"; // discard port — nothing listening
        ConfigManager::Save(cfg);
        JiraClient client;
        const auto listed = client.ListProjectsTyped();
        REQUIRE_FALSE(static_cast<bool>(listed));
        CHECK(listed.error().Kind == TrackerErrorKind::Transport);
        CHECK(listed.error().IsRetryable());
        CHECK(listed.error().Detail.find("HTTP 0") != std::string::npos);
        CHECK(client.ListProjects().empty());
    }
}
