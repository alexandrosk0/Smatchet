// ui.mode — read or set the persisted UI layout-mode preference.
// `mode` arg ∈ {desktop, mobile, auto, cycle}; omitted → query (returns current
// without mutating). `cycle` advances Desktop→Mobile→Auto→Desktop. The handler
// mutates g_ui.cfg.UiMode (UI-thread owned) and marks prefs dirty so the 100 ms
// debounce persists it; SmatchetUI::drawResolveUiMode reads the new value next
// frame. Mirrors ViewToggleCommands.cpp's UI-thread-hop contract.

#include "Commands/UiModeCommand.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "SmatchetUiModeIds.h"
#include "SmatchetUiSession.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

// Same singleton ViewToggleCommands.cpp uses — file-static definition lives in
// SmatchetUI.cpp, declared extern in SmatchetUiSession.h.

namespace smatchet {
namespace cmd {

namespace {

UiMode NextUiMode(UiMode m) {
    switch (m) {
    case UiMode::Desktop:
        return UiMode::Mobile;
    case UiMode::Mobile:
        return UiMode::Auto;
    case UiMode::Auto:
        return UiMode::Desktop;
    }
    return UiMode::Desktop;
}

} // namespace

void RegisterUiModeCommand(CommandRegistry& reg, IMainThreadPoster& app) {
    if (reg.HasExact("ui.mode"))
        return;

    Command c;
    c.Name = "ui.mode";
    c.Category = "ui";
    c.Summary = "Get or set the UI layout mode (desktop|mobile|auto|cycle).";
    c.Destructive = false;
    c.Idempotent = false; // cycle / set are stateful; query is a no-op read
    c.AsyncSafe = true;
    c.Params = {[] {
        ParamSpec p;
        p.Name = "mode";
        p.Type = ParamType::String;
        p.Required = false;
        p.Description = "desktop | mobile | auto | cycle. Omit to query the current mode.";
        return p;
    }()};
    c.Handler = [&app](const nlohmann::json& args, const CommandContext&) {
        const std::string mode = args.value("mode", std::string());
        return RunOnUiThreadAsCommandResult(app, [mode]() {
            // Empty → query only; never mutates or marks prefs dirty.
            if (mode.empty()) {
                nlohmann::json out;
                out["mode"] = uiModeToString(g_ui.cfg.UiMode);
                return CommandResult::Success(std::move(out));
            }

            UiMode target;
            if (mode == "cycle") {
                target = NextUiMode(g_ui.cfg.UiMode);
            } else if (mode == "desktop" || mode == "mobile" || mode == "auto") {
                target = uiModeFromString(mode);
            } else {
                return CommandResult::Failure(ErrorCode::ValidationError, "Unknown ui mode '" + mode + "'.",
                                              "Use desktop, mobile, auto, or cycle.");
            }

            g_ui.cfg.UiMode = target;
            MarkPrefsDirty(g_ui);

            nlohmann::json out;
            out["mode"] = uiModeToString(target);
            return CommandResult::Success(std::move(out));
        });
    };
    reg.Register(std::move(c));
}

} // namespace cmd
} // namespace smatchet
