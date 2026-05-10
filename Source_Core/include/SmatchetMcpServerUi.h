#pragma once

struct TrackerConfig;
class AppController;
struct UiDrawSession;

#if defined(SMATCHET_WITH_MCP)
/** Read-only MCP server status, endpoints, and recent actions (Integrations owns editable fields). */
void SmatchetDrawMcpServerPanel(const AppController& app, const TrackerConfig& cfgOnDisk, UiDrawSession& d);

/** Floating window; toggled from Automation -> Agent Bridge (MCP).... */
void SmatchetDrawMcpServerWindow(const AppController& app, UiDrawSession& d);
#endif





