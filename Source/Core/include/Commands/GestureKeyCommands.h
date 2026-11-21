#ifndef SMATCHET_COMMANDS_GESTURE_KEY_COMMANDS_H
#define SMATCHET_COMMANDS_GESTURE_KEY_COMMANDS_H

// Keyboard equivalents for pointer gestures (docs/plans/input-ux-w2-gesture-keys.md).
// Five commands act on whichever list or splitter has keyboard focus (the drag-reorder, Delete-button and
// splitter-drag gestures): ui.list.move_up, ui.list.move_down and ui.list.remove for a list row, ui.splitter.grow
// and ui.splitter.shrink for a divider. They post a one-shot request (Ui/GestureKeys.h) that only the focused
// widget takes. Two more, view.panel.grow and view.panel.shrink, resize the bottom panel or a side bar: they
// take the panel by name, or the one holding the focused window.

class IMainThreadPoster;

namespace smatchet {
namespace cmd {

class CommandRegistry;

void RegisterGestureKeyCommands(CommandRegistry& registry, IMainThreadPoster& app);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_GESTURE_KEY_COMMANDS_H
