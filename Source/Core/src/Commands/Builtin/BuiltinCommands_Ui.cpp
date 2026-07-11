// ui.* / grid.* interaction commands — font-size zoom, "open view" palette
// shortcut, and grid selection clear. These are the app-global, rebindable
// registry commands behind the menu-bar shortcuts (Zoom In/Out/Reset, Open
// View, Clear Selection); see docs/plans/keybindings-menu-shortcuts-fix.md.
// Like ViewToggleCommands.cpp the handlers mutate `g_ui` (UI-thread-owned), so
// every effect hops to the UI thread via RunOnUiThreadAsCommandResult.
// Strict-lint zone (Commands/): LOG_* only, no raw new/delete, obj["k"]=v.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"

// fan-in Phase 6: every app use in this TU is the IMainThreadPoster upcast for the UI-thread
// hop, so it takes the narrow poster ref and drops AppController.h entirely.
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.
#include "ConfigManager.h"
#include "SmatchetUiSession.h"

#include <string>
#include <utility>

// Same g_ui singleton accessed by SmatchetUI.cpp / ViewToggleCommands.cpp —
// file-static definition lives in SmatchetUI.cpp. extern avoids a getter on
// AppController for what is a bag of UI state.
extern UiDrawSession g_ui;

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;

namespace {

// Per-frame zoom: the renderer derives ImGui's FontGlobalScale from
// cfg.FontSizePt (no atlas rebuild), so a zoom command only needs to set the
// field and persist it. `reset` snaps to the shared default; otherwise `delta`
// nudges the current value. Clamped to the legible range via the single
// SmatchetDefaults::kFontSize*Pt source of truth (shared with config load +
// the menu enable-gates).
CommandResult AdjustFontSize(IMainThreadPoster& poster, int delta, bool reset) {
    return RunOnUiThreadAsCommandResult(poster, [delta, reset]() {
        int pt = reset ? SmatchetDefaults::kFontSizeDefaultPt : g_ui.cfg.FontSizePt + delta;
        if (pt < SmatchetDefaults::kFontSizeMinPt) {
            pt = SmatchetDefaults::kFontSizeMinPt;
        }
        if (pt > SmatchetDefaults::kFontSizeMaxPt) {
            pt = SmatchetDefaults::kFontSizeMaxPt;
        }
        g_ui.cfg.FontSizePt = pt;
        ConfigManager::Save(g_ui.cfg);
        nlohmann::json out;
        out["fontSizePt"] = g_ui.cfg.FontSizePt;
        return CommandResult::Success(std::move(out));
    });
}

} // namespace

void RegisterUiInteractionCommands(CommandRegistry& reg, IMainThreadPoster& poster) {
    if (reg.HasExact("ui.zoom.in")) {
        return;
    }

    {
        Command c = MakeCommand("ui.zoom.in", "Increase the UI font size by one point (max 32).",
                                [&poster](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
                                    return AdjustFontSize(poster, +1, /*reset=*/false);
                                });
        c.Destructive = false;
        c.Idempotent = false; // clamps at 32, but each call nudges until then
        c.AsyncSafe = true;
        reg.Register(std::move(c));
    }
    {
        Command c = MakeCommand("ui.zoom.out", "Decrease the UI font size by one point (min 8).",
                                [&poster](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
                                    return AdjustFontSize(poster, -1, /*reset=*/false);
                                });
        c.Destructive = false;
        c.Idempotent = false;
        c.AsyncSafe = true;
        reg.Register(std::move(c));
    }
    {
        Command c = MakeCommand("ui.zoom.reset", "Reset the UI font size to the default (16).",
                                [&poster](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
                                    return AdjustFontSize(poster, 0, /*reset=*/true);
                                });
        c.Destructive = false;
        c.Idempotent = true; // always lands on the default
        c.AsyncSafe = true;
        reg.Register(std::move(c));
    }
    {
        Command c =
            MakeCommand("ui.open_view", "Open the command palette pre-filtered to the view.toggle.* panel commands.",
                        [&poster](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
                            return RunOnUiThreadAsCommandResult(poster, []() {
                                g_ui.requestCommandPaletteOpen = true;
                                g_ui.requestCommandPaletteFilter = "view.toggle.";
                                return CommandResult::Success({{"opened", true}});
                            });
                        });
        c.Destructive = false;
        c.Idempotent = false; // raises a one-frame open latch
        c.AsyncSafe = true;
        reg.Register(std::move(c));
    }
    {
        Command c = MakeCommand("grid.clear_selection", "Clear the focused grid pane's rectangular selection.",
                                [&poster](const nlohmann::json& /*args*/, const CommandContext& /*ctx*/) {
                                    return RunOnUiThreadAsCommandResult(poster, []() {
                                        g_ui.focusedPane().gridState.RectSel.ClearAll();
                                        return CommandResult::Success({{"cleared", true}});
                                    });
                                });
        c.Destructive = false;
        c.Idempotent = true; // clearing an empty selection is a no-op
        c.AsyncSafe = true;
        reg.Register(std::move(c));
    }
}

} // namespace cmd
} // namespace smatchet
