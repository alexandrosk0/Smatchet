// AiLuaPromptRateLimit — unit gate for the `ai.prompt(...)` Lua-glue rate
// limiter (security audit H5 / backlog E6). The instruction-count lua_sethook
// does NOT cover the outbound LLM HTTP call ai.prompt kicks off, so paste-and-
// run Lua could loop it to burn the API quota or stream ticket data off-host.
// The C++ glue consults DecideAiPromptGate before submitting; this test pins
// the decision table (re-entrancy + 5 s spacing) without sol2 / cpr / a clock.
//
// See Source/Core/include/AiLuaPromptRateLimit.h for the helper + rationale.

#include <doctest/doctest.h>

#include "AiLuaPromptRateLimit.h"

using smatchet::ai::AiPromptGateDecision;
using smatchet::ai::DecideAiPromptGate;
using smatchet::ai::kAiPromptMinIntervalMs;

TEST_CASE("DecideAiPromptGate: first call is always allowed") {
    // everCalled=false → no spacing gate (lastCallMs ignored). The first
    // prompt of a session goes through.
    CHECK(DecideAiPromptGate(/*inFlight=*/false, /*everCalled=*/false,
                             /*lastCallMs=*/0, /*nowMs=*/0) == AiPromptGateDecision::Allow);
    CHECK(DecideAiPromptGate(false, false, 999999, 0) == AiPromptGateDecision::Allow);
}

TEST_CASE("DecideAiPromptGate: re-entrant call is rejected regardless of timing") {
    // inFlight is checked FIRST — a prior turn still pending rejects even if the
    // elapsed time would otherwise pass the spacing rule. This closes the
    // tight-loop-while-pending hole the finding describes.
    CHECK(DecideAiPromptGate(/*inFlight=*/true, /*everCalled=*/true,
                             /*lastCallMs=*/0, /*nowMs=*/100000) == AiPromptGateDecision::RejectReentrant);
    // Even on the very first call, an (impossible-but-defensive) in-flight flag
    // rejects.
    CHECK(DecideAiPromptGate(true, false, 0, 0) == AiPromptGateDecision::RejectReentrant);
}

TEST_CASE("DecideAiPromptGate: two calls < 5 s apart -> second rejected") {
    const std::int64_t t0 = 1000000;
    // 0 ms later (same instant).
    CHECK(DecideAiPromptGate(false, true, t0, t0) == AiPromptGateDecision::RejectTooSoon);
    // Just under the window.
    CHECK(DecideAiPromptGate(false, true, t0, t0 + kAiPromptMinIntervalMs - 1) == AiPromptGateDecision::RejectTooSoon);
    // 1 ms in.
    CHECK(DecideAiPromptGate(false, true, t0, t0 + 1) == AiPromptGateDecision::RejectTooSoon);
}

TEST_CASE("DecideAiPromptGate: calls >= 5 s apart -> allowed") {
    const std::int64_t t0 = 1000000;
    // Exactly at the boundary (>= is allowed; the reject is strict <).
    CHECK(DecideAiPromptGate(false, true, t0, t0 + kAiPromptMinIntervalMs) == AiPromptGateDecision::Allow);
    // Well past.
    CHECK(DecideAiPromptGate(false, true, t0, t0 + kAiPromptMinIntervalMs + 1) == AiPromptGateDecision::Allow);
    CHECK(DecideAiPromptGate(false, true, t0, t0 + 60000) == AiPromptGateDecision::Allow);
}

TEST_CASE("DecideAiPromptGate: spacing window is 5000 ms") {
    // Pin the documented interval so a future maintainer can't silently relax it.
    CHECK(kAiPromptMinIntervalMs == 5000);
}
