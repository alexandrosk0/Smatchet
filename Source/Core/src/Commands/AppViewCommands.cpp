// App-level view + chrome command handlers (see AppViewCommands.h). These replicate,
// behind the unified command registry, the effects the hardcoded keyboard shortcuts
// used to apply inline in SmatchetUI. Every handler mutates `g_ui` (UI-thread owned)
// and so hops to the UI thread via RunOnUiThreadAsCommandResult.

#include "Commands/AppViewCommands.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "Config/ConfigManager.h"
#include "SmatchetUiSession.h"
#include "Ui/SmatchetViewVisibility.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

// Same singleton accessed by SmatchetUI.cpp — file-static definition lives there.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

// "show" forces open, "hide" forces closed, anything else (default) toggles.
std::string ReadAction(const nlohmann::json& args) {
    return args.is_object() ? args.value("action", std::string("toggle")) : std::string("toggle");
}

bool ResolveTarget(const std::string& action, bool current) {
    if (action == "show") {
        return true;
    }
    if (action == "hide") {
        return false;
    }
    return !current; // default "toggle"
}

// Side-bar / bottom-panel visibility — persisted cfg slots (was: handlePanelVisibilityShortcuts).
CommandResult SetPanel(IMainThreadPoster& app, ViewSlot slot, bool TrackerConfig::*cfgFlag, const std::string& action) {
    return RunOnUiThreadAsCommandResult(app, [slot, cfgFlag, action]() {
        const bool target = ResolveTarget(action, g_ui.cfg.*cfgFlag);
        SetViewVisible(g_ui.cfg, slot, target);
        ConfigManager::Save(g_ui.cfg);
        nlohmann::json out;
        out["open"] = target;
        return CommandResult::Success(std::move(out));
    });
}

void RegisterPanel(CommandRegistry& reg, IMainThreadPoster& app, const char* name, const char* label, ViewSlot slot,
                   bool TrackerConfig::*cfgFlag) {
    Command c;
    c.Name = name;
    c.Category = "view";
    c.Summary = std::string("Toggle ") + label + " visibility.";
    c.Destructive = false;
    c.Idempotent = false; // default action toggles
    c.AsyncSafe = true;
    c.Handler = [&app, slot, cfgFlag](const nlohmann::json& args, const CommandContext& /*ctx*/) {
        return SetPanel(app, slot, cfgFlag, ReadAction(args));
    };
    reg.Register(std::move(c));
}

} // namespace

void RegisterAppViewCommands(CommandRegistry& reg, IMainThreadPoster& app) {
    if (reg.HasExact("view.sidebar.primary")) {
        return;
    }

    RegisterPanel(reg, app, "view.sidebar.primary", "primary side bar", ViewSlot::PrimarySideBar,
                  &TrackerConfig::ShowPrimarySideBar);
    RegisterPanel(reg, app, "view.sidebar.secondary", "secondary side bar", ViewSlot::SecondarySideBar,
                  &TrackerConfig::ShowSecondarySideBar);
    RegisterPanel(reg, app, "view.panel.bottom", "bottom panel", ViewSlot::BottomPanel, &TrackerConfig::ShowPanel);

#if defined(SMATCHET_WITH_AI)
    {
        // Always-reveal: re-pressing while open focuses, never closes (assistant contract).
        Command c;
        c.Name = "view.assistant";
        c.Category = "view";
        c.Summary = "Reveal and focus the Smatchet Assistant side panel.";
        c.Destructive = false;
        c.Idempotent = true;
        c.AsyncSafe = true;
        c.Handler = [&app](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
            return RunOnUiThreadAsCommandResult(app, []() {
                g_ui.assistantPanelOpen = true;
                g_ui.requestAssistantFocus = true;
                nlohmann::json out;
                out["open"] = true;
                return CommandResult::Success(std::move(out));
            });
        };
        reg.Register(std::move(c));
    }
#endif

#ifndef SMATCHET_EMBEDDED_IN_UNREAL
    {
        Command c;
        c.Name = "app.fullscreen.toggle";
        c.Category = "app";
        c.Summary = "Toggle borderless full-screen (standalone window only).";
        c.Destructive = false;
        c.Idempotent = false;
        c.AsyncSafe = true;
        c.Handler = [&app](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
            return RunOnUiThreadAsCommandResult(app, []() {
                g_ui.requestFullScreenToggle = true;
                return CommandResult::Success(nlohmann::json::object());
            });
        };
        reg.Register(std::move(c));
    }
#endif

    {
        Command c;
        c.Name = "app.dock_debug.toggle";
        c.Category = "app";
        c.Summary = "Toggle the dockspace debug overlay.";
        c.Destructive = false;
        c.Idempotent = false;
        c.AsyncSafe = true;
        c.Handler = [&app](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
            return RunOnUiThreadAsCommandResult(app, []() {
                g_ui.showDockDebug = !g_ui.showDockDebug;
                nlohmann::json out;
                out["open"] = g_ui.showDockDebug;
                return CommandResult::Success(std::move(out));
            });
        };
        reg.Register(std::move(c));
    }

    {
        Command c;
        c.Name = "app.bug_report.open";
        c.Category = "app";
        c.Summary = "Open the Log-a-Bug report window.";
        c.Destructive = false;
        c.Idempotent = true;
        c.AsyncSafe = true;
        c.Handler = [&app](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
            return RunOnUiThreadAsCommandResult(app, []() {
                g_ui.showBugReport = true;
                g_ui.bugReportOpenLatch = true;
                g_ui.bugReportInclScreenshot = g_ui.cfg.BugReportScreenshotDefault;
                return CommandResult::Success(nlohmann::json::object());
            });
        };
        reg.Register(std::move(c));
    }
}

} // namespace cmd
} // namespace smatchet
