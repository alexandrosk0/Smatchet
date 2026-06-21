// MCP dispatch-time decision-logic tests — the auth-compare and attachment-host
// gating helpers. Both are pure boolean predicates with security implications.

#include "McpJsonRpcPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>

using smatchet::mcp::pure::ConstantTimeStringEquals;
using smatchet::mcp::pure::IsAllowedAttachmentHost;
namespace detail = smatchet::mcp::pure::detail;

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
    CHECK_FALSE(ConstantTimeStringEquals("foo", "foo ")); // length differs.
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

TEST_CASE("IsAllowedAttachmentHost permits exact and subdomain matches against tracker" *
          doctest::test_suite("[high-risk]")) {
    // Forces McpJsonRpcPure.cpp IsAllowedAttachmentHost ::suffix-with-dot guard.
    // Removing the "." prefix from TrackerSuffix would admit `maliciousatlassian.net`
    // when the tracker is `atlassian.net` — verified by the negative case below.
    const std::string tracker = "tracker.atlassian.net";

    CHECK(IsAllowedAttachmentHost(tracker, tracker));                        // exact.
    CHECK(IsAllowedAttachmentHost("assets.tracker.atlassian.net", tracker)); // subdomain.
    CHECK(IsAllowedAttachmentHost("media.tracker.atlassian.net", tracker));  // subdomain.
    CHECK(IsAllowedAttachmentHost("a.b.tracker.atlassian.net", tracker));    // multi-level subdomain.
}

TEST_CASE("IsAllowedAttachmentHost rejects suffix-without-dot near-matches" * doctest::test_suite("[high-risk]")) {
    // The suffix guard inserts a leading "." — `maliciousatlassian.net` ends in
    // `atlassian.net` but not `.atlassian.net`, so it must be rejected.
    CHECK_FALSE(IsAllowedAttachmentHost("maliciousatlassian.net", "atlassian.net"));
    CHECK_FALSE(IsAllowedAttachmentHost("evil-tracker.atlassian.net.attacker.com", "tracker.atlassian.net"));
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

// --- pure::detail log-summary helpers (promoted from an anonymous namespace so the
// dispatch summary builders are directly testable). ---

TEST_CASE("detail::ToLowerAscii lowercases ASCII and leaves other bytes intact") {
    CHECK(detail::ToLowerAscii("HeLLo World") == "hello world");
    CHECK(detail::ToLowerAscii("ABC-123_XYZ") == "abc-123_xyz");
    CHECK(detail::ToLowerAscii("") == "");
}

TEST_CASE("detail::TrimAsciiWhitespace strips leading and trailing whitespace") {
    CHECK(detail::TrimAsciiWhitespace("  spaced  ") == "spaced");
    CHECK(detail::TrimAsciiWhitespace("\t\n value \r\n") == "value");
    CHECK(detail::TrimAsciiWhitespace("no-edge-ws") == "no-edge-ws");
    CHECK(detail::TrimAsciiWhitespace("   ") == "");
    CHECK(detail::TrimAsciiWhitespace("") == "");
}

TEST_CASE("detail::BasenameForDisplay returns the final path component") {
    CHECK(detail::BasenameForDisplay("Scripts/Automation.lua") == "Automation.lua");
    CHECK(detail::BasenameForDisplay("C:\\dir\\sub\\file.lua") == "file.lua");
    CHECK(detail::BasenameForDisplay("flat.lua") == "flat.lua");
    CHECK(detail::BasenameForDisplay("") == "");
    // Mixed separators — last of either wins.
    CHECK(detail::BasenameForDisplay("a/b\\c.lua") == "c.lua");
}

TEST_CASE("detail::AppendAllowlistedArgKvs emits only allow-listed keys with their values") {
    nlohmann::json args;
    args["issue_key"] = "PROJ-42";
    args["priority"] = "High";
    args["id"] = 7;
    args["secret"] = "should-not-appear"; // not allow-listed
    std::ostringstream oss;
    detail::AppendAllowlistedArgKvs(oss, args);
    const std::string out = oss.str();
    CHECK(out.find("issue_key=PROJ-42") != std::string::npos);
    CHECK(out.find("priority=High") != std::string::npos);
    CHECK(out.find("id=7") != std::string::npos);
    CHECK(out.find("secret") == std::string::npos);
}

TEST_CASE("detail::AppendAllowlistedArgKvs is a no-op on a non-object json") {
    std::ostringstream oss;
    detail::AppendAllowlistedArgKvs(oss, nlohmann::json("not-an-object"));
    CHECK(oss.str().empty());
}

// --- Summary builders that route through the promoted helpers. ---

TEST_CASE("BuildRunLuaSummary script mode uses BasenameForDisplay + allow-listed args") {
    nlohmann::json args;
    args["mode"] = "script";
    args["script"] = "Scripts/sub/MyHook.lua";
    args["args"] = {{"issue_key", "PROJ-9"}, {"ignored", "x"}};
    const std::string s = smatchet::mcp::pure::BuildRunLuaSummary(args);
    CHECK(s.find("mode=script") != std::string::npos);
    CHECK(s.find("script=MyHook.lua") != std::string::npos);
    CHECK(s.find("issue_key=PROJ-9") != std::string::npos);
    CHECK(s.find("ignored") == std::string::npos);
}

TEST_CASE("BuildToolCallSummary lists sorted-iteration arg keys + allow-listed values") {
    nlohmann::json args;
    args["issue_key"] = "PROJ-1";
    args["extra"] = "data";
    const std::string s = smatchet::mcp::pure::BuildToolCallSummary("update_ticket", args);
    CHECK(s.find("tool=update_ticket") != std::string::npos);
    CHECK(s.find("arg_keys=[") != std::string::npos);
    CHECK(s.find("issue_key=PROJ-1") != std::string::npos);
}
