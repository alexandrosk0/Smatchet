#ifndef SMATCHET_COMMANDS_APP_VIEW_COMMANDS_H
#define SMATCHET_COMMANDS_APP_VIEW_COMMANDS_H

// App-level view + chrome commands that the pre-registry keyboard shortcuts used to
// drive inline from SmatchetUI: side-bar / bottom-panel visibility (persisted cfg
// slots), the AI assistant reveal, full-screen toggle, dock-debug overlay, and the
// "Log a Bug" opener. Registered from RegisterBuiltinCommands so the unified command
// registry (and therefore CLI / Palette / MCP / Lua / the rebindable shortcut table)
// can drive them from any surface. Handlers mutate `g_ui` on the UI thread.
// See docs/plans/shipped/keyboard-shortcuts-rebindable.md.

class IMainThreadPoster;

namespace smatchet {
namespace cmd {

class CommandRegistry;

void RegisterAppViewCommands(CommandRegistry& registry, IMainThreadPoster& app);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_APP_VIEW_COMMANDS_H
