#ifndef SMATCHET_COMMANDS_BUILTIN_COMMANDS_H
#define SMATCHET_COMMANDS_BUILTIN_COMMANDS_H

class AppController;

namespace smatchet {
namespace cmd {

class CommandRegistry;

/// Register all built-in commands on `registry`. Idempotent w.r.t. calls within
/// one process — duplicate registration throws. `app` is captured by the
/// handlers and must outlive the registry. Called from `AppController::Initialize`.
void RegisterBuiltinCommands(CommandRegistry& registry, AppController& app);

}  // namespace cmd
}  // namespace smatchet

#endif  // SMATCHET_COMMANDS_BUILTIN_COMMANDS_H
