// view.toggle.<id> commands — flip side-bar / panel visibility flags on the
// shared UiDrawSession. Mirrors the inline View menu items in
// SmatchetUI::drawMainMenuBar so the Command Palette, CLI, Lua, and MCP can
// open / close panels from any surface.
//
// The handlers mutate `g_ui` which is owned by the UI thread; every handler
// hops to the UI thread via RunOnUiThreadAsCommandResult.

#include "Commands/ViewToggleCommands.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "SmatchetUiSession.h"

#include <string>
#include <utility>

// Same singleton accessed by SmatchetUI.cpp — file-static definition lives
// there. Pulling it in via extern avoids adding a getter on AppController for
// what is essentially a bag of UI booleans.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

CommandResult ToggleFlag(AppController& app, bool UiDrawSession::* flag, void (*onOpen)(UiDrawSession&)) {
    return RunOnUiThreadAsCommandResult(app, [flag, onOpen]() {
        bool& v = g_ui.*flag;
        v = !v;
        if (v && onOpen) {
            onOpen(g_ui);
        }
        nlohmann::json out;
        out["open"] = v;
        return CommandResult::Success(std::move(out));
    });
}

void RegisterToggle(CommandRegistry& reg, AppController& app, const char* name, const char* label,
                    bool UiDrawSession::* flag, void (*onOpen)(UiDrawSession&)) {
    Command c;
    c.Name = name;
    c.Category = "view";
    c.Summary = std::string("Toggle ") + label + " panel visibility.";
    c.Destructive = false;
    c.Idempotent = false; // toggling flips state each call
    c.AsyncSafe = true;
    c.Handler = [&app, flag, onOpen](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
        return ToggleFlag(app, flag, onOpen);
    };
    reg.Register(std::move(c));
}

// --- Focus side-effects matching the inline View menu handlers -------------

void OnOpenViewsDashboard(UiDrawSession& d) { d.requestViewsDashboardFocus = true; }

void OnOpenAuditTrail(UiDrawSession& d) { d.requestAuditTrailFocus = true; }

void OnOpenAnnotate(UiDrawSession& d) {
    d.annotateTabVisible = true;
    d.activeGridTab = 1;
    d.activeGridTabForcePending = true;
    d.requestActiveProjectFocus = true;
}

#if defined(SMATCHET_WITH_MCP)
void OnOpenMcpServer(UiDrawSession& d) { d.requestMcpServerFocus = true; }
#endif

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
void OnOpenLuaAutomation(UiDrawSession& d) {
    d.requestLuaAutomationFocus = true;
    d.requestScriptingEditorTabFocus = true;
}
#endif

} // namespace

void RegisterViewToggleCommands(CommandRegistry& reg, AppController& app) {
    if (reg.HasExact("view.toggle.views_dashboard"))
        return;

    RegisterToggle(reg, app, "view.toggle.views_dashboard", "Views Dashboard", &UiDrawSession::showViewsDashboard,
                   &OnOpenViewsDashboard);
    RegisterToggle(reg, app, "view.toggle.source_blame", "Annotate", &UiDrawSession::showBlameAnalysis,
                   &OnOpenAnnotate);
    RegisterToggle(reg, app, "view.toggle.log", "Log", &UiDrawSession::showLogWindow, nullptr);
    RegisterToggle(reg, app, "view.toggle.backend_audit", "Backend Audit", &UiDrawSession::showAuditTrail,
                   &OnOpenAuditTrail);
    RegisterToggle(reg, app, "view.toggle.performance", "Performance", &UiDrawSession::showPerformance, nullptr);
    RegisterToggle(reg, app, "view.toggle.plan_doc_viewer", "Plan Docs", &UiDrawSession::showPlanDocViewer, nullptr);
    RegisterToggle(reg, app, "view.toggle.bulk_import", "Bulk Import", &UiDrawSession::showBulkImport, nullptr);
    RegisterToggle(reg, app, "view.toggle.bulk_export", "Bulk Export", &UiDrawSession::showBulkExport, nullptr);
    RegisterToggle(reg, app, "view.toggle.preferences", "Preferences", &UiDrawSession::showPreferences, nullptr);
#if defined(SMATCHET_WITH_MCP)
    RegisterToggle(reg, app, "view.toggle.mcp_server", "MCP Server", &UiDrawSession::showMcpServerWindow,
                   &OnOpenMcpServer);
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    RegisterToggle(reg, app, "view.toggle.scripts", "Scripts & Actions", &UiDrawSession::showLuaAutomationWindow,
                   &OnOpenLuaAutomation);
#endif
}

} // namespace cmd
} // namespace smatchet
