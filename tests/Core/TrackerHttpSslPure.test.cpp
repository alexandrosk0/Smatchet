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
    SUBCASE("userinfo is stripped before the host is classified") {
        // HostFromBase drops a user[:pass]@ prefix, so a loopback host behind credentials still
        // classifies as loopback and a public host behind credentials still does not.
        CHECK(IsLoopbackHost("http://user:pass@localhost:8080/jira"));
        CHECK(IsLoopbackHost("http://admin@127.0.0.1"));
        CHECK_FALSE(IsLoopbackHost("http://user:pass@corp-jira.example.com"));
    }
    SUBCASE("malformed bracketed IPv6 without a closing bracket is not loopback") {
        CHECK_FALSE(IsLoopbackHost("[::1")); // no ']' — brackets left intact, no literal match.
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
    SUBCASE("surrounding whitespace is trimmed before the scheme check") {
        CHECK(ShouldUpgradeCleartextBase("  http://corp-jira.example.com  "));
        CHECK_FALSE(ShouldUpgradeCleartextBase("\t https://corp-jira.example.com \n"));
    }
}

// The IsTrackerTransportErrorText doctest block was deleted with the function in N12 slice 3 —
// transport-ness travels as the structured TrackerError kind classified at each backend's error
// site (kind mapping pinned by tests/Core/Tracker2xxErrorGuard.test.cpp).
