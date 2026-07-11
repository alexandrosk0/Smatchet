#ifndef SMATCHET_COMMANDS_PANE_COMMANDS_H
#define SMATCHET_COMMANDS_PANE_COMMANDS_H

// Registers the pane.* command group on the AppController's registry (multi-grid-tabs
// Slice 4, plan item 20). Mirrors RegisterViewCommands: the grid-pane state lives on
// the UiDrawSession singleton (g_ui), not on AppController, so SmatchetUI passes the
// live session in once panes are loaded. Registering here surfaces pane scripting on
// all five command frontends (CLI / Palette / MCP / Lua / Scenarios) for free.
// Every handler mutates UI-thread-owned pane state through the SAME request latches the
// "+" / close-X / focus-cycle UI uses (paneAddRequestSourceId, pane.open, focusedPaneId
// + gridPaneFocusReassigned) — never resizing/reordering d.gridPanes mid-frame. The
// host (SmatchetUI::drawGridPaneWindows) drains those latches at known points.

#include "GridPane.h"

#include <string>

class IAppCommands;
class IMainThreadPoster;
struct UiDrawSession;
struct TrackerConfig;

namespace smatchet {
namespace cmd {

/// Idempotent: subsequent calls skip registration (HasExact check).
void RegisterPaneCommands(IAppCommands& app, IMainThreadPoster& poster, UiDrawSession& session);

namespace detail {

/// Outcome of the pane.new / pane.duplicate / pane.split arm-only-on-success decision.
/// Ok == true means Request is fully resolved and the caller should latch it; Ok == false
/// means a credential / validation check failed and the caller must NOT arm the latch
/// (issue #1458: a failure return that left the latch armed spawned a spurious duplicate
/// pane next frame). FailureMessage carries the user-facing reason on the failure path.
struct PaneAddDecision {
    bool Ok = true;
    PaneAddRequest Request;
    std::string FailureMessage;
};

/// Pure decision for the pane-add command family. Validates the optional backend arg
/// against `cfg` and resolves the request into PaneAddDecision::Request WITHOUT touching
/// any UI state — so the handler arms the deferred-create latch ONLY on the success path.
/// `sourceId` is the focused pane id; `acceptBackend` is true for pane.new (the only verb
/// that takes backend/view args). `backendArg` empty = same-backend duplicate (unchanged
/// behaviour). SQLite/ImGui-free → unit-testable.
PaneAddDecision DecidePaneAddRequest(const std::string& sourceId, bool acceptBackend, const std::string& backendArg,
                                     const std::string& viewArg, const TrackerConfig& cfg);

} // namespace detail

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_PANE_COMMANDS_H
