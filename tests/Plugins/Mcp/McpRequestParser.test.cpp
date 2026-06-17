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
using smatchet::mcp::pure::UrlHasUserinfo;

TEST_CASE("LooksLikeHttpUrl recognises both schemes and rejects others") {
    CHECK(LooksLikeHttpUrl("http://example.com/"));
    CHECK(LooksLikeHttpUrl("https://example.com/"));
    CHECK(LooksLikeHttpUrl("http://"));  // prefix-only — production accepts (find==0).
    CHECK(LooksLikeHttpUrl("https://")); // same.

    CHECK_FALSE(LooksLikeHttpUrl(""));
    CHECK_FALSE(LooksLikeHttpUrl("ftp://example.com"));
    CHECK_FALSE(LooksLikeHttpUrl("ws://example.com"));
    CHECK_FALSE(LooksLikeHttpUrl("example.com"));
    CHECK_FALSE(LooksLikeHttpUrl(" http://example.com")); // leading whitespace shifts prefix.
    CHECK_FALSE(LooksLikeHttpUrl("HTTP://example.com"));  // case-sensitive scheme check.
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

TEST_CASE("UrlHasUserinfo flags the allow-list-bypass authority (security)" * doctest::test_suite("[high-risk]")) {
    // The attack: cpr/curl connect to the host AFTER the last '@'. A naive
    // allow-list that splits the authority on the first ':' reads the userinfo
    // prefix as the host. `https://allowed.example.net:x@evil.com/` would pass an
    // allow-list keyed on `allowed.example.net` while the fetch -- carrying the
    // Basic-auth tracker credential -- lands on `evil.com`. The proxy rejects any
    // userinfo URL outright via this predicate before host validation.
    CHECK(UrlHasUserinfo("https://allowed.example.net:x@evil.com/"));
    CHECK(UrlHasUserinfo("https://user:pass@allowed.example.com/"));
    CHECK(UrlHasUserinfo("https://user@example.com"));     // no path; authority runs to end.
    CHECK(UrlHasUserinfo("https://user@example.com?x=1")); // query terminates the authority.
    CHECK(UrlHasUserinfo("https://a@b@example.com/"));     // multiple '@' still flagged.

    // No userinfo -> not flagged. A '@' in the PATH or QUERY must not false-trigger.
    CHECK_FALSE(UrlHasUserinfo("https://example.com/"));
    CHECK_FALSE(UrlHasUserinfo("https://example.com:8080/path"));
    CHECK_FALSE(UrlHasUserinfo("https://example.com/path?to=a@b")); // '@' is past the authority.
    CHECK_FALSE(UrlHasUserinfo("https://example.com/u@ser"));       // '@' in path segment.
    CHECK_FALSE(UrlHasUserinfo("https://example.com#a@b"));         // '@' in fragment.
    CHECK_FALSE(UrlHasUserinfo("no-scheme@example.com"));           // no "://" sep -> not a URL.
    CHECK_FALSE(UrlHasUserinfo(""));
}

TEST_CASE("ExtractHostFromUrl strips userinfo to the real connect-host (security)" *
          doctest::test_suite("[high-risk]")) {
    // Defence-in-depth alongside UrlHasUserinfo: even if a userinfo URL reached
    // this helper, it must surface the REAL host (after the last '@') so the
    // allow-list judges the host cpr will actually dial -- never the userinfo.
    CHECK(ExtractHostFromUrl("https://allowed.example.net:x@evil.com/") == "evil.com");
    CHECK(ExtractHostFromUrl("https://user:pass@allowed.example.com/") == "allowed.example.com");
    CHECK(ExtractHostFromUrl("https://user@Example.COM/foo") == "example.com"); // lowercased after strip.
    CHECK(ExtractHostFromUrl("https://a@b@evil.com:443/") == "evil.com");       // last '@' wins; port dropped.
    CHECK(ExtractHostFromUrl("https://user@[::1]:8080/foo") == "::1");          // userinfo before bracketed IPv6.
    CHECK(ExtractHostFromUrl("https://user@/path") == "");                      // empty host after strip.
}

TEST_CASE("ExtractHostFromUrl returns empty for malformed inputs") {
    CHECK(ExtractHostFromUrl("no-scheme.com") == "");        // no "://" sep.
    CHECK(ExtractHostFromUrl("") == "");                     // empty.
    CHECK(ExtractHostFromUrl("https://") == "");             // scheme-only; hostStart >= size.
    CHECK(ExtractHostFromUrl("https:///path") == "");        // empty host part.
    CHECK(ExtractHostFromUrl("http://[unterminated") == ""); // missing closing bracket.
}

TEST_CASE("IsLoopbackAddress recognises canonical loopback forms (case + whitespace insensitive)" *
          doctest::test_suite("[high-risk]")) {
    // Forces McpJsonRpcPure.cpp IsLoopbackAddress ::trim+lowercase path AND the explicit
    // four-value set. Removing any of the literals breaks the matching assertion below.
    CHECK(IsLoopbackAddress(SmatchetDefaults::Mcp::kBindLocalhost)); // canonical "127.0.0.1".
    CHECK(IsLoopbackAddress("127.0.0.1"));
    CHECK(IsLoopbackAddress("::1"));
    CHECK(IsLoopbackAddress("localhost"));
    CHECK(IsLoopbackAddress("LOCALHOST"));
    CHECK(IsLoopbackAddress("LocalHost"));
    CHECK(IsLoopbackAddress("::ffff:127.0.0.1"));
    CHECK(IsLoopbackAddress("  127.0.0.1  ")); // trims surrounding whitespace.
    CHECK(IsLoopbackAddress("\tlocalhost\n"));
}

TEST_CASE("IsLoopbackAddress rejects non-loopback addresses") {
    CHECK_FALSE(IsLoopbackAddress(""));
    CHECK_FALSE(IsLoopbackAddress("   "));
    CHECK_FALSE(IsLoopbackAddress("127.0.0.2"));
    CHECK_FALSE(IsLoopbackAddress("192.168.1.1"));
    CHECK_FALSE(IsLoopbackAddress("10.0.0.1"));
    CHECK_FALSE(IsLoopbackAddress("example.com"));
    CHECK_FALSE(IsLoopbackAddress("0.0.0.0")); // bind-any is NOT loopback.
    CHECK_FALSE(IsLoopbackAddress("::2"));
}
