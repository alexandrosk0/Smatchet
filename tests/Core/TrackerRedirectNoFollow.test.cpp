// tracker-redirect-no-follow-regression-test.
//
// SECURITY regression for MakeTrackerRedirectPolicy() (TrackerHttpUtils.cpp,
// security H4 / E2). The tracker `Authorization` header is a caller-set RAW header,
// not libcurl CURLOPT_USERPWD, so curl's CURLOPT_UNRESTRICTED_AUTH=0 default does
// NOT strip it on a cross-host 30x. The policy therefore disables redirect-following
// entirely (cpr::Redirect(follow=false, cont_send_cred=false)). If a future refactor
// re-enabled follow, a malicious/MITM tracker host could 30x a request to an
// attacker host and the API token would be forwarded.
//
// This file asserts BOTH halves of the contract:
//   (1) NO-FOLLOW   — the policy object is configured follow=false (deterministic,
//                     inspected directly), and a live cross-host 302 is returned to
//                     the caller verbatim rather than chased (the redirect-target
//                     server is never hit).
//   (2) NO CRED LEAK — the policy object is configured cont_send_cred=false, and on
//                     the live round-trip the cross-host redirect target NEVER
//                     receives the `Authorization` header (it never receives any
//                     request at all, because the redirect is not followed).
//
// cpr + httplib bound — belongs in the cpr-bearing SmatchetTests rig (NOT the
// cpr-free TSan / SQLite-free rigs). TrackerHttpUtils.cpp + TrackerHttpClient.cpp +
// cpr + httplib are all already linked into that target.

#include "TrackerHttpUtils.h"

#include <doctest/doctest.h>

#include <cpr/cpr.h>
#include <httplib.h>

#include <mutex>
#include <string>
#include <thread>

namespace {

// A minimal in-process loopback server that records, per request, the path it was
// hit on and the `Authorization` header it received. Purpose-built here (rather than
// extending the shared JiraCatalogHttpFixture) so the cross-host redirect + the
// captured-Authorization assertion stay self-contained and unambiguous for the
// security regression. Header-only httplib, same lifecycle pattern as the shared
// HTTP fixtures.
class RecordingServer {
  public:
    RecordingServer() {
        server_.Get(".*", [this](const httplib::Request& req, httplib::Response& res) { Record(req, res); });
        port_ = server_.bind_to_any_port("127.0.0.1");
        thread_ = std::thread([this]() { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~RecordingServer() {
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    RecordingServer(const RecordingServer&) = delete;
    RecordingServer& operator=(const RecordingServer&) = delete;

    // Make every GET on this server reply with a 302 whose Location points at
    // `redirectTo` (a full absolute URL, used to point at a DIFFERENT host:port —
    // a cross-host redirect for libcurl's credential-stripping origin check).
    void RedirectAllTo(const std::string& redirectTo) {
        std::lock_guard<std::mutex> lock(mutex_);
        redirectTo_ = redirectTo;
    }

    int RequestCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requestCount_;
    }

    // The `Authorization` header value seen on the most recent request (empty if the
    // server was never hit, or the request carried no Authorization header).
    std::string LastAuthorization() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastAuthorization_;
    }

    std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

  private:
    void Record(const httplib::Request& req, httplib::Response& res) {
        std::string redirectTo;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++requestCount_;
            lastAuthorization_ =
                req.has_header("Authorization") ? req.get_header_value("Authorization") : std::string();
            redirectTo = redirectTo_;
        }
        if (!redirectTo.empty()) {
            res.status = 302;
            res.set_header("Location", redirectTo);
            res.set_content("{}", "application/json");
            return;
        }
        res.status = 200;
        res.set_content("{}", "application/json");
    }

    httplib::Server server_;
    std::thread thread_;
    int port_ = 0;

    mutable std::mutex mutex_;
    int requestCount_ = 0;
    std::string lastAuthorization_;
    std::string redirectTo_;
};

TrackerConfig MakeConfig(const std::string& baseUrl, const std::string& token) {
    TrackerConfig cfg;
    cfg.Domain = baseUrl;
    cfg.Email = "fixture@test.invalid";
    cfg.ApiToken = token;
    cfg.TrackerType = "Jira";
    return cfg;
}

} // namespace

// (1) + (2), deterministic half: the policy object itself is configured no-follow and
// no-cred-on-redirect. This is the faithful unit assertion — if a refactor changes
// either field the test fails without needing a live socket.
TEST_CASE("MakeTrackerRedirectPolicy disables follow and cross-host credential resend") {
    const cpr::Redirect policy = MakeTrackerRedirectPolicy();

    // NO-FOLLOW: 3xx responses are never chased.
    CHECK(policy.follow == false);
    // NO CRED LEAK: even if follow were ever re-enabled, credentials must not be
    // re-sent across a hostname change (CURLOPT_UNRESTRICTED_AUTH stays off).
    CHECK(policy.cont_send_cred == false);
}

// (1) live half: a real tracker GET against a server that 302s to a DIFFERENT
// host:port returns the 302 verbatim — the redirect is NOT followed.
TEST_CASE("Tracker GET does not follow a cross-host redirect") {
    RecordingServer attackerHost; // the cross-host redirect target
    RecordingServer trackerHost;  // the configured tracker host
    trackerHost.RedirectAllTo(attackerHost.BaseUrl() + "/leak");

    const TrackerConfig cfg = MakeConfig(trackerHost.BaseUrl(), "pr20-secret-token");
    const cpr::Response resp = TrackerGetLogged("test", trackerHost.BaseUrl() + "/probe", BuildTrackerHeaders(cfg));

    // The 302 surfaces to the caller as a non-2xx status (callers already handle it),
    // proving redirect-following is disabled.
    CHECK(resp.status_code == 302);
    // The tracker host was hit exactly once (the original request, never re-issued).
    CHECK(trackerHost.RequestCount() == 1);
}

// (2) live half — the SECURITY assertion: the cross-host redirect target NEVER
// receives the request, and therefore NEVER receives the `Authorization` header
// carrying the API token. This is the credential-leak guard.
TEST_CASE("Tracker Authorization header is never re-sent to a cross-host redirect target") {
    RecordingServer attackerHost; // the cross-host redirect target
    RecordingServer trackerHost;  // the configured tracker host
    trackerHost.RedirectAllTo(attackerHost.BaseUrl() + "/steal-token");

    const TrackerConfig cfg = MakeConfig(trackerHost.BaseUrl(), "pr20-secret-token");
    const cpr::Response resp = TrackerGetLogged("test", trackerHost.BaseUrl() + "/probe", BuildTrackerHeaders(cfg));

    // Sanity: the tracker host did receive the credentialed request (the redirect
    // originates from there) — so the test is genuinely exercising the auth path.
    CHECK(resp.status_code == 302);
    CHECK(trackerHost.LastAuthorization().find("Basic ") == 0u);

    // CORE SECURITY GUARANTEE: the cross-host redirect target was NEVER contacted...
    CHECK(attackerHost.RequestCount() == 0);
    // ...so the Authorization header (and the API token inside it) never leaked to it.
    CHECK(attackerHost.LastAuthorization().empty());
}
