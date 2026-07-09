#ifndef SMATCHET_PLUGINS_MCP_MCP_RATE_LIMIT_PURE_H
#define SMATCHET_PLUGINS_MCP_MCP_RATE_LIMIT_PURE_H

// Pure token-bucket policy for the MCP tools/call rate limit (AGENTIC_INFRA_AUDIT.md B3).
// Kept free of IPlugin / httplib / clocks so the doctest rig can exercise it like
// CanAcceptSseConnection; the caller owns the bucket state, its mutex, and the
// monotonic clock that produces `nowMs`.

namespace smatchet {
namespace mcp {
namespace pure {

/// Token-bucket state for the tools/call limiter. Seed `Tokens` with the configured
/// burst capacity and `LastRefillMs` with the current monotonic timestamp at server
/// start so the first burst is admitted at full capacity.
struct McpToolsCallBucket {
    double Tokens = 0.0;
    long long LastRefillMs = 0;
};

/// Accept/deny decision. `RetryAfterMs` is meaningful only when `Allow` is false: the
/// minimum wait until one whole token has refilled (always >= 1 ms on deny).
struct McpRateLimitDecision {
    bool Allow = false;
    long long RetryAfterMs = 0;
};

/// Consume one token for a tools/call. `burst` is the bucket capacity (maximum saved-up
/// calls), `refillPerSec` the sustained rate; a non-positive value for either disables
/// the limit (always allow — the operator opted out). `nowMs` must come from a monotonic
/// clock; a timestamp that went backwards refills nothing (fails toward limiting rather
/// than granting free tokens).
inline McpRateLimitDecision ConsumeToolsCallToken(McpToolsCallBucket& bucket, int burst, double refillPerSec,
                                                  long long nowMs) {
    McpRateLimitDecision decision;
    if (burst <= 0 || refillPerSec <= 0.0) {
        decision.Allow = true;
        return decision;
    }
    const long long elapsedMs = nowMs - bucket.LastRefillMs;
    if (elapsedMs > 0) {
        bucket.Tokens += (static_cast<double>(elapsedMs) / 1000.0) * refillPerSec;
    }
    if (bucket.Tokens > static_cast<double>(burst)) {
        bucket.Tokens = static_cast<double>(burst);
    }
    bucket.LastRefillMs = nowMs;
    if (bucket.Tokens >= 1.0) {
        bucket.Tokens -= 1.0;
        decision.Allow = true;
        return decision;
    }
    decision.RetryAfterMs = static_cast<long long>(((1.0 - bucket.Tokens) * 1000.0) / refillPerSec) + 1;
    return decision;
}

} // namespace pure
} // namespace mcp
} // namespace smatchet

#endif // SMATCHET_PLUGINS_MCP_MCP_RATE_LIMIT_PURE_H
