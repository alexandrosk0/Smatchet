// Finding DR12 — pure-logic coverage for the MCP spawn-token lockout fix (part a) and the CLI
// attach-path auth header (part b). No sockets, no HTTP, no plugin instance: exercises the
// extracted header-only decision helpers directly.
//
// Part (a): PlanMcpAuthToken / McpAuthTokenSatisfiesConfig back the McpPlugin::OnStart adoption
// and NeedsRestart decisions. The regression: after a --spawn child adopts SMATCHET_MCP_SPAWN_TOKEN
// and scrubs it from the env, a later SyncMcpPluginWithConfig restart re-ran OnStart with an empty
// env var and came back tokenless -- so every parent request 401'd. Retaining the adopted token
// (and treating it as the expected state in the restart decision) keeps the server authenticated.
//
// Part (b): McpAttachAuthHeader backs RunCmdAttachDispatch. The regression: the direct `cmd` attach
// path sent no X-Smatchet-Token, so an operator-token server 401'd (res set) and the --spawn
// fallback (gated on a failed connection) never engaged.

#include "CliCommandRunner.h"
#include "McpPlugin.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::cli::McpAttachAuthHeader;
using smatchet::mcp::McpAuthTokenPlan;
using smatchet::mcp::McpAuthTokenSatisfiesConfig;
using smatchet::mcp::PlanMcpAuthToken;

TEST_CASE("PlanMcpAuthToken adopts the spawn env token, retains it, and scrubs the env") {
    const McpAuthTokenPlan plan = PlanMcpAuthToken(/*configToken=*/"", /*envSpawnToken=*/"spawn-abc",
                                                   /*retainedSpawnToken=*/"");
    CHECK(plan.effectiveToken == "spawn-abc");
    CHECK(plan.retainedSpawnToken == "spawn-abc");
    CHECK(plan.scrubSpawnEnv);
}

TEST_CASE("PlanMcpAuthToken re-adopts the retained token when the env var is gone (restart)") {
    // Second OnStart of a spawned child: the env var was scrubbed on the first start, so only the
    // retained token remains. Without re-adoption the server would restart tokenless (the DR12a bug).
    const McpAuthTokenPlan plan = PlanMcpAuthToken(/*configToken=*/"", /*envSpawnToken=*/"",
                                                   /*retainedSpawnToken=*/"spawn-abc");
    CHECK(plan.effectiveToken == "spawn-abc");
    CHECK(plan.retainedSpawnToken == "spawn-abc");
    // Nothing was read from the env this time, so there is nothing to scrub.
    CHECK_FALSE(plan.scrubSpawnEnv);
}

TEST_CASE("PlanMcpAuthToken prefers a fresh env token over a stale retained one") {
    const McpAuthTokenPlan plan = PlanMcpAuthToken(/*configToken=*/"", /*envSpawnToken=*/"spawn-new",
                                                   /*retainedSpawnToken=*/"spawn-old");
    CHECK(plan.effectiveToken == "spawn-new");
    CHECK(plan.retainedSpawnToken == "spawn-new");
    CHECK(plan.scrubSpawnEnv);
}

TEST_CASE("PlanMcpAuthToken never overrides an operator-configured token") {
    const McpAuthTokenPlan plan = PlanMcpAuthToken(/*configToken=*/"operator-token",
                                                   /*envSpawnToken=*/"spawn-abc",
                                                   /*retainedSpawnToken=*/"spawn-prev");
    CHECK(plan.effectiveToken == "operator-token");
    // The operator token wins; the spawn env is left untouched (the adoption block never ran).
    CHECK_FALSE(plan.scrubSpawnEnv);
    // The prior retained token is passed through unchanged.
    CHECK(plan.retainedSpawnToken == "spawn-prev");
}

TEST_CASE("PlanMcpAuthToken yields an empty token when nothing is configured or provisioned") {
    const McpAuthTokenPlan plan = PlanMcpAuthToken(/*configToken=*/"", /*envSpawnToken=*/"",
                                                   /*retainedSpawnToken=*/"");
    CHECK(plan.effectiveToken.empty());
    CHECK(plan.retainedSpawnToken.empty());
    CHECK_FALSE(plan.scrubSpawnEnv);
}

TEST_CASE("McpAuthTokenSatisfiesConfig treats the retained spawn token as the expected state") {
    // Empty config + server running the adopted spawn token: NeedsRestart must NOT fire, otherwise
    // the restart drops the token (the DR12a lockout).
    CHECK(McpAuthTokenSatisfiesConfig(/*runningToken=*/"spawn-abc", /*configToken=*/"",
                                      /*retainedSpawnToken=*/"spawn-abc"));
}

TEST_CASE("McpAuthTokenSatisfiesConfig matches an exact operator token") {
    CHECK(McpAuthTokenSatisfiesConfig("operator-token", "operator-token", ""));
}

TEST_CASE("McpAuthTokenSatisfiesConfig reports a real divergence as needing restart") {
    // Operator changed / set a token that the running server does not carry -> restart warranted.
    CHECK_FALSE(McpAuthTokenSatisfiesConfig("spawn-abc", "operator-token", "spawn-abc"));
    // Running token differs from both the (empty) config and the retained spawn token.
    CHECK_FALSE(McpAuthTokenSatisfiesConfig("stale-token", "", "spawn-abc"));
    // Empty config, empty retained, but the server somehow carries a token -> not satisfied.
    CHECK_FALSE(McpAuthTokenSatisfiesConfig("stray-token", "", ""));
}

TEST_CASE("McpAttachAuthHeader emits the token header when a token is configured") {
    std::string name;
    std::string value;
    CHECK(McpAttachAuthHeader("operator-token", name, value));
    CHECK(name == "X-Smatchet-Token");
    CHECK(value == "operator-token");
}

TEST_CASE("McpAttachAuthHeader sends no header when no token is configured") {
    std::string name = "unset";
    std::string value = "unset";
    CHECK_FALSE(McpAttachAuthHeader("", name, value));
    // The out params are left untouched so the caller attaches no default header.
    CHECK(name == "unset");
    CHECK(value == "unset");
}
