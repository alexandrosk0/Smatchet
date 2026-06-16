// Slice D of docs/plans/http-fault-injection.md — integration coverage of the LIVE
// D3 wiring. Drives the real TrackerGet/Post/PutLogged helpers (TrackerHttpUtils.cpp) against
// a scripted in-process httplib loopback (JiraCatalogHttpFixture), then asserts on the
// fixture's per-path request count to prove the safe-tiered retry policy end-to-end over real
// cpr HTTP:
//   * idempotent GET / PUT retry on a forced 5xx/429 (count > 1),
//   * non-idempotent POST is single-shot on a landed 5xx (count == 1) so a mutation that
//     reached the server is never double-fired.
// The pure-unit half (classify table + predicate semantics, no socket) is in
// TrackerHttpRetry.test.cpp.

#include "JiraCatalogHttpFixture.h"
#include "TrackerHttpClient.h"
#include "TrackerHttpUtils.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

namespace {

std::string Url(const JiraCatalogHttpFixture& fx, const std::string& path) {
    return fx.Config().Domain + path;
}

} // namespace

TEST_CASE("Idempotent GET retries to exhaustion on a forced 429") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/probe", 429, "GET");
    const TrackerConfig cfg = fx.Config();

    const cpr::Response resp = TrackerGetLogged("test", Url(fx, "/probe"), BuildTrackerHeaders(cfg));

    CHECK(resp.status_code == 429);
    // Default maxAttempts == 3: original + two retries, all 429.
    CHECK(fx.RequestCount("/probe") == kTrackerHttpDefaultMaxAttempts);
}

TEST_CASE("Idempotent PUT retries to exhaustion on a forced 5xx") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/field", 500, "PUT");
    const TrackerConfig cfg = fx.Config();

    const cpr::Response resp = TrackerPutLogged("test", Url(fx, "/field"), BuildTrackerHeaders(cfg, true), "{}");

    CHECK(resp.status_code == 500);
    CHECK(fx.RequestCount("/field") == kTrackerHttpDefaultMaxAttempts);
}

TEST_CASE("Non-idempotent POST is single-shot on a landed 5xx") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/create", 500, "POST");
    const TrackerConfig cfg = fx.Config();

    const cpr::Response resp = TrackerPostLogged("test", Url(fx, "/create"), BuildTrackerHeaders(cfg, true), "{}");

    CHECK(resp.status_code == 500);
    // A POST that reached the server (status 500, not a transport failure) must NOT be resent.
    CHECK(fx.RequestCount("/create") == 1);
}

TEST_CASE("A successful GET makes exactly one request") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/ok", nlohmann::json::object(), "GET");
    const TrackerConfig cfg = fx.Config();

    const cpr::Response resp = TrackerGetLogged("test", Url(fx, "/ok"), BuildTrackerHeaders(cfg));

    CHECK(resp.status_code == 200);
    CHECK(fx.RequestCount("/ok") == 1);
}
