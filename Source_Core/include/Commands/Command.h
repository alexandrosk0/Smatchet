#ifndef SMATCHET_COMMANDS_COMMAND_H
#define SMATCHET_COMMANDS_COMMAND_H

// Unified Command System core types — see docs/plans/shipped/command-system-plan.md.
//
// One Command struct = one operation. The same struct feeds five frontends:
//   - CLI subcommand (Target_Standalone/CliCommandRunner)
//   - In-app Command Palette (Ctrl+Shift+P)
//   - MCP tools/list + tools/call (Plugins/Mcp/McpPlugin)
//   - Lua commands.invoke (Source_Core/src/AppController_LuaBindings.cpp)
//   - Unreal in-process bridge (UnrealPlugins/SmatchetImGuiPlugin)
//
// This header is C++14-strict — no string_view / optional / variant / structured
// bindings / if constexpr — because it compiles into both SmatchetStandalone
// (GCC MinGW UCRT) and SmatchetCore_DX12 (MSVC under Unreal).

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class AppController;

namespace smatchet {
namespace cmd {

enum class ParamType {
    String,
    Int,
    Bool,
    Number,
    Json
};

struct ParamSpec {
    std::string Name;
    ParamType Type = ParamType::String;
    bool Required = false;
    std::string Description;
    nlohmann::json Default;            ///< null when no default
    std::vector<std::string> Enum;     ///< empty unless the param is enum-restricted
};

/// Stable enum — kebab-case strings cross the API boundary; renames are breaking.
enum class ErrorCode {
    None,
    UnknownCommand,
    MissingRequiredArg,
    ValidationError,
    HandlerError,
    ConfirmRequired,
    NotConnected,
    AppendOnly,
    NotFound,
    BackendError,
    DryRunUnsupported,
    Timeout
};

/// Stable kebab-case string for an `ErrorCode`. Never localized.
const char* ErrorCodeString(ErrorCode c);

struct CommandError {
    ErrorCode Code = ErrorCode::None;
    std::string Message;                      ///< human-readable
    std::string Hint;                         ///< actionable next step
    std::vector<std::string> Suggestions;     ///< e.g. fuzzy did-you-mean
    nlohmann::json Details;                   ///< optional structured detail

    nlohmann::json ToJson() const;
};

struct CommandResult {
    bool Ok = true;
    CommandError Error;
    nlohmann::json Data;

    static CommandResult Success(nlohmann::json data);
    static CommandResult Failure(ErrorCode code,
                                 std::string message,
                                 std::string hint = std::string(),
                                 std::vector<std::string> suggestions = std::vector<std::string>());

    /// Canonical wire envelope: `{ok, command, data?}` or `{ok:false, command, error}`.
    /// Caller (CLI / MCP / Lua bridge) supplies `commandName` so this struct stays
    /// independent of the registry's lookup state.
    nlohmann::json ToWireJson(const std::string& commandName, bool dryRun = false) const;
};

enum class CommandSource {
    Cli,
    Palette,
    Mcp,
    Lua,
    Unreal,
    Internal
};

struct CommandContext {
    AppController* App = nullptr;
    CommandSource Source = CommandSource::Internal;
    /// Set by `--yes` (CLI), `__confirm:true` (MCP/Unreal), or palette Shift+Enter.
    bool ConfirmedDestructive = false;
    /// Set by `--dry-run` / `__dry_run:true`. Handler must NOT mutate; returns a `wouldDo` payload.
    bool DryRun = false;
    /// Spawn-mode async wait ceiling. 0 = no cap. Surfaced as `__timeout_ms` over MCP.
    int TimeoutMs = 0;
};

struct Command {
    std::string Name;                     ///< dotted, e.g. "tickets.search_active"
    std::string Category;                 ///< first dotted segment, e.g. "tickets"
    std::string Summary;                  ///< one-line, verb-first
    std::string Description;              ///< multi-line; must include returns shape + examples
    std::vector<ParamSpec> Params;

    bool Destructive = false;             ///< requires ConfirmedDestructive (unless DryRun)
    bool Idempotent = true;
    bool AsyncSafe = true;                ///< false => spawn-mode driver must wait on PendingAsyncResult()
    bool DryRunSupported = false;         ///< honors ctx.DryRun by computing diff without mutating

    /// Back-compat names. e.g. "list_active_tickets" -> "tickets.list_active".
    std::vector<std::string> Aliases;

    /// Handler takes ctx by const reference: no built-in command mutates ctx, and the
    /// const signature lets handlers be declared `(const json&, const CommandContext&)`
    /// to satisfy cppcheck's constParameterReference rule without per-handler suppressions.
    std::function<CommandResult(const nlohmann::json&, const CommandContext&)> Handler;

    /// Build a JSON Schema object suitable for MCP `inputSchema`.
    nlohmann::json BuildJsonSchema() const;

    /// Build a human-readable help block. Format documented in COMMAND_SYSTEM_PLAN.md.
    std::string BuildHelpText() const;
};

}  // namespace cmd
}  // namespace smatchet

#endif  // SMATCHET_COMMANDS_COMMAND_H
