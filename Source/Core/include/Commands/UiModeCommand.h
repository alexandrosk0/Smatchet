#ifndef SMATCHET_COMMANDS_UI_MODE_COMMAND_H
#define SMATCHET_COMMANDS_UI_MODE_COMMAND_H

// ui.mode command — read or set the persisted UI layout-mode preference
// (desktop | mobile | auto), or `cycle` to advance Desktop→Mobile→Auto→Desktop.
// Unlike the bool view.toggle.* group this takes a `mode` arg, so it is a small
// hand-rolled command rather than a RegisterToggle. The handler mutates
// g_ui.cfg.UiMode on the UI thread and marks prefs dirty; drawResolveUiMode picks
// the change up next frame. Registered from RegisterBuiltinCommands.

class IMainThreadPoster;

namespace smatchet {
namespace cmd {

class CommandRegistry;

void RegisterUiModeCommand(CommandRegistry& registry, IMainThreadPoster& app);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_UI_MODE_COMMAND_H
