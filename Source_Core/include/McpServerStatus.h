#pragma once

#include <string>

/** Read-only snapshot of the MCP HTTP plugin for UI / diagnostics. */
struct McpServerStatus {
    /** True when this snapshot came from a live `McpPlugin` instance. */
    bool PluginRegistered = false;
    /** `httplib::Server::is_running()` — accept loop active. */
    bool ServerRunning = false;
    int ListenPort = 0;
    std::string BindHost;
    /** True when `X-Smatchet-Token` is required for all clients. */
    bool AuthRequired = false;
    bool RoutesInstalled = false;
    bool ThreadJoinable = false;
};
