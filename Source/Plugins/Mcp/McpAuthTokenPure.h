#ifndef SMATCHET_PLUGINS_MCP_MCP_AUTH_TOKEN_PURE_H
#define SMATCHET_PLUGINS_MCP_MCP_AUTH_TOKEN_PURE_H

// Pure MCP spawn-token adoption + restart-decision policy (finding DR12a). Kept
// free of IPlugin / httplib so it can be unit-tested without pulling the full
// McpPlugin class (whose virtual overrides are gated behind the MCP build).

#include <string>

namespace smatchet {
namespace mcp {

/// Outcome of the MCP spawn-token adoption decision at (re)start (finding DR12a).
struct McpAuthTokenPlan {
    std::string effectiveToken;     ///< value the running server's auth token should take
    std::string retainedSpawnToken; ///< spawn token to persist for a later restart re-adoption
    bool scrubSpawnEnv = false;     ///< whether SMATCHET_MCP_SPAWN_TOKEN was read this start and should be unset
};

/// Pure spawn-token adoption policy (finding DR12a). An ephemeral --spawn child has no
/// persisted mcp_auth_token yet adopts the parent's per-spawn SMATCHET_MCP_SPAWN_TOKEN and
/// scrubs it from the env. Because the env var is gone on a later restart, the token must be
/// retained in plugin state so a re-adoption keeps the server authenticated instead of coming
/// back tokenless and 401ing every parent request. An operator-configured token always wins and
/// is never overridden. `retainedSpawnToken` is the token adopted on a prior start (empty on the
/// first). Returns the token to run with, the token to persist, and whether to scrub the env.
inline McpAuthTokenPlan PlanMcpAuthToken(const std::string& configToken, const std::string& envSpawnToken,
                                         const std::string& retainedSpawnToken) {
    McpAuthTokenPlan plan;
    plan.retainedSpawnToken = retainedSpawnToken;
    if (!configToken.empty()) {
        plan.effectiveToken = configToken;
        return plan;
    }
    const std::string spawnToken = !envSpawnToken.empty() ? envSpawnToken : retainedSpawnToken;
    plan.effectiveToken = spawnToken;
    plan.retainedSpawnToken = spawnToken;
    plan.scrubSpawnEnv = !envSpawnToken.empty();
    return plan;
}

/// Pure restart-decision helper (finding DR12a). Answers whether the running server's token
/// already satisfies the config, so NeedsRestart does not force a spurious tokenless restart.
/// An exact match with the configured token satisfies it; so does an empty config when the
/// server is running the retained spawn token (that adopted token is the expected state, not
/// empty). Any other divergence means a real restart is warranted.
inline bool McpAuthTokenSatisfiesConfig(const std::string& runningToken, const std::string& configToken,
                                        const std::string& retainedSpawnToken) {
    if (runningToken == configToken) {
        return true;
    }
    return configToken.empty() && !retainedSpawnToken.empty() && runningToken == retainedSpawnToken;
}

} // namespace mcp
} // namespace smatchet

#endif // SMATCHET_PLUGINS_MCP_MCP_AUTH_TOKEN_PURE_H
