// MCP Host/Origin DNS-rebinding defence (security synthesis #4) — pure-logic
// tests for the decision the Authorize path applies on loopback-bound servers.
// No sockets, no HTTP: exercises the extracted IsLoopbackHostHeader /
// IsAllowedMcpOrigin / IsMcpHostOriginAllowed helpers directly.

#include "McpJsonRpcPure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::mcp::pure::CanAcceptSseConnection;
using smatchet::mcp::pure::IsAllowedMcpOrigin;
using smatchet::mcp::pure::IsLoopbackHostHeader;
using smatchet::mcp::pure::IsMcpHostOriginAllowed;
using smatchet::mcp::pure::kMaxConcurrentSseConnections;

TEST_CASE("IsLoopbackHostHeader accepts loopback literals with/without port") {
    CHECK(IsLoopbackHostHeader("127.0.0.1"));
    CHECK(IsLoopbackHostHeader("127.0.0.1:42360"));
    CHECK(IsLoopbackHostHeader("localhost"));
    CHECK(IsLoopbackHostHeader("localhost:42360"));
    CHECK(IsLoopbackHostHeader("LocalHost:8080"));      // case-folded.
    CHECK(IsLoopbackHostHeader("  127.0.0.1:42360  ")); // surrounding whitespace trimmed.

    // Bracketed IPv6 loopback, with and without port.
    CHECK(IsLoopbackHostHeader("[::1]"));
    CHECK(IsLoopbackHostHeader("[::1]:42360"));
}

TEST_CASE("IsLoopbackHostHeader rejects rebind / forged Host values (fail closed)") {
    // The classic DNS-rebind arrival: attacker hostname in Host, browser connected to 127.0.0.1.
    CHECK_FALSE(IsLoopbackHostHeader("attacker.example.com"));
    CHECK_FALSE(IsLoopbackHostHeader("attacker.example.com:42360"));

    // Empty Host -> reject.
    CHECK_FALSE(IsLoopbackHostHeader(""));
    CHECK_FALSE(IsLoopbackHostHeader("   "));

    // Trailing-dot FQDN forms resolve to loopback but are not literals a legit client sends.
    CHECK_FALSE(IsLoopbackHostHeader("127.0.0.1."));
    CHECK_FALSE(IsLoopbackHostHeader("localhost."));
    CHECK_FALSE(IsLoopbackHostHeader("localhost.:42360"));

    // Look-alikes that must not match the literal set.
    CHECK_FALSE(IsLoopbackHostHeader("127.0.0.2"));
    CHECK_FALSE(IsLoopbackHostHeader("localhost.attacker.com"));
    CHECK_FALSE(IsLoopbackHostHeader("notlocalhost"));

    // Unterminated bracket -> malformed -> reject.
    CHECK_FALSE(IsLoopbackHostHeader("[::1"));
}

TEST_CASE("IsAllowedMcpOrigin accepts absent/null/loopback and rejects cross-origin") {
    // Legitimate local clients: no Origin at all.
    CHECK(IsAllowedMcpOrigin(""));
    CHECK(IsAllowedMcpOrigin("   "));

    // Sandboxed / file origins serialize to the literal "null".
    CHECK(IsAllowedMcpOrigin("null"));
    CHECK(IsAllowedMcpOrigin("NULL")); // case-folded.

    // Loopback origins (http/https, with/without port, IPv6) are fine.
    CHECK(IsAllowedMcpOrigin("http://127.0.0.1:42360"));
    CHECK(IsAllowedMcpOrigin("https://localhost"));
    CHECK(IsAllowedMcpOrigin("http://[::1]:42360"));

    // Cross-origin browser request carrying the attacker's Origin -> reject.
    CHECK_FALSE(IsAllowedMcpOrigin("https://attacker.example.com"));
    CHECK_FALSE(IsAllowedMcpOrigin("http://evil.test:8080"));

    // Non-http schemes and garbage -> reject.
    CHECK_FALSE(IsAllowedMcpOrigin("file://127.0.0.1"));
    CHECK_FALSE(IsAllowedMcpOrigin("ftp://localhost"));
    CHECK_FALSE(IsAllowedMcpOrigin("not-an-origin"));

    // Trailing-dot loopback Origin must not slip past.
    CHECK_FALSE(IsAllowedMcpOrigin("http://localhost."));
}

TEST_CASE("IsMcpHostOriginAllowed combines checks and reports a reason on reject") {
    std::string reason;

    // Loopback Host + no Origin: the legitimate-local-client path.
    reason.clear();
    CHECK(IsMcpHostOriginAllowed("127.0.0.1:42360", "", reason));
    CHECK(reason.empty()); // untouched on accept.

    // Loopback Host + loopback Origin: also fine.
    reason.clear();
    CHECK(IsMcpHostOriginAllowed("localhost", "http://127.0.0.1:42360", reason));

    // Non-loopback Host short-circuits with bad_host.
    reason.clear();
    CHECK_FALSE(IsMcpHostOriginAllowed("attacker.example.com", "", reason));
    CHECK(reason == "bad_host");

    // Loopback Host but cross-origin Origin -> bad_origin.
    reason.clear();
    CHECK_FALSE(IsMcpHostOriginAllowed("127.0.0.1:42360", "https://attacker.example.com", reason));
    CHECK(reason == "bad_origin");
}

// Concurrent-SSE bound (security synthesis #20): the MCP server admits at most
// kMaxConcurrentSseConnections SSE streams so an unbounded number cannot exhaust the
// fixed httplib worker pool. CanAcceptSseConnection is the pure admit/reject decision.
TEST_CASE("CanAcceptSseConnection admits below the cap and rejects at/above it") {
    CHECK(CanAcceptSseConnection(0));
    CHECK(CanAcceptSseConnection(kMaxConcurrentSseConnections - 1)); // last admissible slot.
    CHECK_FALSE(CanAcceptSseConnection(kMaxConcurrentSseConnections));
    CHECK_FALSE(CanAcceptSseConnection(kMaxConcurrentSseConnections + 1));
    CHECK_FALSE(CanAcceptSseConnection(-1)); // corrupted counter fails closed.
}

// SSE CORS policy: never a wildcard. Echo the Authorize-vetted Origin on the loopback bind
// only; on the 0.0.0.0 bind (Host/Origin gate intentionally skipped) emit no CORS header at
// all, so an arbitrary web origin can never read the token-authenticated SSE stream.
TEST_CASE("ResolveSseCorsOrigin echoes a vetted Origin only on the loopback bind") {
    using smatchet::mcp::pure::ResolveSseCorsOrigin;

    // Loopback bind: echo exactly the request Origin (already vetted by Authorize).
    CHECK(ResolveSseCorsOrigin("http://127.0.0.1:42360", true) == "http://127.0.0.1:42360");
    CHECK(ResolveSseCorsOrigin("null", true) == "null");

    // No Origin header (native MCP client): no CORS header on either bind.
    CHECK(ResolveSseCorsOrigin("", true).empty());
    CHECK(ResolveSseCorsOrigin("", false).empty());

    // Remote bind: no grant, whatever the Origin claims to be.
    CHECK(ResolveSseCorsOrigin("http://127.0.0.1:42360", false).empty());
    CHECK(ResolveSseCorsOrigin("https://attacker.example.com", false).empty());

    // Defence in depth: a non-loopback Origin gets no grant even on the loopback bind
    // (Authorize would have 403'd it already; the pure function re-vets regardless).
    CHECK(ResolveSseCorsOrigin("https://attacker.example.com", true).empty());
}
