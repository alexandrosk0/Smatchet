#ifndef SMATCHET_COMMANDS_VIEW_COMMANDS_H
#define SMATCHET_COMMANDS_VIEW_COMMANDS_H

// Registers the view.* command group on the AppController's registry.
// Called from SmatchetUI once ViewState is loaded (line ~450 of SmatchetUI.cpp).
// Split out from BuiltinCommands because ViewState lives on SmatchetUI,
// not on AppController — this avoids adding a Views* accessor to AppController.

class AppController;
class Views;

namespace smatchet {
namespace cmd {

/// Idempotent: subsequent calls skip registration (HasExact check).
void RegisterViewCommands(AppController& app, Views& views);

}  // namespace cmd
}  // namespace smatchet

#endif  // SMATCHET_COMMANDS_VIEW_COMMANDS_H
