// Built-in commands registered by AppController::Initialize.
// See docs/design/applied/command-system-plan.md for the full catalogue spec.
//
// This file is the thin dispatcher: each per-category bucket TU lives under
// Source_Core/src/Commands/Builtin/ and exposes a single
// Register<Category>Commands(reg, app) function. The dispatcher calls them in
// the same order the original monolithic body did. Helper definitions live in
// Builtin/BuiltinCommands_Helpers.cpp and are surfaced via
// Builtin/BuiltinCommands_Internal.h.
//
// All handlers are AsyncSafe=true unless explicitly marked otherwise. Every
// handler must be reentrant — handlers may call back into the registry via
// `Lua commands.invoke` or `commands.invoke` MCP tool.

#include "Commands/BuiltinCommands.h"

#include "Commands/CommandRegistry.h"
#include "Commands/ViewToggleCommands.h"

#include "Builtin/BuiltinCommands_Internal.h"

#include "Logger.h"

namespace smatchet {
namespace cmd {

void RegisterBuiltinCommands(CommandRegistry& reg, AppController& app) {
    // Order mirrors the original monolithic RegisterBuiltinCommands body so
    // first-seen wins behave identically. Same-category blocks that were
    // scattered in the original are merged into a single bucket TU; within
    // each bucket the original within-category order is preserved.
    RegisterMetaCommands(reg, app);
    RegisterAppCommands(reg, app);
    RegisterConfigCommands(reg, app);
    RegisterPerfCommands(reg, app);
    RegisterTicketsCommands(reg, app);
    RegisterDebugCommands(reg, app);
    RegisterSyncCommands(reg, app);
    RegisterTicketMutationCommands(reg, app);
    RegisterFieldsCommands(reg, app);
    RegisterUsersCommands(reg, app);
    RegisterOfflineCommands(reg, app);
    RegisterAttachCommands(reg, app);
    RegisterScenarioCommands(reg, app);
    RegisterUiTestCommands(reg, app);
    RegisterAiCommands(reg, app);
    RegisterAgenticCommands(reg, app);
    RegisterHandoffCommands(reg, app);
    RegisterAutomationCommands(reg, app);

    RegisterViewToggleCommands(reg, app);

    LOG_INFO("CommandRegistry: registered %zu built-in commands", reg.All().size());
}

} // namespace cmd
} // namespace smatchet
