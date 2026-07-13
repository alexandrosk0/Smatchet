// view.toggle.<id> commands — flip side-bar / panel visibility flags on the
// shared UiDrawSession. Mirrors the inline View menu items in
// SmatchetUI::drawMainMenuBar so the Command Palette, CLI, Lua, and MCP can
// open / close panels from any surface.
// The handlers mutate `g_ui` which is owned by the UI thread; every handler
// hops to the UI thread via RunOnUiThreadAsCommandResult.

#include "Commands/ViewToggleCommands.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "SmatchetUiSession.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

// Same singleton accessed by SmatchetUI.cpp — file-static definition lives
// there. Pulling it in via extern avoids adding a getter on AppController for
// what is essentially a bag of UI booleans.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

// `action`: "show" forces open, "hide" forces closed, anything else (default
// "toggle") flips. `onOpen` fires on any open (matches the inline View-menu focus
// side-effects). `focusLatch`, when set, is raised ONLY on action=="show" — so a
// plain menu/palette toggle does not steal window focus, while an explicit reveal
// shortcut (Ctrl+Shift+F etc.) does. This keeps the pre-migration behavior exact.
CommandResult ToggleFlag(IMainThreadPoster& app, bool UiDrawSession::*flag, void (*onOpen)(UiDrawSession&),
                         bool UiDrawSession::*focusLatch, const std::string& action) {
    return RunOnUiThreadAsCommandResult(app, [flag, onOpen, focusLatch, action]() {
        bool& v = g_ui.*flag;
        bool open;
        if (action == "show") {
            open = true;
        } else if (action == "hide") {
            open = false;
        } else {
            open = !v; // default "toggle"
        }
        v = open;
        if (open) {
            if (onOpen) {
                onOpen(g_ui);
            }
            if (action == "show" && focusLatch) {
                g_ui.*focusLatch = true;
            }
        }
        nlohmann::json out;
        out["open"] = v;
        return CommandResult::Success(std::move(out));
    });
}

void RegisterToggle(CommandRegistry& reg, IMainThreadPoster& app, const char* name, const char* label,
                    bool UiDrawSession::*flag, void (*onOpen)(UiDrawSession&),
                    bool UiDrawSession::*focusLatch = nullptr, const char* alias = nullptr) {
    Command c;
    c.Name = name;
    c.Category = "view";
    c.Summary = std::string("Toggle ") + label + " panel visibility.";
    // Short display label — the registry seam UI surfaces (Recently Used Views menu,
    // palette rows) resolve instead of showing the raw command id.
    c.Title = label;
    if (alias != nullptr) {
        c.Aliases.push_back(alias);
    }
    c.Destructive = false;
    c.Idempotent = false; // toggling flips state each call
    c.AsyncSafe = true;
    c.Handler = [&app, flag, onOpen, focusLatch](const nlohmann::json& args, const CommandContext& /*ctx*/) {
        const std::string action =
            args.is_object() ? args.value("action", std::string("toggle")) : std::string("toggle");
        return ToggleFlag(app, flag, onOpen, focusLatch, action);
    };
    reg.Register(std::move(c));
}

// --- Focus side-effects matching the inline View menu handlers -------------

void OnOpenViewsDashboard(UiDrawSession& d) { d.requestViewsDashboardFocus = true; }

void OnOpenAuditTrail(UiDrawSession& d) { d.requestAuditTrailFocus = true; }

void OnOpenNotificationCenter(UiDrawSession& d) { d.requestNotificationCenterFocus = true; }

void OnOpenAnnotate(UiDrawSession& d) { d.showAnnotateAnalysis = true; }

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

void RegisterViewToggleCommands(CommandRegistry& reg, IMainThreadPoster& app) {
    if (reg.HasExact("view.toggle.views_dashboard"))
        return;

    RegisterToggle(reg, app, "view.toggle.views_dashboard", "Views Dashboard", &UiDrawSession::showViewsDashboard,
                   &OnOpenViewsDashboard);
    RegisterToggle(reg, app, "view.toggle.source_annotate", "Annotate", &UiDrawSession::showAnnotateAnalysis,
                   &OnOpenAnnotate);
    RegisterToggle(reg, app, "view.toggle.log", "Log", &UiDrawSession::showLogWindow, nullptr);
    // Canonical id follows the view.toggle.* naming of its siblings; the bare
    // `notifications` name survives as an alias (ergonomic CLI/MCP/Lua shorthand + saved
    // keybindings from configs written before the rename keep dispatching).
    RegisterToggle(reg, app, "view.toggle.notifications", "Notifications", &UiDrawSession::showNotificationCenterWindow,
                   &OnOpenNotificationCenter, &UiDrawSession::requestNotificationCenterFocus, "notifications");
    RegisterToggle(reg, app, "view.toggle.backend_audit", "Backend Audit", &UiDrawSession::showAuditTrail,
                   &OnOpenAuditTrail);
    RegisterToggle(reg, app, "view.toggle.performance", "Performance", &UiDrawSession::showPerformance, nullptr,
                   &UiDrawSession::requestPerformanceFocus);
    RegisterToggle(reg, app, "view.toggle.plan_doc_viewer", "Plan Docs", &UiDrawSession::showPlanDocViewer, nullptr,
                   &UiDrawSession::requestPlanDocViewerFocus);
    RegisterToggle(reg, app, "view.toggle.bulk_import", "Bulk Import", &UiDrawSession::showBulkImport, nullptr,
                   &UiDrawSession::requestBulkImportFocus);
    RegisterToggle(reg, app, "view.toggle.bulk_export", "Bulk Export", &UiDrawSession::showBulkExport, nullptr,
                   &UiDrawSession::requestBulkExportFocus);
    RegisterToggle(reg, app, "view.toggle.preferences", "Preferences", &UiDrawSession::showPreferences, nullptr,
                   &UiDrawSession::requestPreferencesFocus);
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
