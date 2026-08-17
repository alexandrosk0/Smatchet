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
class IAppSync;
class IAppFields;

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

// --- Duration helpers ------------------------------------------------------

/// Render a POSITIVE duration as the tracker's `timeSpent` string: "1h", "1m 30s", "1h 1m 30s".
/// Every non-zero component is emitted, including the sub-minute tail. The previous inline
/// formatter had an if/else-if chain that stopped after hours+minutes, so 3630s submitted as "1h"
/// and 3690s as "1h 1m" — the tracker recorded 30 s less time than the caller logged, silently
/// (#2054). `ticket.add_worklog` rejects seconds <= 0 before calling this; a non-positive argument
/// here is a programming error and returns "0s" rather than an empty or negative timeSpent.
std::string FormatWorklogTimeSpent(int seconds);

// --- Command builders ------------------------------------------------------

Command MakeCommand(std::string name, std::string summary,
                    std::function<CommandResult(const nlohmann::json&, const CommandContext&)> handler);

ParamSpec PInt(std::string name, std::string desc, long long defaultVal);
ParamSpec PString(std::string name, std::string desc, bool required = false);

} // namespace builtin_detail

// --- Per-category registration functions ----------------------------------
// Each bucket TU defines one of these; the thin dispatcher in
// BuiltinCommands.cpp calls them in order.

// commands.* / config.* need no app facet — commands.* introspects the registry, config.*
// goes through the ConfigManager singleton. Taking no app object makes both harness-testable
// against a bare CommandRegistry and keeps them off the AppController fan-in.
void RegisterMetaCommands(CommandRegistry& reg);
void RegisterAppCommands(CommandRegistry& reg, IAppMeta& app);
void RegisterConfigCommands(CommandRegistry& reg);
void RegisterPerfCommands(CommandRegistry& reg, AppController& app);
void RegisterTicketsCommands(CommandRegistry& reg, IAppTicketData& app);
void RegisterTicketMutationCommands(CommandRegistry& reg, IAppTicketMutations& app);
void RegisterDebugCommands(CommandRegistry& reg, IAppDebug& app, IMainThreadPoster& poster);
void RegisterSyncCommands(CommandRegistry& reg, IAppSync& app);
void RegisterFieldsCommands(CommandRegistry& reg, IAppFields& app);
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
