#include "SmatchetMcpServerUi.h"

#if defined(SMATCHET_WITH_MCP)

#include "AppController.h"
#include "ConfigManager.h"
#include "McpServerStatus.h"
#include "PluginHost.h"
#include "SmatchetUiSession.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

void AppendLine(std::string& out, const char* fmt, ...) {
    char stack[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    stack[sizeof(stack) - 1u] = '\0';
    out += stack;
    out.push_back('\n');
}

/** Read-only multiline so the user can drag-select and Ctrl+C (plain Text/BulletText is not selectable). */
void DrawCopyableMultiline(const char* id, const std::string& text, float heightPx, std::vector<char>& scratch) {
    const size_t need = text.size() + 1u;
    if (scratch.size() < need) {
        scratch.resize(need + 1024u);
    }
    if (!text.empty()) {
        std::memcpy(scratch.data(), text.data(), text.size());
    }
    scratch[text.size()] = '\0';
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_ReadOnly;
    ImGui::InputTextMultiline(id, scratch.data(), static_cast<int>(scratch.size()), ImVec2(-1.0f, heightPx), flags);
}

void SaveMcpWindowLayoutDebounced(UiDrawSession& d) {
    static std::chrono::steady_clock::time_point s_lastWrite{};
    static float s_infoH = std::numeric_limits<float>::quiet_NaN();
    static float s_actH = 0.f;

    const auto differs = [](float a, float b) { return std::fabs(a - b) > 0.75f; };

    const bool haveSnap = !std::isnan(s_infoH);
    const bool diff = !haveSnap || differs(d.cfg.McpServerInfoPanelHeightPx, s_infoH) ||
                      differs(d.cfg.McpServerActivityPanelHeightPx, s_actH);
    if (!diff) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (haveSnap && now - s_lastWrite < std::chrono::milliseconds(350)) {
        return;
    }
    s_lastWrite = now;
    s_infoH = d.cfg.McpServerInfoPanelHeightPx;
    s_actH = d.cfg.McpServerActivityPanelHeightPx;
    ConfigManager::Save(d.cfg);
}

} // namespace

void SmatchetDrawMcpServerPanel(AppController& app, const TrackerConfig& cfgOnDisk, UiDrawSession& d) {
    static std::vector<char> s_infoScratch;
    static std::vector<char> s_logScratch;

    ImGui::TextUnformatted("Select text below and use Ctrl+C to copy.");
    ImGui::Spacing();

    std::string info;
    info.reserve(2048);
    info += "Configured (saved on disk)\n";
    AppendLine(info, "- Enabled: %s", cfgOnDisk.McpEnabled ? "yes" : "no");
    AppendLine(info, "- Port: %d", cfgOnDisk.McpPort);
    AppendLine(info, "- Bind: %s", cfgOnDisk.McpAllowRemote ? "0.0.0.0 (LAN)" : "127.0.0.1 (localhost only)");
    AppendLine(info, "- Auth token: %s", cfgOnDisk.McpAuthToken.empty() ? "not set" : "set (hidden)");
    if (cfgOnDisk.McpAllowRemote && cfgOnDisk.McpAuthToken.empty()) {
        info += "\nWarning: LAN bind with no token only restricts by source IP in the MCP server; use a token for "
                "stronger access control.\n";
    }

    info += "\nRuntime (this process)\n";
    ::PluginHost* ph = app.RuntimePluginHost();
    if (ph == nullptr) {
        info += "No runtime plugin host is registered. Saving MCP under Preferences → Integrations may require a full "
                "app restart in this host.\n";
    } else {
        const McpServerStatus st = ph->GetMcpServerStatus();
        const int cfgPortClamped =
            (cfgOnDisk.McpPort >= 1 && cfgOnDisk.McpPort <= 65535) ? cfgOnDisk.McpPort : 8080;
        AppendLine(info, "- MCP plugin loaded: %s", st.PluginRegistered ? "yes" : "no");
        AppendLine(info, "- HTTP server running: %s", st.ServerRunning ? "yes" : "no");
        AppendLine(info, "- Listen port: %d", st.ListenPort);
        AppendLine(info, "- Bind host: %s", st.BindHost.empty() ? "(n/a)" : st.BindHost.c_str());
        AppendLine(info, "- Routes installed: %s", st.RoutesInstalled ? "yes" : "no");
        AppendLine(info, "- Listen thread joinable: %s", st.ThreadJoinable ? "yes" : "no");

        const std::string expectedBind = cfgOnDisk.McpAllowRemote ? "0.0.0.0" : "127.0.0.1";
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
        std::snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%d", urlPort);
        info += "\nEndpoints (base URL for local use)\n";
        AppendLine(info, "%s", baseUrl);
        if (cfgOnDisk.McpAllowRemote) {
            info += "LAN bind: use this machine's LAN IP with the same port from other devices.\n";
        }
        char ep[192];
        std::snprintf(ep, sizeof(ep), "%s/mcp/list_tickets", baseUrl);
        AppendLine(info, "- %s", ep);
        std::snprintf(ep, sizeof(ep), "%s/mcp/search?q=TEXT", baseUrl);
        AppendLine(info, "- %s", ep);
        std::snprintf(ep, sizeof(ep), "%s/mcp/tools/list", baseUrl);
        AppendLine(info, "- %s", ep);
        std::snprintf(ep, sizeof(ep), "%s/mcp/tools/call (POST)", baseUrl);
        AppendLine(info, "- %s", ep);
        std::snprintf(ep, sizeof(ep), "%s/mcp/sse", baseUrl);
        AppendLine(info, "- %s", ep);
        std::snprintf(ep, sizeof(ep), "%s/mcp/messages (POST)", baseUrl);
        AppendLine(info, "- %s", ep);
        std::snprintf(ep, sizeof(ep), "%s/mcp/attachment_proxy?url=... (GET)", baseUrl);
        AppendLine(info, "- %s", ep);
    }

    const float line = ImGui::GetTextLineHeightWithSpacing();
    float infoH = d.cfg.McpServerInfoPanelHeightPx;
    if (infoH < 80.0f) {
        infoH = line * 18.0f;
    }
    DrawCopyableMultiline("##mcp_server_info", info, infoH, s_infoScratch);

    ImGui::Spacing();
    const float splitGrabH = 7.0f;
    ImGui::InvisibleButton("##mcp_panel_split", ImVec2(ImGui::GetContentRegionAvail().x, splitGrabH));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        float next = d.cfg.McpServerInfoPanelHeightPx;
        if (next < 80.0f) {
            next = infoH;
        }
        next += ImGui::GetIO().MouseDelta.y;
        next = (std::max)(120.0f, (std::min)(900.0f, next));
        if (std::fabs(next - d.cfg.McpServerInfoPanelHeightPx) > 0.5f) {
            d.cfg.McpServerInfoPanelHeightPx = next;
            SaveMcpWindowLayoutDebounced(d);
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Recent actions");
    const std::vector<std::string> log = app.CopyMcpActivityLog();
    std::string logText;
    if (log.empty()) {
        logText = "No MCP activity yet.\n";
    } else {
        logText.reserve(log.size() * 64u);
        for (const std::string& lineEntry : log) {
            logText += lineEntry;
            logText.push_back('\n');
        }
    }
    float actH = d.cfg.McpServerActivityPanelHeightPx;
    actH = (std::max)(80.0f, (std::min)(480.0f, actH));
    d.cfg.McpServerActivityPanelHeightPx = actH;
    DrawCopyableMultiline("##mcp_activity_log", logText, actH, s_logScratch);

    ImGui::TextDisabled("Edit MCP fields in Preferences → Integrations.");
}

void SmatchetDrawMcpServerWindow(AppController& app, UiDrawSession& d) {
    if (!d.showMcpServerWindow) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(480.0f, 520.0f), ImGuiCond_FirstUseEver);

    const bool wantFocus = d.requestMcpServerFocus;
    if (wantFocus) {
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin("MCP Server", &d.showMcpServerWindow)) {
        ImGui::End();
        if (wantFocus) {
            d.requestMcpServerFocus = false;
        }
        return;
    }
    if (wantFocus) {
        ImGui::SetWindowFocus();
        d.requestMcpServerFocus = false;
    }

    SmatchetDrawMcpServerPanel(app, d.cfg, d);

    SaveMcpWindowLayoutDebounced(d);

    ImGui::End();
}

#endif // SMATCHET_WITH_MCP






