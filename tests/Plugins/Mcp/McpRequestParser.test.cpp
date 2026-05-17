// MCP request-parser pure-logic tests — exercises the URL/host parsing surface
// that fronts the JSON-RPC server's request-routing layer. No sockets, no HTTP.

#include "McpJsonRpcPure.h"
#include "SmatchetDefaults.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::mcp::pure::ExtractHostFromUrl;
using smatchet::mcp::pure::IsLoopbackAddress;
using smatchet::mcp::pure::LooksLikeHttpUrl;
using smatchet::mcp::pure::NormalizeDomain;

TEST_CASE("LooksLikeHttpUrl recognises both schemes and rejects others") {
    CHECK(LooksLikeHttpUrl("http://example.com/"));
    CHECK(LooksLikeHttpUrl("https://example.com/"));
    CHECK(LooksLikeHttpUrl("http://"));   // prefix-only — production accepts (find==0).
    CHECK(LooksLikeHttpUrl("https://"));  // same.

    CHECK_FALSE(LooksLikeHttpUrl(""));
    CHECK_FALSE(LooksLikeHttpUrl("ftp://example.com"));
    CHECK_FALSE(LooksLikeHttpUrl("ws://example.com"));
    CHECK_FALSE(LooksLikeHttpUrl("example.com"));
    CHECK_FALSE(LooksLikeHttpUrl(" http://example.com"));  // leading whitespace shifts prefix.
    CHECK_FALSE(LooksLikeHttpUrl("HTTP://example.com"));   // case-sensitive scheme check.
}

TEST_CASE("NormalizeDomain strips scheme / userinfo / port / path and lowercases") {
    // Forces McpJsonRpcPure.cpp NormalizeDomain ::lowercased path (final ToLowerAscii).
    CHECK(NormalizeDomain("Example.COM") == "example.com");
    CHECK(NormalizeDomain("example.com") == "example.com");

    // Bare host:port — port is stripped after lowercase tracks through.
    CHECK(NormalizeDomain("Example.com:8080") == "example.com");

    // Scheme stripped.
    CHECK(NormalizeDomain("https://example.com") == "example.com");
    CHECK(NormalizeDomain("HTTP://Example.com") == "example.com");

    // Path stripped before lowercase — full URL with userinfo + port + path.
    CHECK(NormalizeDomain("https://user@Example.com:443/path/sub") == "example.com");

    // Trailing whitespace + slash.
    CHECK(NormalizeDomain("  https://example.com/  ") == "example.com");
    CHECK(NormalizeDomain("\texample.com\n") == "example.com");

    // Empty / whitespace-only.
    CHECK(NormalizeDomain("") == "");
    CHECK(NormalizeDomain("   ") == "");

    // No-scheme bare path-shaped input — domain part before first slash.
    CHECK(NormalizeDomain("example.com/foo") == "example.com");
}

TEST_CASE("ExtractHostFromUrl returns lowercased host minus port for well-formed URLs") {
    CHECK(ExtractHostFromUrl("https://example.com/") == "example.com");
    CHECK(ExtractHostFromUrl("http://Example.COM/foo") == "example.com");
    CHECK(ExtractHostFromUrl("https://api.example.com:8080/path") == "api.example.com");

    // No path — host runs to end of string.
    CHECK(ExtractHostFromUrl("https://example.com") == "example.com");

    // Query / fragment as host terminator.
    CHECK(ExtractHostFromUrl("https://example.com?x=1") == "example.com");
    CHECK(ExtractHostFromUrl("https://example.com#frag") == "example.com");

    // IPv6 in brackets — bracket stripping path. Port suffix outside brackets ignored
    // because hostAndPort.front() == '[' takes the bracket branch.
    CHECK(ExtractHostFromUrl("http://[::1]:8080/foo") == "::1");
    CHECK(ExtractHostFromUrl("http://[2001:db8::1]/") == "2001:db8::1");
}

TEST_CASE("ExtractHostFromUrl returns empty for malformed inputs") {
    CHECK(ExtractHostFromUrl("no-scheme.com") == "");           // no "://" sep.
    CHECK(ExtractHostFromUrl("") == "");                        // empty.
    CHECK(ExtractHostFromUrl("https://") == "");                // scheme-only; hostStart >= size.
    CHECK(ExtractHostFromUrl("https:///path") == "");           // empty host part.
    CHECK(ExtractHostFromUrl("http://[unterminated") == "");    // missing closing bracket.
}

TEST_CASE("IsLoopbackAddress recognises canonical loopback forms (case + whitespace insensitive)" * doctest::test_suite("[high-risk]")) {
    // Forces McpJsonRpcPure.cpp IsLoopbackAddress ::trim+lowercase path AND the explicit
    // four-value set. Removing any of the literals breaks the matching assertion below.
    CHECK(IsLoopbackAddress(SmatchetDefaults::Mcp::kBindLocalhost));  // canonical "127.0.0.1".
    CHECK(IsLoopbackAddress("127.0.0.1"));
    CHECK(IsLoopbackAddress("::1"));
    CHECK(IsLoopbackAddress("localhost"));
    CHECK(IsLoopbackAddress("LOCALHOST"));
    CHECK(IsLoopbackAddress("LocalHost"));
    CHECK(IsLoopbackAddress("::ffff:127.0.0.1"));
    CHECK(IsLoopbackAddress("  127.0.0.1  "));  // trims surrounding whitespace.
    CHECK(IsLoopbackAddress("\tlocalhost\n"));
}

TEST_CASE("IsLoopbackAddress rejects non-loopback addresses") {
    CHECK_FALSE(IsLoopbackAddress(""));
    CHECK_FALSE(IsLoopbackAddress("   "));
    CHECK_FALSE(IsLoopbackAddress("127.0.0.2"));
    CHECK_FALSE(IsLoopbackAddress("192.168.1.1"));
    CHECK_FALSE(IsLoopbackAddress("10.0.0.1"));
    CHECK_FALSE(IsLoopbackAddress("example.com"));
    CHECK_FALSE(IsLoopbackAddress("0.0.0.0"));  // bind-any is NOT loopback.
    CHECK_FALSE(IsLoopbackAddress("::2"));
}
