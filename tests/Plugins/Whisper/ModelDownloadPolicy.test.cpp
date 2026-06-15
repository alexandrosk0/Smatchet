// Whisper model-download network hardening (security audit LOW: redirect with no host-pin
// / no size cap). Pure-logic tests for the host-allow-list + size-cap decisions the
// downloader applies on the initial URL, each redirect target, and the streamed byte count.
// No cpr, no sockets, no I/O.

#include "ModelDownloadPolicy.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::whisper::policy::ExceedsModelSizeCap;
using smatchet::whisper::policy::ExtractHost;
using smatchet::whisper::policy::IsAllowedModelHost;
using smatchet::whisper::policy::IsAllowedModelUrl;
using smatchet::whisper::policy::kMaxModelDownloadBytes;

TEST_CASE("ExtractHost pulls the bare lowercased host from an absolute URL") {
    CHECK(ExtractHost("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin") ==
          "huggingface.co");
    CHECK(ExtractHost("https://CDN-LFS.huggingface.co/path?x=1") == "cdn-lfs.huggingface.co");
    CHECK(ExtractHost("https://host:8443/p") == "host");              // port stripped.
    CHECK(ExtractHost("https://user:pw@cdn.hf.co/p") == "cdn.hf.co"); // userinfo stripped.
    CHECK(ExtractHost("not-a-url").empty());                          // no scheme.
    CHECK(ExtractHost("").empty());
}

TEST_CASE("IsAllowedModelHost: huggingface apex + subdomains, plus hf.co; look-alikes rejected") {
    SUBCASE("allow-listed") {
        CHECK(IsAllowedModelHost("huggingface.co"));
        CHECK(IsAllowedModelHost("cdn-lfs.huggingface.co"));
        CHECK(IsAllowedModelHost("cdn-lfs-us-1.huggingface.co"));
        CHECK(IsAllowedModelHost("hf.co"));
        CHECK(IsAllowedModelHost("cdn.hf.co"));
        CHECK(IsAllowedModelHost("HuggingFace.Co")); // case-insensitive.
    }
    SUBCASE("rejected look-alikes / off-list") {
        CHECK_FALSE(IsAllowedModelHost("evilhuggingface.co")); // not a dotted-boundary suffix.
        CHECK_FALSE(IsAllowedModelHost("huggingface.co.attacker.com"));
        CHECK_FALSE(IsAllowedModelHost("attacker.com"));
        CHECK_FALSE(IsAllowedModelHost("nothf.co"));
        CHECK_FALSE(IsAllowedModelHost(""));
    }
}

TEST_CASE("IsAllowedModelUrl: https + allow-listed host required") {
    SUBCASE("accepts the catalog URL and a CDN redirect target") {
        CHECK(IsAllowedModelUrl("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"));
        CHECK(IsAllowedModelUrl("https://cdn-lfs.huggingface.co/repos/abc/ggml-tiny.en.bin?token=x"));
    }
    SUBCASE("rejects cleartext downgrade, off-list host, and garbage") {
        CHECK_FALSE(IsAllowedModelUrl("http://huggingface.co/x.bin")); // http downgrade refused.
        CHECK_FALSE(IsAllowedModelUrl("https://attacker.example.com/x.bin"));
        CHECK_FALSE(IsAllowedModelUrl("https://evilhuggingface.co/x.bin"));
        CHECK_FALSE(IsAllowedModelUrl("ftp://huggingface.co/x.bin"));
        CHECK_FALSE(IsAllowedModelUrl(""));
    }
}

TEST_CASE("ExceedsModelSizeCap: ceiling boundary") {
    CHECK_FALSE(ExceedsModelSizeCap(0));
    CHECK_FALSE(ExceedsModelSizeCap(1533774781ull));          // largest catalog model (medium) is fine.
    CHECK_FALSE(ExceedsModelSizeCap(kMaxModelDownloadBytes)); // exactly at cap is allowed.
    CHECK(ExceedsModelSizeCap(kMaxModelDownloadBytes + 1));   // one byte over -> abort.
}
