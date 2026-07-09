// MCP tools/call token-bucket rate limit (AGENTIC_INFRA_AUDIT.md B3) — pure-logic
// tests for the accept/deny decision shared by the REST and JSON-RPC routes. No
// sockets, no clocks: the caller-owned bucket state and monotonic timestamps are
// passed in directly.

#include "McpRateLimitPure.h"

#include <doctest/doctest.h>

using smatchet::mcp::pure::ConsumeToolsCallToken;
using smatchet::mcp::pure::McpRateLimitDecision;
using smatchet::mcp::pure::McpToolsCallBucket;

namespace {

McpToolsCallBucket FullBucket(int burst, long long nowMs) {
    McpToolsCallBucket b;
    b.Tokens = static_cast<double>(burst);
    b.LastRefillMs = nowMs;
    return b;
}

} // namespace

TEST_CASE("ConsumeToolsCallToken admits a full burst then denies the next call") {
    McpToolsCallBucket bucket = FullBucket(3, 1000);
    for (int i = 0; i < 3; ++i) {
        CHECK(ConsumeToolsCallToken(bucket, 3, 1.0, 1000).Allow);
    }
    const McpRateLimitDecision denied = ConsumeToolsCallToken(bucket, 3, 1.0, 1000);
    CHECK_FALSE(denied.Allow);
    CHECK(denied.RetryAfterMs > 0);
}

TEST_CASE("ConsumeToolsCallToken refills at the configured per-second rate") {
    McpToolsCallBucket bucket = FullBucket(1, 0);
    CHECK(ConsumeToolsCallToken(bucket, 1, 2.0, 0).Allow);
    CHECK_FALSE(ConsumeToolsCallToken(bucket, 1, 2.0, 0).Allow);
    // 2 tokens/s -> one whole token after 500 ms.
    CHECK(ConsumeToolsCallToken(bucket, 1, 2.0, 500).Allow);
    CHECK_FALSE(ConsumeToolsCallToken(bucket, 1, 2.0, 500).Allow);
}

TEST_CASE("ConsumeToolsCallToken caps refill at the burst capacity") {
    McpToolsCallBucket bucket = FullBucket(2, 0);
    // A long idle period must not bank more than `burst` tokens.
    CHECK(ConsumeToolsCallToken(bucket, 2, 1.0, 3600000).Allow);
    CHECK(ConsumeToolsCallToken(bucket, 2, 1.0, 3600000).Allow);
    CHECK_FALSE(ConsumeToolsCallToken(bucket, 2, 1.0, 3600000).Allow);
}

TEST_CASE("ConsumeToolsCallToken retry-after reflects the token deficit") {
    McpToolsCallBucket bucket = FullBucket(1, 0);
    CHECK(ConsumeToolsCallToken(bucket, 1, 1.0, 0).Allow);
    const McpRateLimitDecision denied = ConsumeToolsCallToken(bucket, 1, 1.0, 0);
    CHECK_FALSE(denied.Allow);
    // Empty bucket at 1 token/s: a whole token is ~1000 ms away.
    CHECK(denied.RetryAfterMs >= 1000);
    CHECK(denied.RetryAfterMs <= 1001);
}

TEST_CASE("ConsumeToolsCallToken non-positive burst or refill disables the limit") {
    McpToolsCallBucket bucket;
    CHECK(ConsumeToolsCallToken(bucket, 0, 5.0, 0).Allow);
    CHECK(ConsumeToolsCallToken(bucket, -1, 5.0, 0).Allow);
    CHECK(ConsumeToolsCallToken(bucket, 5, 0.0, 0).Allow);
    CHECK(ConsumeToolsCallToken(bucket, 5, -1.0, 0).Allow);
}

TEST_CASE("ConsumeToolsCallToken a backwards timestamp refills nothing") {
    McpToolsCallBucket bucket = FullBucket(1, 5000);
    CHECK(ConsumeToolsCallToken(bucket, 1, 100.0, 5000).Allow);
    // nowMs behind LastRefillMs must not grant free tokens.
    CHECK_FALSE(ConsumeToolsCallToken(bucket, 1, 100.0, 4000).Allow);
}
