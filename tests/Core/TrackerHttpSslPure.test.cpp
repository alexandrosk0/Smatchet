#include <doctest/doctest.h>

#include "Tracker/TrackerHttpPure.h"

#include <string>

using TrackerHttpPure::GetCaBundlePath;
using TrackerHttpPure::IsLoopbackHost;
using TrackerHttpPure::ResolveSslConfig;
using TrackerHttpPure::SetCaBundlePath;
using TrackerHttpPure::ShouldUpgradeCleartextBase;
using TrackerHttpPure::SslConfig;

// WS2 / Issue #1068 — the runtime TLS-trust seam. These exercise the pure decision logic
// (ResolveSslConfig) and the process-global path holder (Set/Get). The cpr-side attachment
// (CURLOPT_CAINFO) is intentionally out of scope here — this layer is deliberately cpr-free.

TEST_CASE("ResolveSslConfig: empty path leaves libcurl default untouched") {
    SUBCASE("empty string -> no attach, empty cafile") {
        const SslConfig out = ResolveSslConfig("");
        CHECK_FALSE(out.attach);
        CHECK(out.caInfoPath.empty());
    }
}

TEST_CASE("ResolveSslConfig: non-empty path attaches an explicit CAINFO") {
    SUBCASE("realistic Android private-dir cacert path") {
        const std::string path = "/data/user/0/com.smatchet.mobile/files/cacert.pem";
        const SslConfig out = ResolveSslConfig(path);
        CHECK(out.attach);
        CHECK(out.caInfoPath == path);
    }

    SUBCASE("secondary-user path also attaches verbatim (no fixed-uid assumption)") {
        const std::string path = "/data/user/10/com.smatchet.mobile/files/cacert.pem";
        const SslConfig out = ResolveSslConfig(path);
        CHECK(out.attach);
        CHECK(out.caInfoPath == path);
    }

    SUBCASE("arbitrary path passes through unchanged") {
        const SslConfig out = ResolveSslConfig("/tmp/custom-ca.pem");
        CHECK(out.attach);
        CHECK(out.caInfoPath == "/tmp/custom-ca.pem");
    }
}

TEST_CASE("SetCaBundlePath / GetCaBundlePath: process-global round-trip") {
    SUBCASE("set then get returns the same path") {
        SetCaBundlePath("/data/user/0/com.smatchet.mobile/files/cacert.pem");
        CHECK(GetCaBundlePath() == "/data/user/0/com.smatchet.mobile/files/cacert.pem");

        // ResolveSslConfig over the stored path attaches it.
        const SslConfig out = ResolveSslConfig(GetCaBundlePath());
        CHECK(out.attach);
        CHECK(out.caInfoPath == "/data/user/0/com.smatchet.mobile/files/cacert.pem");
    }

    SUBCASE("overwrite replaces the prior value") {
        SetCaBundlePath("/first/ca.pem");
        SetCaBundlePath("/second/ca.pem");
        CHECK(GetCaBundlePath() == "/second/ca.pem");
    }

    SUBCASE("reset to empty disarms the explicit CAINFO (desktop default)") {
        SetCaBundlePath("/some/ca.pem");
        SetCaBundlePath("");
        CHECK(GetCaBundlePath().empty());
        CHECK_FALSE(ResolveSslConfig(GetCaBundlePath()).attach);
    }

    // Hygiene: leave the process-global holder empty so test ordering can't leak a CAINFO
    // into any other case that reads GetCaBundlePath().
    SetCaBundlePath("");
}

// Cleartext-credential hardening (security audit LOW): NormalizeBaseUrl must not send the
// tracker Basic-auth header over cleartext http:// to a non-loopback host. These pin the
// pure decision the production NormalizeBaseUrl applies before building any tracker URL.

TEST_CASE("IsLoopbackHost: loopback literals accepted, public hosts rejected") {
    SUBCASE("loopback literals (with/without scheme, port, IPv6)") {
        CHECK(IsLoopbackHost("localhost"));
        CHECK(IsLoopbackHost("localhost:8080"));
        CHECK(IsLoopbackHost("127.0.0.1"));
        CHECK(IsLoopbackHost("127.0.0.1:9999"));
        CHECK(IsLoopbackHost("127.5.5.5")); // any 127.x.x.x is loopback.
        CHECK(IsLoopbackHost("http://localhost:8080/jira"));
        CHECK(IsLoopbackHost("https://127.0.0.1/"));
        CHECK(IsLoopbackHost("::1"));
        CHECK(IsLoopbackHost("[::1]"));
        CHECK(IsLoopbackHost("[::1]:8080"));
    }
    SUBCASE("public / non-loopback hosts rejected") {
        CHECK_FALSE(IsLoopbackHost("corp-jira.example.com"));
        CHECK_FALSE(IsLoopbackHost("https://acme.atlassian.net"));
        CHECK_FALSE(IsLoopbackHost("127.example.com")); // not a 127.x literal — a hostname.
        CHECK_FALSE(IsLoopbackHost("localhost.evil.com"));
        CHECK_FALSE(IsLoopbackHost(""));
    }
}

TEST_CASE("ShouldUpgradeCleartextBase: cleartext http to public host must upgrade") {
    SUBCASE("non-loopback cleartext http -> upgrade") {
        CHECK(ShouldUpgradeCleartextBase("http://corp-jira.example.com"));
        CHECK(ShouldUpgradeCleartextBase("http://acme.atlassian.net/rest/api"));
        CHECK(ShouldUpgradeCleartextBase("HTTP://Corp-Jira.Example.Com")); // case-insensitive scheme.
    }
    SUBCASE("https never upgraded") {
        CHECK_FALSE(ShouldUpgradeCleartextBase("https://corp-jira.example.com"));
        CHECK_FALSE(ShouldUpgradeCleartextBase("https://127.0.0.1"));
    }
    SUBCASE("loopback cleartext http left as-is (local dev allowed)") {
        CHECK_FALSE(ShouldUpgradeCleartextBase("http://localhost:8080"));
        CHECK_FALSE(ShouldUpgradeCleartextBase("http://127.0.0.1:9999/jira"));
        CHECK_FALSE(ShouldUpgradeCleartextBase("http://[::1]:8080"));
    }
}

// IsTrackerTransportErrorText is the pure connectivity classifier (no cpr) that the offline-create
// path + OfflineQueueService read to decide "is this failure transport, or a real client error?".
// It lives in TrackerHttpPure (moved out of TrackerHttpUtils so cpr-free consumers — Sync, the TSan
// threading subset — classify without dragging in <cpr/cpr.h>). Global free function — no namespace
// qualifier. These pin both keyword families, the hard-wins precedence, and the conservative default.

TEST_CASE("IsTrackerTransportErrorText: transport/connectivity failures classify true") {
    SUBCASE("HTTP 0 + 5xx server-side") {
        CHECK(IsTrackerTransportErrorText("HTTP 0: empty reply"));
        CHECK(IsTrackerTransportErrorText("HTTP 500 internal server error"));
        CHECK(IsTrackerTransportErrorText("HTTP 502 bad gateway"));
        CHECK(IsTrackerTransportErrorText("HTTP 503 service unavailable"));
        CHECK(IsTrackerTransportErrorText("HTTP 504 gateway timeout"));
    }
    SUBCASE("timeouts / DNS / connect failures") {
        CHECK(IsTrackerTransportErrorText("operation timed out after 30000 ms"));
        CHECK(IsTrackerTransportErrorText("Could not resolve host: acme.atlassian.net"));
        CHECK(IsTrackerTransportErrorText("Failed to connect to corp-jira port 443"));
        CHECK(IsTrackerTransportErrorText("Connection refused"));
        CHECK(IsTrackerTransportErrorText("Connection reset by peer"));
        CHECK(IsTrackerTransportErrorText("Network is unreachable"));
    }
    SUBCASE("TLS / SSL connectivity") {
        CHECK(IsTrackerTransportErrorText("SSL connect error"));
        CHECK(IsTrackerTransportErrorText("certificate verify failed"));
        CHECK(IsTrackerTransportErrorText("schannel: failed to receive handshake"));
    }
    SUBCASE("case-insensitive (input is lowercased before matching)") {
        CHECK(IsTrackerTransportErrorText("TIMEOUT"));
        CHECK(IsTrackerTransportErrorText("Connection Refused"));
    }
}

TEST_CASE("IsTrackerTransportErrorText: client/config/auth failures classify false") {
    SUBCASE("missing config / auth") {
        CHECK_FALSE(IsTrackerTransportErrorText("Missing tracker domain"));
        CHECK_FALSE(IsTrackerTransportErrorText("Missing API token"));
        CHECK_FALSE(IsTrackerTransportErrorText("Tracker backend is not initialized"));
        CHECK_FALSE(IsTrackerTransportErrorText("Plane is not configured"));
        CHECK_FALSE(IsTrackerTransportErrorText("Plane API key is missing"));
    }
    SUBCASE("HTTP 4xx validation/auth responses are not transport") {
        CHECK_FALSE(IsTrackerTransportErrorText("HTTP 400 bad request"));
        CHECK_FALSE(IsTrackerTransportErrorText("HTTP 401 invalid credentials"));
        CHECK_FALSE(IsTrackerTransportErrorText("HTTP 403 forbidden"));
        CHECK_FALSE(IsTrackerTransportErrorText("HTTP 404 not found"));
        CHECK_FALSE(IsTrackerTransportErrorText("HTTP 409 conflict"));
        CHECK_FALSE(IsTrackerTransportErrorText("HTTP 422 unprocessable entity"));
    }
}

TEST_CASE("IsTrackerTransportErrorText: edge cases") {
    SUBCASE("empty string is not transport") { CHECK_FALSE(IsTrackerTransportErrorText("")); }
    SUBCASE("unknown text is conservatively not transport") {
        CHECK_FALSE(IsTrackerTransportErrorText("something completely unexpected happened"));
    }
    SUBCASE("hard tokens win over transport tokens (checked first)") {
        // Contains "http 400" (hard) AND "connection" (transport) — hard is scanned first, so the
        // string classifies as a client error (false), not transport. Pins the documented precedence.
        CHECK_FALSE(IsTrackerTransportErrorText("HTTP 400: bad connection parameters in request"));
    }
}
