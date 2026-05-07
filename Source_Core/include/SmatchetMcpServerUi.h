#pragma once

struct TrackerConfig;
class AppController;
struct UiDrawSession;

#if defined(SMATCHET_WITH_MCP)
/** Read-only MCP server status, endpoints, and recent actions (Integrations owns editable fields). */
void SmatchetDrawMcpServerPanel(AppController& app, const TrackerConfig& cfgOnDisk, UiDrawSession& d);

/** Floating window; toggled from Windows → MCP Server (or MCP menu when Lua automation is off). */
void SmatchetDrawMcpServerWindow(AppController& app, UiDrawSession& d);
#endif






