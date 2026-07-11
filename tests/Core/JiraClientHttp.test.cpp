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

#include <doctest/doctest.h>

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
