#include <doctest/doctest.h>

#include "Tracker/TrackerHttpPure.h"

#include <string>

using TrackerHttpPure::GetCaBundlePath;
using TrackerHttpPure::ResolveSslConfig;
using TrackerHttpPure::SetCaBundlePath;
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
