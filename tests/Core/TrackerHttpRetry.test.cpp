// Slice D of docs/plans/http-fault-injection.md — pure unit coverage of the
// retry+classify engine (TrackerHttpClient.cpp). No socket: ClassifyTrackerResponse is
// driven with hand-built cpr::Response values, and TrackerHttpRequestWithRetry is driven
// with stateful requestFn lambdas that count invocations, so we assert retry COUNT and
// final TrackerErrorKind (not wall-clock timing — the backoff sleep is real and not
// injectable). The integration half (live wiring over loopback HTTP) lives in
// TrackerHttpFaults.test.cpp.

#include "TrackerHttpClient.h"
#include "TrackerHttpPure.h"

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

// Same, with a `Retry-After` response header (the GitHub secondary-rate-limit shape).
TrackerHttpResult ResultFromStatusWithRetryAfter(int status, const std::string& retryAfter) {
    cpr::Response resp;
    resp.status_code = status;
    resp.header["Retry-After"] = retryAfter;
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

TEST_CASE("ClassifyTrackerResponse — 403 + Retry-After is a throttle, not an auth failure") {
    // GitHub's secondary rate limits / abuse detection answer 403 with a Retry-After
    // header; those must classify retryable (RateLimited) so the field-edit chain treats
    // them as an offline-queueable transient rather than a hard credential error.
    SUBCASE("403 with Retry-After → RateLimited (retryable)") {
        const TrackerHttpResult r = ResultFromStatusWithRetryAfter(403, "30");
        CHECK(r.Error.Kind == TrackerErrorKind::RateLimited);
        CHECK(r.Error.IsRetryable());
    }
    SUBCASE("header lookup is case-insensitive (cpr::Header contract)") {
        cpr::Response resp;
        resp.status_code = 403;
        resp.header["retry-after"] = "5";
        CHECK(ClassifyTrackerResponse(resp).Error.Kind == TrackerErrorKind::RateLimited);
    }
    SUBCASE("403 with an unparseable Retry-After (HTTP-date form) stays Auth") {
        const TrackerHttpResult r = ResultFromStatusWithRetryAfter(403, "Wed, 21 Oct 2026 07:28:00 GMT");
        CHECK(r.Error.Kind == TrackerErrorKind::Auth);
    }
    SUBCASE("plain 403 without the header stays Auth (real scope/credential failures)") {
        CHECK(ResultFromStatus(403).Error.Kind == TrackerErrorKind::Auth);
    }
    SUBCASE("401 with Retry-After stays Auth — only 403 wears the throttle disguise") {
        CHECK(ResultFromStatusWithRetryAfter(401, "30").Error.Kind == TrackerErrorKind::Auth);
    }
}

TEST_CASE("ParseRetryAfterSeconds — delta-seconds form only") {
    CHECK(TrackerHttpPure::ParseRetryAfterSeconds("120") == 120);
    CHECK(TrackerHttpPure::ParseRetryAfterSeconds(" 60 ") == 60);
    CHECK(TrackerHttpPure::ParseRetryAfterSeconds("0") == 0);
    CHECK(TrackerHttpPure::ParseRetryAfterSeconds("") == -1);
    CHECK(TrackerHttpPure::ParseRetryAfterSeconds("Wed, 21 Oct 2026 07:28:00 GMT") == -1);
    CHECK(TrackerHttpPure::ParseRetryAfterSeconds("-5") == -1);
    CHECK(TrackerHttpPure::ParseRetryAfterSeconds("12.5") == -1);
    CHECK(TrackerHttpPure::ParseRetryAfterSeconds("9999999") == -1); // > 6 digits = garbage
}

TEST_CASE("ComputeTrackerRetryDelayMs — backoff raised to a capped Retry-After") {
    constexpr long kBase = 250;
    constexpr long kExpCap = 4000;
    constexpr long kHonorCap = 30000;
    SUBCASE("no header (-1) → plain exponential backoff, capped") {
        CHECK(TrackerHttpPure::ComputeTrackerRetryDelayMs(1, kBase, kExpCap, -1, kHonorCap) == 250);
        CHECK(TrackerHttpPure::ComputeTrackerRetryDelayMs(2, kBase, kExpCap, -1, kHonorCap) == 500);
        CHECK(TrackerHttpPure::ComputeTrackerRetryDelayMs(3, kBase, kExpCap, -1, kHonorCap) == 1000);
        CHECK(TrackerHttpPure::ComputeTrackerRetryDelayMs(9, kBase, kExpCap, -1, kHonorCap) == 4000);
    }
    SUBCASE("Retry-After above the backoff wins") {
        CHECK(TrackerHttpPure::ComputeTrackerRetryDelayMs(1, kBase, kExpCap, 2, kHonorCap) == 2000);
    }
    SUBCASE("Retry-After below the backoff never shortens it") {
        CHECK(TrackerHttpPure::ComputeTrackerRetryDelayMs(9, kBase, kExpCap, 1, kHonorCap) == 4000);
    }
    SUBCASE("hostile Retry-After is capped at the honor ceiling") {
        CHECK(TrackerHttpPure::ComputeTrackerRetryDelayMs(1, kBase, kExpCap, 999999, kHonorCap) == kHonorCap);
    }
    SUBCASE("Retry-After: 0 (retry now) falls back to the backoff floor") {
        CHECK(TrackerHttpPure::ComputeTrackerRetryDelayMs(1, kBase, kExpCap, 0, kHonorCap) == 250);
    }
}

TEST_CASE("TrackerHttpRequestWithRetry retries a 403-with-Retry-After like any throttle") {
    int calls = 0;
    const TrackerHttpResult r = TrackerHttpRequestWithRetry([&calls]() {
        ++calls;
        // Retry-After: 0 keeps the honored delay at the plain backoff so the test doesn't sleep.
        return calls == 1 ? ResultFromStatusWithRetryAfter(403, "0") : ResultFromStatus(200);
    });
    CHECK(r.IsOk());
    CHECK(calls == 2); // the throttle-shaped 403 retried; a plain 403 would have been single-shot.
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
