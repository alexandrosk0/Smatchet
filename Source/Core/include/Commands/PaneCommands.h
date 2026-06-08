#ifndef SMATCHET_COMMANDS_PANE_COMMANDS_H
#define SMATCHET_COMMANDS_PANE_COMMANDS_H

// Registers the pane.* command group on the AppController's registry (multi-grid-tabs
// Slice 4, plan item 20). Mirrors RegisterViewCommands: the grid-pane state lives on
// the UiDrawSession singleton (g_ui), not on AppController, so SmatchetUI passes the
// live session in once panes are loaded. Registering here surfaces pane scripting on
// all five command frontends (CLI / Palette / MCP / Lua / Scenarios) for free.
//
// Every handler mutates UI-thread-owned pane state through the SAME request latches the
// "+" / close-X / focus-cycle UI uses (paneAddRequestSourceId, pane.open, focusedPaneId
// + gridPaneFocusReassigned) — never resizing/reordering d.gridPanes mid-frame. The
// host (SmatchetUI::drawGridPaneWindows) drains those latches at known points.

class AppController;
struct UiDrawSession;

namespace smatchet {
namespace cmd {

/// Idempotent: subsequent calls skip registration (HasExact check).
void RegisterPaneCommands(AppController& app, UiDrawSession& session);

}  // namespace cmd
}  // namespace smatchet

#endif  // SMATCHET_COMMANDS_PANE_COMMANDS_H
