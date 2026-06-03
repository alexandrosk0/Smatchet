// Pure-logic coverage of BuildMcpServerInfoText, extracted from SmatchetDrawMcpServerPanel
// (function-size decomposition — drops the panel under the 30-branch cap). The builder is a
// byte-for-byte extraction of the inline string assembly, so these cases pin every config /
// runtime branch and the endpoint lines the monolith shipped.

#include "McpServerInfoTextPure.h"

#include <doctest/doctest.h>

#include <string>

using smatchet::mcp_ui::BuildMcpServerInfoText;

namespace {

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

TrackerConfig MakeCfg(bool enabled, int port, bool allowRemote, const std::string& token) {
    TrackerConfig cfg;
    cfg.McpEnabled = enabled;
    cfg.McpPort = port;
    cfg.McpAllowRemote = allowRemote;
    cfg.McpAuthToken = token;
    return cfg;
}

} // namespace

TEST_CASE("BuildMcpServerInfoText null runtime host -> restart note, no endpoints") {
    const TrackerConfig cfg = MakeCfg(true, 42360, false, "");
    const std::string info = BuildMcpServerInfoText(cfg, nullptr);

    CHECK(Contains(info, "Configured (saved on disk)"));
    CHECK(Contains(info, "- Enabled: yes"));
    CHECK(Contains(info, "- Port: 42360"));
    CHECK(Contains(info, "- Bind: 127.0.0.1 (localhost only)"));
    CHECK(Contains(info, "- Auth token: not set"));
    CHECK(Contains(info, "Runtime (this process)"));
    CHECK(Contains(info, "No runtime plugin host is registered."));
    // The endpoints block is gated on a live host — absent here.
    CHECK_FALSE(Contains(info, "Endpoints (base URL for local use)"));
    CHECK_FALSE(Contains(info, "/mcp/list_tickets"));
}

TEST_CASE("BuildMcpServerInfoText LAN bind with no token warns") {
    const TrackerConfig cfg = MakeCfg(true, 42360, true, "");
    const std::string info = BuildMcpServerInfoText(cfg, nullptr);
    CHECK(Contains(info, "- Bind: 0.0.0.0 (LAN)"));
    CHECK(Contains(info, "Warning: LAN bind with no token"));
}

TEST_CASE("BuildMcpServerInfoText LAN bind with token does not warn") {
    const TrackerConfig cfg = MakeCfg(true, 42360, true, "secret");
    const std::string info = BuildMcpServerInfoText(cfg, nullptr);
    CHECK(Contains(info, "- Auth token: set (hidden)"));
    CHECK_FALSE(Contains(info, "Warning: LAN bind with no token"));
}

TEST_CASE("BuildMcpServerInfoText running server emits endpoints from runtime port") {
    const TrackerConfig cfg = MakeCfg(true, 42360, false, "");
    McpServerStatus st;
    st.PluginRegistered = true;
    st.ServerRunning = true;
    st.ListenPort = 42360;
    st.BindHost = "127.0.0.1";
    st.RoutesInstalled = true;
    st.ThreadJoinable = true;
    st.AuthRequired = false;
    const std::string info = BuildMcpServerInfoText(cfg, &st);

    CHECK(Contains(info, "- MCP plugin loaded: yes"));
    CHECK(Contains(info, "- HTTP server running: yes"));
    CHECK(Contains(info, "- Listen port: 42360"));
    CHECK(Contains(info, "- Bind host: 127.0.0.1"));
    CHECK(Contains(info, "Endpoints (base URL for local use)"));
    CHECK(Contains(info, "http://127.0.0.1:42360"));
    CHECK(Contains(info, "http://127.0.0.1:42360/mcp/list_tickets"));
    CHECK(Contains(info, "http://127.0.0.1:42360/mcp/sse"));
    CHECK(Contains(info, "http://127.0.0.1:42360/mcp/attachment_proxy?url=... (GET)"));
    // Aligned config vs runtime -> no "settings differ" note.
    CHECK_FALSE(Contains(info, "Configured settings differ from the running server"));
}

TEST_CASE("BuildMcpServerInfoText config/runtime mismatch emits differ note") {
    const TrackerConfig cfg = MakeCfg(true, 42360, false, "");
    McpServerStatus st;
    st.PluginRegistered = true;
    st.ServerRunning = true;
    st.ListenPort = 9999; // differs from configured 42360
    st.BindHost = "127.0.0.1";
    const std::string info = BuildMcpServerInfoText(cfg, &st);
    CHECK(Contains(info, "Configured settings differ from the running server"));
    // URL uses the runtime port when the plugin is registered and the port is valid.
    CHECK(Contains(info, "http://127.0.0.1:9999"));
}

TEST_CASE("BuildMcpServerInfoText enabled but plugin not loaded note") {
    const TrackerConfig cfg = MakeCfg(true, 42360, false, "");
    McpServerStatus st;
    st.PluginRegistered = false;
    const std::string info = BuildMcpServerInfoText(cfg, &st);
    CHECK(Contains(info, "MCP is enabled in config but the MCP plugin is not loaded."));
    // Invalid/zero runtime port falls back to the clamped configured port for the URL.
    CHECK(Contains(info, "http://127.0.0.1:42360"));
}

TEST_CASE("BuildMcpServerInfoText invalid configured port clamps to default") {
    const TrackerConfig cfg = MakeCfg(true, 70000, false, ""); // out of [1,65535]
    McpServerStatus st;
    st.PluginRegistered = false;
    const std::string info = BuildMcpServerInfoText(cfg, &st);
    // Configured line still shows the raw out-of-range value...
    CHECK(Contains(info, "- Port: 70000"));
    // ...but the endpoint URL uses the clamped default (42360).
    CHECK(Contains(info, "http://127.0.0.1:42360"));
}

TEST_CASE("BuildMcpServerInfoText plugin present but server down note") {
    const TrackerConfig cfg = MakeCfg(true, 42360, false, "");
    McpServerStatus st;
    st.PluginRegistered = true;
    st.ServerRunning = false;
    st.ThreadJoinable = false;
    const std::string info = BuildMcpServerInfoText(cfg, &st);
    CHECK(Contains(info, "MCP plugin is present but the HTTP server is not running."));
}
