#ifndef SMATCHET_SRC_COMMANDS_BUILTIN_INTERNAL_H
#define SMATCHET_SRC_COMMANDS_BUILTIN_INTERNAL_H

// Internal helpers shared across the per-category Builtin TUs. Definitions
// live in BuiltinCommands_Helpers.cpp. Public surface (RegisterBuiltinCommands)
// is unchanged — see Commands/BuiltinCommands.h.
// This header lives next to its only consumers (BuiltinCommands_*.cpp) rather
// than in Source/Core/include because it is an implementation detail of the
// builtin-command registration and must not leak into other modules.

#include "Commands/Command.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

class AppController;
class IAppOfflineQueue; // fan-in Phase 5: RegisterOfflineCommands takes the narrow facet, not AppController

namespace smatchet {
namespace cmd {

class CommandRegistry;

namespace builtin_detail {

// --- Pagination helpers ----------------------------------------------------

nlohmann::json PaginateString(const std::vector<std::string>& items, int limit, int offset);
nlohmann::json PaginateJsonArray(const nlohmann::json& items, int limit, int offset);

// --- String helpers --------------------------------------------------------

std::string ToLowerAscii(std::string s);
std::string CategoryFromName(const std::string& name);
bool IsSensitiveEnvName(const std::string& name);
nlohmann::json ObservedSmatchetEnv();

// --- Command builders ------------------------------------------------------

Command MakeCommand(std::string name, std::string summary,
                    std::function<CommandResult(const nlohmann::json&, const CommandContext&)> handler);

ParamSpec PInt(std::string name, std::string desc, long long defaultVal);
ParamSpec PString(std::string name, std::string desc, bool required = false);

} // namespace builtin_detail

// --- Per-category registration functions ----------------------------------
// Each bucket TU defines one of these; the thin dispatcher in
// BuiltinCommands.cpp calls them in order.

void RegisterMetaCommands(CommandRegistry& reg, AppController& app);
void RegisterAppCommands(CommandRegistry& reg, AppController& app);
void RegisterConfigCommands(CommandRegistry& reg, AppController& app);
void RegisterPerfCommands(CommandRegistry& reg, AppController& app);
void RegisterTicketsCommands(CommandRegistry& reg, AppController& app);
void RegisterTicketMutationCommands(CommandRegistry& reg, AppController& app);
void RegisterDebugCommands(CommandRegistry& reg, AppController& app);
void RegisterSyncCommands(CommandRegistry& reg, AppController& app);
void RegisterFieldsCommands(CommandRegistry& reg, AppController& app);
void RegisterUsersCommands(CommandRegistry& reg, AppController& app);
void RegisterOfflineCommands(CommandRegistry& reg, IAppOfflineQueue& app);
void RegisterScenarioCommands(CommandRegistry& reg, AppController& app);
void RegisterUiTestCommands(CommandRegistry& reg, AppController& app);
void RegisterAttachCommands(CommandRegistry& reg, AppController& app);
void RegisterAiCommands(CommandRegistry& reg, AppController& app);
void RegisterAutomationCommands(CommandRegistry& reg, AppController& app);
void RegisterBugReportCommands(CommandRegistry& reg, AppController& app);
void RegisterUiInteractionCommands(CommandRegistry& reg, AppController& app);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_SRC_COMMANDS_BUILTIN_INTERNAL_H
