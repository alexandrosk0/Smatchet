#ifndef SMATCHET_COMMANDS_VIEW_TOGGLE_COMMANDS_H
#define SMATCHET_COMMANDS_VIEW_TOGGLE_COMMANDS_H

// view.toggle.<id> command group — one entry per side-bar View / Panel.
// Each command flips the matching `g_ui.show*` bool and replays the same
// focus side-effects the inline View menu items in SmatchetUI::drawMainMenuBar
// apply. Registered from RegisterBuiltinCommands so a single entry point still
// owns the cross-cutting catalogue. Idempotent on repeat calls.

class IMainThreadPoster;

namespace smatchet {
namespace cmd {

class CommandRegistry;

void RegisterViewToggleCommands(CommandRegistry& registry, IMainThreadPoster& app);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_VIEW_TOGGLE_COMMANDS_H
