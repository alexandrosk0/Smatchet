#pragma once

struct JiraConfig;
class AppController;
struct UiDrawSession;

#if defined(SMATCHET_WITH_MCP)
/** Read-only MCP server status, endpoints, and recent actions (Integrations owns editable fields). */
void SmatchetDrawMcpServerPanel(AppController& app, const JiraConfig& cfgOnDisk, UiDrawSession& d);

/** Floating window; toggled from Scripts → MCP Server (or MCP menu when Lua automation is off). */
void SmatchetDrawMcpServerWindow(AppController& app, UiDrawSession& d);
#endif
