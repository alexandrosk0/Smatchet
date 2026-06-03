#pragma once

// Pure builder for the MCP server panel's "info" text block, extracted from
// SmatchetDrawMcpServerPanel (function-size decomposition — drops the panel under the
// 30-branch cap). No ImGui / AppController dependency: it takes the on-disk config plus a
// nullable runtime-status snapshot and returns the formatted multiline string. Behaviour
// is byte-for-byte identical to the inline builder the monolith shipped, so the bucket-A
// test pins every config/runtime branch and endpoint line.

#include "ConfigManager.h"
#include "McpServerStatus.h"
#include "SmatchetDefaults.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace smatchet {
namespace mcp_ui {

inline void AppendInfoLine(std::string& out, const char* fmt, ...) {
    char stack[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    stack[sizeof(stack) - 1u] = '\0';
    out += stack;
    out.push_back('\n');
}

// Build the diagnostics info text. `status` is the runtime plugin-host snapshot, or
// nullptr when no runtime plugin host is registered (the "may require a full app restart"
// path). Mirrors the original inline string assembly exactly.
inline std::string BuildMcpServerInfoText(const TrackerConfig& cfgOnDisk, const McpServerStatus* status) {
    std::string info;
    info.reserve(2048);
    info += "Configured (saved on disk)\n";
    AppendInfoLine(info, "- Enabled: %s", cfgOnDisk.McpEnabled ? "yes" : "no");
    AppendInfoLine(info, "- Port: %d", cfgOnDisk.McpPort);
    AppendInfoLine(info, "- Bind: %s", cfgOnDisk.McpAllowRemote ? "0.0.0.0 (LAN)" : "127.0.0.1 (localhost only)");
    AppendInfoLine(info, "- Auth token: %s", cfgOnDisk.McpAuthToken.empty() ? "not set" : "set (hidden)");
    if (cfgOnDisk.McpAllowRemote && cfgOnDisk.McpAuthToken.empty()) {
        info += "\nWarning: LAN bind with no token only restricts by source IP in the MCP server; use a token for "
                "stronger access control.\n";
    }

    info += "\nRuntime (this process)\n";
    if (status == nullptr) {
        info += "No runtime plugin host is registered. Saving MCP under Preferences → Integrations may require a full "
                "app restart in this host.\n";
        return info;
    }

    const McpServerStatus& st = *status;
    const int cfgPortClamped = (cfgOnDisk.McpPort >= 1 && cfgOnDisk.McpPort <= 65535)
                                   ? cfgOnDisk.McpPort
                                   : SmatchetDefaults::Mcp::kDefaultPort;
    AppendInfoLine(info, "- MCP plugin loaded: %s", st.PluginRegistered ? "yes" : "no");
    AppendInfoLine(info, "- HTTP server running: %s", st.ServerRunning ? "yes" : "no");
    AppendInfoLine(info, "- Listen port: %d", st.ListenPort);
    AppendInfoLine(info, "- Bind host: %s", st.BindHost.empty() ? "(n/a)" : st.BindHost.c_str());
    AppendInfoLine(info, "- Routes installed: %s", st.RoutesInstalled ? "yes" : "no");
    AppendInfoLine(info, "- Listen thread joinable: %s", st.ThreadJoinable ? "yes" : "no");

    const std::string expectedBind =
        cfgOnDisk.McpAllowRemote ? SmatchetDefaults::Mcp::kBindAny : SmatchetDefaults::Mcp::kBindLocalhost;
    const bool cfgAuthSet = !cfgOnDisk.McpAuthToken.empty();
    if (cfgOnDisk.McpEnabled && st.PluginRegistered &&
        (st.ListenPort != cfgPortClamped || st.BindHost != expectedBind || st.AuthRequired != cfgAuthSet)) {
        info += "\nNote: Configured settings differ from the running server (port/bind/auth token presence). "
                "Save in Preferences → Integrations to apply a restart.\n";
    }
    if (cfgOnDisk.McpEnabled && !st.PluginRegistered) {
        info += "\nNote: MCP is enabled in config but the MCP plugin is not loaded. Restart the app or use a host "
                "build that registers the plugin host.\n";
    }
    if (cfgOnDisk.McpEnabled && st.PluginRegistered && !st.ServerRunning && !st.ThreadJoinable) {
        info += "\nNote: MCP plugin is present but the HTTP server is not running.\n";
    }

    const int runtimePort = (st.ListenPort >= 1 && st.ListenPort <= 65535) ? st.ListenPort : 0;
    const int urlPort = (st.PluginRegistered && runtimePort > 0) ? runtimePort : cfgPortClamped;
    char baseUrl[128];
    std::snprintf(baseUrl, sizeof(baseUrl), "http://%s:%d", SmatchetDefaults::Mcp::kBindLocalhost, urlPort);
    info += "\nEndpoints (base URL for local use)\n";
    AppendInfoLine(info, "%s", baseUrl);
    if (cfgOnDisk.McpAllowRemote) {
        info += "LAN bind: use this machine's LAN IP with the same port from other devices.\n";
    }
    char ep[192];
    std::snprintf(ep, sizeof(ep), "%s/mcp/list_tickets", baseUrl);
    AppendInfoLine(info, "- %s", ep);
    std::snprintf(ep, sizeof(ep), "%s/mcp/search?q=TEXT", baseUrl);
    AppendInfoLine(info, "- %s", ep);
    std::snprintf(ep, sizeof(ep), "%s/mcp/tools/list", baseUrl);
    AppendInfoLine(info, "- %s", ep);
    std::snprintf(ep, sizeof(ep), "%s/mcp/tools/call (POST)", baseUrl);
    AppendInfoLine(info, "- %s", ep);
    std::snprintf(ep, sizeof(ep), "%s%s", baseUrl, SmatchetDefaults::Mcp::kSsePath);
    AppendInfoLine(info, "- %s", ep);
    std::snprintf(ep, sizeof(ep), "%s/mcp/messages (POST)", baseUrl);
    AppendInfoLine(info, "- %s", ep);
    std::snprintf(ep, sizeof(ep), "%s/mcp/attachment_proxy?url=... (GET)", baseUrl);
    AppendInfoLine(info, "- %s", ep);
    return info;
}

} // namespace mcp_ui
} // namespace smatchet
