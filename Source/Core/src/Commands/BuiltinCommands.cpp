// Built-in commands registered by AppController::Initialize.
// See docs/plans/shipped/command-system-plan.md for the full catalogue spec.
// This file is the thin dispatcher: each per-category bucket TU lives under
// Source/Core/src/Commands/Builtin/ and exposes a single
// Register<Category>Commands(reg, app) function. The dispatcher calls them in
// the same order the original monolithic body did. Helper definitions live in
// Builtin/BuiltinCommands_Helpers.cpp and are surfaced via
// Builtin/BuiltinCommands_Internal.h.
// All handlers are AsyncSafe=true unless explicitly marked otherwise. Every
// handler must be reentrant — handlers may call back into the registry via
// `Lua commands.invoke` or `commands.invoke` MCP tool.

#include "Commands/BuiltinCommands.h"

#include "Commands/AppViewCommands.h"
#include "Commands/CommandRegistry.h"
#include "Commands/UiModeCommand.h"
#include "Commands/ViewToggleCommands.h"

#include "Builtin/BuiltinCommands_Internal.h"

// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=this thin dispatcher is the single orchestrator that routes the concrete AppController to the per-domain IApp* facet interfaces (fan-in Phase 5); it needs AppController's complete definition for the AppController& -> IApp*& derived-to-base conversion when calling a facet-migrated registrar. It is the ONE-TIME orchestrator includer that replaces the N per-category leaf command TUs each dropping AppController.h as facets land; owner=orchestrator; revisit=when the builtin-command wiring moves to an AppController-side call site that already holds a complete *this)
#include "AppController.h"
// clang-format on

#include "Logger.h"

#if !defined(SMATCHET_WITH_AI)
namespace smatchet {
namespace cmd {

void RegisterAiCommands(CommandRegistry& reg, AppController& app) {
    (void)reg;
    (void)app;
}

} // namespace cmd
} // namespace smatchet
#endif

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
    RegisterAutomationCommands(reg, app);
    RegisterBugReportCommands(reg, app);
    RegisterUiInteractionCommands(reg, app);

    RegisterViewToggleCommands(reg, app);
    RegisterAppViewCommands(reg, app);
    RegisterUiModeCommand(reg, app);

    LOG_INFO("CommandRegistry: registered %zu built-in commands", reg.All().size());
}

} // namespace cmd
} // namespace smatchet
