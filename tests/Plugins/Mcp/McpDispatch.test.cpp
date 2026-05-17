// MCP dispatch-time decision-logic tests — the auth-compare and attachment-host
// gating helpers. Both are pure boolean predicates with security implications.

#include "McpJsonRpcPure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::mcp::pure::ConstantTimeStringEquals;
using smatchet::mcp::pure::IsAllowedAttachmentHost;

// NOTE: Timing-side-channel properties of ConstantTimeStringEquals cannot be asserted
// from a unit-test harness (would be hopelessly flaky). We only test boolean
// correctness here; the constant-time invariant is preserved by the implementation's
// always-reading-max(|a|,|b|)-bytes loop, audited by code review.
TEST_CASE("ConstantTimeStringEquals returns true for byte-equal strings of any length") {
    CHECK(ConstantTimeStringEquals("", ""));
    CHECK(ConstantTimeStringEquals("a", "a"));
    CHECK(ConstantTimeStringEquals("hello", "hello"));
    CHECK(ConstantTimeStringEquals(std::string(1024, 'x'), std::string(1024, 'x')));

    // Embedded NUL — strict byte-wise equality.
    const std::string nul1("a\0b", 3);
    const std::string nul2("a\0b", 3);
    CHECK(ConstantTimeStringEquals(nul1, nul2));
}

TEST_CASE("ConstantTimeStringEquals returns false on length mismatch or byte diff") {
    CHECK_FALSE(ConstantTimeStringEquals("", "x"));
    CHECK_FALSE(ConstantTimeStringEquals("x", ""));
    CHECK_FALSE(ConstantTimeStringEquals("foo", "foo "));   // length differs.
    CHECK_FALSE(ConstantTimeStringEquals("foo ", "foo"));

    // Single-byte diff at start.
    CHECK_FALSE(ConstantTimeStringEquals("Xello", "hello"));
    // Single-byte diff at middle.
    CHECK_FALSE(ConstantTimeStringEquals("heXlo", "hello"));
    // Single-byte diff at end.
    CHECK_FALSE(ConstantTimeStringEquals("hellX", "hello"));

    // Long equal-length pair with a single differing byte buried deep — exercises the
    // accumulating-XOR diff path.
    std::string a(1024, 'a');
    std::string b(1024, 'a');
    b[512] = 'b';
    CHECK_FALSE(ConstantTimeStringEquals(a, b));

    // Case sensitive.
    CHECK_FALSE(ConstantTimeStringEquals("Foo", "foo"));
}

TEST_CASE("IsAllowedAttachmentHost permits exact and subdomain matches against tracker"
          * doctest::test_suite("[high-risk]")) {
    // Forces McpJsonRpcPure.cpp IsAllowedAttachmentHost ::suffix-with-dot guard.
    // Removing the "." prefix from TrackerSuffix would admit `maliciousatlassian.net`
    // when the tracker is `atlassian.net` — verified by the negative case below.
    const std::string tracker = "tracker.atlassian.net";

    CHECK(IsAllowedAttachmentHost(tracker, tracker));                          // exact.
    CHECK(IsAllowedAttachmentHost("assets.tracker.atlassian.net", tracker));   // subdomain.
    CHECK(IsAllowedAttachmentHost("media.tracker.atlassian.net", tracker));    // subdomain.
    CHECK(IsAllowedAttachmentHost("a.b.tracker.atlassian.net", tracker));      // multi-level subdomain.
}

TEST_CASE("IsAllowedAttachmentHost rejects suffix-without-dot near-matches"
          * doctest::test_suite("[high-risk]")) {
    // The suffix guard inserts a leading "." — `maliciousatlassian.net` ends in
    // `atlassian.net` but not `.atlassian.net`, so it must be rejected.
    CHECK_FALSE(IsAllowedAttachmentHost("maliciousatlassian.net", "atlassian.net"));
    CHECK_FALSE(IsAllowedAttachmentHost("evil-tracker.atlassian.net.attacker.com",
                                        "tracker.atlassian.net"));
    CHECK_FALSE(IsAllowedAttachmentHost("notatracker.atlassian.net", "tracker.atlassian.net"));
}

TEST_CASE("IsAllowedAttachmentHost admits the Atlassian media exception unconditionally") {
    // api.media.atlassian.com is hardcoded; allowed regardless of tracker domain.
    CHECK(IsAllowedAttachmentHost("api.media.atlassian.com", "tracker.atlassian.net"));
    CHECK(IsAllowedAttachmentHost("api.media.atlassian.com", "plane.example.com"));
    CHECK(IsAllowedAttachmentHost("api.media.atlassian.com", "anything.else"));

    // Near-matches of the media host are NOT admitted (no subdomain logic for the
    // hardcoded exception).
    CHECK_FALSE(IsAllowedAttachmentHost("sub.api.media.atlassian.com", "x.y"));
    CHECK_FALSE(IsAllowedAttachmentHost("api.media.atlassian.co", "x.y"));
}

TEST_CASE("IsAllowedAttachmentHost rejects empty host or empty tracker") {
    CHECK_FALSE(IsAllowedAttachmentHost("", "tracker.atlassian.net"));
    CHECK_FALSE(IsAllowedAttachmentHost("foo.atlassian.net", ""));
    CHECK_FALSE(IsAllowedAttachmentHost("", ""));
    // Note: "api.media.atlassian.com" with empty tracker also rejected — the empty-guard
    // fires first, before the media-exception check.
    CHECK_FALSE(IsAllowedAttachmentHost("api.media.atlassian.com", ""));
}
