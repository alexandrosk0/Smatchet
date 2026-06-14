#pragma once

// Pure decision helper for the `ai.prompt(...)` Lua-glue rate limiter
// (security audit H5 / backlog E6). The instruction-count `lua_sethook`
// (LUA_MASKCOUNT) bounds *Lua* execution but does NOT cover the C++-side
// outbound LLM HTTP call that `ai.prompt` kicks off — so paste-and-run Lua
// could call it in a tight loop to burn the API quota or stream ticket data
// off-host. This helper is the gate the glue consults before submitting.
// Two reject conditions:
//   1. re-entrancy — a prior `ai.prompt` turn is still in flight (its HTTP
//      call has not been observed to complete on this gate), so a fresh call
//      from the same instance is rejected outright.
//   2. spacing — the call lands < kMinIntervalMs after the previous accepted
//      call's timestamp.
// Free-function + plain integer-millis args (NOT a clock type) so the unit
// test (`AiLuaPromptRateLimit.test.cpp`) can exercise every branch without a
// real clock, sol2, or cpr. The caller (AppController::Impl) owns the
// per-instance last-call timestamp + in-flight flag and converts its
// steady_clock readings to millis before calling here.

#include <cstdint>

namespace smatchet {
namespace ai {

/// Minimum spacing between two accepted `ai.prompt` calls, in milliseconds.
/// 5 s per the audit fix. Anything closer is rejected.
constexpr std::int64_t kAiPromptMinIntervalMs = 5000;

enum class AiPromptGateDecision {
    Allow,
    RejectReentrant, ///< a prior turn is still in flight
    RejectTooSoon,   ///< spaced < kAiPromptMinIntervalMs from the last call
};

/// Pure rate-limit decision.
///   `inFlight`      — true when a prior `ai.prompt` turn has not completed.
///   `everCalled`    — false until the first accepted call this session
///                     (so the first call is never gated on spacing).
///   `lastCallMs`    — steady-clock millis of the previous accepted call
///                     (ignored when `everCalled` is false).
///   `nowMs`         — steady-clock millis of the current call.
/// Re-entrancy is checked first: an in-flight turn rejects regardless of
/// elapsed time (closes the tight-loop-while-pending hole).
inline AiPromptGateDecision DecideAiPromptGate(bool inFlight, bool everCalled, std::int64_t lastCallMs,
                                               std::int64_t nowMs) {
    if (inFlight) {
        return AiPromptGateDecision::RejectReentrant;
    }
    if (!everCalled) {
        return AiPromptGateDecision::Allow;
    }
    if (nowMs - lastCallMs < kAiPromptMinIntervalMs) {
        return AiPromptGateDecision::RejectTooSoon;
    }
    return AiPromptGateDecision::Allow;
}

} // namespace ai
} // namespace smatchet
