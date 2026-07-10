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
class IMainThreadPoster; // fan-in Phase 5: a migrated registrar that marshals to the UI thread takes this alongside its
                         // domain facet
// fan-in Phase 5: the migrated per-category registrars take a narrow IApp* facet, not AppController.
class IAppOfflineQueue;
class IAppMeta;
class IAppAttachments;
class IAppScenarios;
class IAppUsers;
class IAppDebug;
class IAppAutomation;
class IAppTicketData;
class IAppTicketMutations;

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
void RegisterAppCommands(CommandRegistry& reg, IAppMeta& app);
void RegisterConfigCommands(CommandRegistry& reg, AppController& app);
void RegisterPerfCommands(CommandRegistry& reg, AppController& app);
void RegisterTicketsCommands(CommandRegistry& reg, IAppTicketData& app);
void RegisterTicketMutationCommands(CommandRegistry& reg, IAppTicketMutations& app);
void RegisterDebugCommands(CommandRegistry& reg, IAppDebug& app, IMainThreadPoster& poster);
void RegisterSyncCommands(CommandRegistry& reg, AppController& app);
void RegisterFieldsCommands(CommandRegistry& reg, AppController& app);
void RegisterUsersCommands(CommandRegistry& reg, IAppUsers& app);
void RegisterOfflineCommands(CommandRegistry& reg, IAppOfflineQueue& app);
void RegisterScenarioCommands(CommandRegistry& reg, IAppScenarios& app, IMainThreadPoster& poster);
void RegisterUiTestCommands(CommandRegistry& reg, IAppScenarios& app, IMainThreadPoster& poster);
void RegisterAttachCommands(CommandRegistry& reg, IAppAttachments& app);
void RegisterAiCommands(CommandRegistry& reg, AppController& app);
void RegisterAutomationCommands(CommandRegistry& reg, IAppAutomation& app, IMainThreadPoster& poster);
void RegisterBugReportCommands(CommandRegistry& reg, IAppMeta& app, IMainThreadPoster& poster);
void RegisterUiInteractionCommands(CommandRegistry& reg, IMainThreadPoster& poster);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_SRC_COMMANDS_BUILTIN_INTERNAL_H
