// Slice D of docs/plans/http-fault-injection.md — pure unit coverage of the
// retry+classify engine (TrackerHttpClient.cpp). No socket: ClassifyTrackerResponse is
// driven with hand-built cpr::Response values, and TrackerHttpRequestWithRetry is driven
// with stateful requestFn lambdas that count invocations, so we assert retry COUNT and
// final TrackerErrorKind (not wall-clock timing — the backoff sleep is real and not
// injectable). The integration half (live wiring over loopback HTTP) lives in
// TrackerHttpFaults.test.cpp.

#include "TrackerHttpClient.h"

#include <doctest/doctest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace {

// Build a classified result the same way the production helpers do (via the real
// ClassifyTrackerResponse), so the retry tests exercise the exact Error mapping callers see.
TrackerHttpResult ResultFromStatus(int status, const std::string& text = "") {
    cpr::Response resp;
    resp.status_code = status;
    resp.text = text;
    return ClassifyTrackerResponse(resp);
}

// Transport-only predicate the POST call site passes (a never-landed request is safe to
// resend; a landed-then-5xx mutation is not).
bool RetryTransportOnly(const TrackerError& e) { return e.Kind == TrackerErrorKind::Transport; }

} // namespace

TEST_CASE("ClassifyTrackerResponse maps HTTP status to TrackerErrorKind") {
    CHECK(ClassifyTrackerResponse(ResultFromStatus(200).Response).Error.Kind == TrackerErrorKind::None);
    CHECK(ResultFromStatus(200).IsOk());
    CHECK(ResultFromStatus(401).Error.Kind == TrackerErrorKind::Auth);
    CHECK(ResultFromStatus(403).Error.Kind == TrackerErrorKind::Auth);
    CHECK(ResultFromStatus(404).Error.Kind == TrackerErrorKind::NotFound);
    CHECK(ResultFromStatus(429).Error.Kind == TrackerErrorKind::RateLimited);
    CHECK(ResultFromStatus(500).Error.Kind == TrackerErrorKind::ServerError);
    CHECK(ResultFromStatus(503).Error.Kind == TrackerErrorKind::ServerError);
    CHECK(ResultFromStatus(400).Error.Kind == TrackerErrorKind::InvalidRequest);
    CHECK(ResultFromStatus(422).Error.Kind == TrackerErrorKind::InvalidRequest);
    CHECK(ResultFromStatus(0).Error.Kind == TrackerErrorKind::Transport);
}

TEST_CASE("ClassifyTrackerResponse builds a detail string and preserves the raw response") {
    // Body present -> detail is the body.
    const TrackerHttpResult withBody = ResultFromStatus(500, "upstream boom");
    CHECK(withBody.Error.Detail == "upstream boom");
    CHECK(withBody.Status() == 500);
    CHECK(withBody.Response.status_code == 500);

    // No body, positive status -> generic "HTTP <code>".
    const TrackerHttpResult noBody = ResultFromStatus(503, "");
    CHECK(noBody.Error.Detail == "HTTP 503");

    // No body, transport (status 0) -> generic transport message.
    const TrackerHttpResult transport = ResultFromStatus(0, "");
    CHECK(transport.Error.Detail == "Unknown network error");
}

TEST_CASE("TrackerHttpRequestWithRetry stops as soon as a retry recovers") {
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry([&calls]() {
        ++calls;
        return calls == 1 ? ResultFromStatus(429) : ResultFromStatus(200);
    });
    CHECK(r.IsOk());
    CHECK(calls == 2); // 429 once, then 200 — never reaches attempt 3.
}

TEST_CASE("TrackerHttpRequestWithRetry exhausts maxAttempts on a persistent retryable error") {
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(
        [&calls]() {
            ++calls;
            return ResultFromStatus(429);
        },
        2);
    CHECK_FALSE(r.IsOk());
    CHECK(calls == 2);
    CHECK(r.Error.Kind == TrackerErrorKind::RateLimited);
}

TEST_CASE("Default predicate retries 5xx ServerError") {
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(
        [&calls]() {
            ++calls;
            return ResultFromStatus(500);
        },
        2);
    CHECK(calls == 2);
    CHECK(r.Error.Kind == TrackerErrorKind::ServerError);
}

TEST_CASE("Default predicate does NOT retry non-retryable errors") {
    SUBCASE("Auth 401 is single-shot") {
        int calls = 0;
        const TrackerHttpResult r = TrackerHttpRequestWithRetry(
            [&calls]() {
                ++calls;
                return ResultFromStatus(401);
            },
            3);
        CHECK(calls == 1);
        CHECK(r.Error.Kind == TrackerErrorKind::Auth);
    }
    SUBCASE("NotFound 404 is single-shot") {
        int calls = 0;
        const TrackerHttpResult r = TrackerHttpRequestWithRetry(
            [&calls]() {
                ++calls;
                return ResultFromStatus(404);
            },
            3);
        CHECK(calls == 1);
        CHECK(r.Error.Kind == TrackerErrorKind::NotFound);
    }
}

TEST_CASE("POST Transport-only predicate never resends a landed-then-5xx mutation") {
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(
        [&calls]() {
            ++calls;
            return ResultFromStatus(500); // reached the server, server failed — not safe to resend.
        },
        3, nullptr, RetryTransportOnly);
    CHECK(calls == 1);
    CHECK(r.Error.Kind == TrackerErrorKind::ServerError);
}

TEST_CASE("POST Transport-only predicate DOES retry a never-landed request") {
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(
        [&calls]() {
            ++calls;
            return ResultFromStatus(0); // status 0 = Transport: request provably never landed.
        },
        2, nullptr, RetryTransportOnly);
    CHECK(calls == 2);
    CHECK(r.Error.Kind == TrackerErrorKind::Transport);
}

TEST_CASE("maxAttempts below 1 is clamped to a single attempt") {
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(
        [&calls]() {
            ++calls;
            return ResultFromStatus(429);
        },
        0);
    CHECK(calls == 1);
}

TEST_CASE("An empty requestFn yields an Unknown error without invoking anything") {
    const std::function<TrackerHttpResult()> empty;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(empty);
    CHECK(r.Error.Kind == TrackerErrorKind::Unknown);
    CHECK(r.Error.Detail.find("empty requestFn") != std::string::npos);
}

TEST_CASE("cancelled before the first attempt short-circuits to Cancelled") {
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(
        [&calls]() {
            ++calls;
            return ResultFromStatus(200);
        },
        3, []() { return true; });
    CHECK(calls == 0);
    CHECK(r.Error.Kind == TrackerErrorKind::Cancelled);
}

TEST_CASE("cancelled flipping true mid-loop stops further retries with Cancelled") {
    // Guards the http-fault-injection-residue fix: the search-fetch GET loops now thread the sync
    // worker's cancel token into the retry wrapper, so an abort during a backoff/retry window is
    // honoured immediately. The first 429 schedules a retry; the token flips before that retry, so
    // the loop must exit Cancelled WITHOUT a second requestFn invocation.
    int calls = 0;
    bool cancel = false;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(
        [&calls, &cancel]() {
            ++calls;
            cancel = true; // abort requested while the first attempt's backoff is pending.
            return ResultFromStatus(429);
        },
        3, [&cancel]() { return cancel; });
    CHECK(calls == 1); // first attempt ran; the cancel poll before attempt 2 short-circuits.
    CHECK(r.Error.Kind == TrackerErrorKind::Cancelled);
}

TEST_CASE("A first-try success returns immediately") {
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(
        [&calls]() {
            ++calls;
            return ResultFromStatus(200);
        },
        3);
    CHECK(calls == 1);
    CHECK(r.IsOk());
}

TEST_CASE("shared_ptr<atomic<bool>> shutdown token raised mid-loop aborts the mutation retry path") {
    // Mutation-path cancel wiring (automation-shutdown): JiraClient/PlaneClient::UpdateIssueFields wrap
    // the AppController-owned `shared_ptr<atomic<bool>>` shutdown token in exactly this `cancelled`
    // lambda shape and forward it to TrackerPut/Patch/PostLogged. This asserts the token shape drives
    // the retry wrapper to Cancelled WITHOUT a second request, mirroring shutdown flipping the token on
    // an automation worker blocked in a tracker mutation so its bounded join completes (no .detach).
    auto shutdownToken = std::make_shared<std::atomic<bool>>(false);
    std::function<bool()> cancelled = [shutdownToken]() { return shutdownToken->load(); };
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry(
        [&calls, &shutdownToken]() {
            ++calls;
            shutdownToken->store(true);   // shutdown raises the token during the first attempt's backoff.
            return ResultFromStatus(503); // retryable 5xx → schedules a retry the cancel poll cuts off.
        },
        3, cancelled);
    CHECK(calls == 1); // first attempt ran; the token poll before attempt 2 short-circuits.
    CHECK(r.Error.Kind == TrackerErrorKind::Cancelled);
}
