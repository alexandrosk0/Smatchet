// Keyboard equivalents for pointer gestures (see GestureKeyCommands.h). Every handler hops to the UI
// thread: the latch and the dock tree are UI-thread state.

#include "Commands/GestureKeyCommands.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "SmatchetUiSession.h"
#include "Ui/GestureKeys.h"
#include "Ui/SmatchetBottomPanelDrag.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

// Same singleton accessed by SmatchetUI.cpp — file-static definition lives there.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

namespace {

void RegisterFocusScoped(CommandRegistry& reg, IMainThreadPoster& app, const char* name, const char* title,
                         const char* summary, ui::GestureKey key) {
    Command c;
    c.Name = name;
    c.Category = "ui";
    c.Title = title;
    c.Summary = summary;
    c.Description = std::string(summary) +
                    "\nActs only on the list or splitter that has keyboard focus; with none focused, nothing "
                    "happens.\nReturns: {\"posted\": true}.";
    c.Destructive = false;
    c.Idempotent = false; // each call is one more step
    c.AsyncSafe = true;
    c.Handler = [&app, key](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
        return RunOnUiThreadAsCommandResult(app, [key]() {
            ui::PostGestureKey(key);
            nlohmann::json out;
            out["posted"] = true;
            return CommandResult::Success(std::move(out));
        });
    };
    reg.Register(std::move(c));
}

bool ParsePanelTarget(const std::string& name, SmatchetBottomPanelDrag::PanelTarget& out) {
    if (name.empty() || name == "focused") {
        out = SmatchetBottomPanelDrag::PanelTarget::Focused;
    } else if (name == "bottom") {
        out = SmatchetBottomPanelDrag::PanelTarget::Bottom;
    } else if (name == "primary") {
        out = SmatchetBottomPanelDrag::PanelTarget::Primary;
    } else if (name == "secondary") {
        out = SmatchetBottomPanelDrag::PanelTarget::Secondary;
    } else {
        return false;
    }
    return true;
}

void RegisterPanelNudge(CommandRegistry& reg, IMainThreadPoster& app, const char* name, const char* title,
                        const char* summary, float direction) {
    Command c;
    c.Name = name;
    c.Category = "view";
    c.Title = title;
    c.Summary = summary;
    c.Description = std::string(summary) +
                    "\nOne step is two lines of the UI font. The size is clamped and saved with the layout, as a "
                    "splitter drag is.\nReturns: {\"resized\": true, \"panel\": \"bottom|primary|secondary\", "
                    "\"extent\": <px>}, or "
                    "{\"resized\": false, \"reason\": ...} when \"focused\" finds no panel.";
    c.Destructive = false;
    c.Idempotent = false;
    c.AsyncSafe = true;
    {
        ParamSpec p;
        p.Name = "panel";
        p.Type = ParamType::String;
        p.Description = "Which panel: \"focused\" (the one holding the focused window), \"bottom\", "
                        "\"primary\" or \"secondary\".";
        p.Default = std::make_shared<nlohmann::json>("focused");
        p.Enum = {"focused", "bottom", "primary", "secondary"};
        c.Params.push_back(std::move(p));
    }
    c.Handler = [&app, direction](const nlohmann::json& args, const CommandContext& /*ctx*/) {
        const std::string panel =
            args.is_object() ? args.value("panel", std::string("focused")) : std::string("focused");
        SmatchetBottomPanelDrag::PanelTarget target = SmatchetBottomPanelDrag::PanelTarget::Focused;
        if (!ParsePanelTarget(panel, target)) {
            return CommandResult::Failure(ErrorCode::ValidationError,
                                          "panel must be one of: focused, bottom, primary, secondary.");
        }
        return RunOnUiThreadAsCommandResult(app, [target, direction]() {
            const float step = SmatchetBottomPanelDrag::KeyboardNudgeStepPx();
            const SmatchetBottomPanelDrag::PanelNudgeResult r =
                SmatchetBottomPanelDrag::NudgePanelExtent(g_ui, target, direction * step);
            if (!r.Ok && r.NoFocusedPanel) {
                // The default chord with the keyboard in the grid or a dialog: nothing to resize is the normal
                // case, not a failure worth a warning on every press and key repeat.
                nlohmann::json none;
                none["resized"] = false;
                none["reason"] = r.Error;
                return CommandResult::Success(std::move(none));
            }
            if (!r.Ok) {
                return CommandResult::Failure(ErrorCode::ValidationError, r.Error);
            }
            nlohmann::json out;
            out["resized"] = true;
            out["panel"] = r.Panel;
            out["extent"] = r.ExtentPx;
            return CommandResult::Success(std::move(out));
        });
    };
    reg.Register(std::move(c));
}

} // namespace

void RegisterGestureKeyCommands(CommandRegistry& reg, IMainThreadPoster& app) {
    if (reg.HasExact("ui.list.move_up")) {
        return;
    }
    RegisterFocusScoped(reg, app, "ui.list.move_up", "Move Row Up",
                        "Move the selected row of the focused list up one place.", ui::GestureKey::ListMoveUp);
    RegisterFocusScoped(reg, app, "ui.list.move_down", "Move Row Down",
                        "Move the selected row of the focused list down one place.", ui::GestureKey::ListMoveDown);
    RegisterFocusScoped(reg, app, "ui.list.remove", "Remove Row",
                        "Remove the selected row of the focused list, as its Delete button does.",
                        ui::GestureKey::ListRemove);
    RegisterFocusScoped(reg, app, "ui.splitter.grow", "Grow First Pane",
                        "Move the divider of the splitter in the focused window down or right.",
                        ui::GestureKey::SplitterGrow);
    RegisterFocusScoped(reg, app, "ui.splitter.shrink", "Shrink First Pane",
                        "Move the divider of the splitter in the focused window up or left.",
                        ui::GestureKey::SplitterShrink);
    RegisterPanelNudge(reg, app, "view.panel.grow", "Grow Panel",
                       "Make the bottom panel or a side bar bigger by one step.", 1.0f);
    RegisterPanelNudge(reg, app, "view.panel.shrink", "Shrink Panel",
                       "Make the bottom panel or a side bar smaller by one step.", -1.0f);
}

} // namespace cmd
} // namespace smatchet
