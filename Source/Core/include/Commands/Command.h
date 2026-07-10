#ifndef SMATCHET_COMMANDS_COMMAND_H
#define SMATCHET_COMMANDS_COMMAND_H

// Unified Command System core types — see docs/plans/shipped/command-system-plan.md.
// One Command struct = one operation. The same struct feeds five frontends:
//   - CLI subcommand (Source/Standalone/CliCommandRunner)
//   - In-app Command Palette (Ctrl+Shift+P)
//   - MCP tools/list + tools/call (Source/Plugins/Mcp/McpPlugin)
//   - Lua commands.invoke (Source/Core/src/AppController_LuaBindings.cpp)
//   - Unreal in-process bridge (Source/UnrealPlugins/SmatchetImGuiPlugin)
// This header is C++14-strict — no string_view / optional / variant / structured
// bindings / if constexpr — because it compiles into both SmatchetStandalone
// (GCC MinGW UCRT) and SmatchetCore_DX12 (MSVC under Unreal).

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// json_fwd, not the full json.hpp: the three by-value json members below are
// boxed behind a shared pointer, so this 42-includer header needs only the
// forward declaration. The complete json type is required at the .cpp sites that
// build or read them (Command.cpp plus the command-registration TUs), which
// include nlohmann/json.hpp directly. See debt json-fwd-swap-by-value-carriers.
#include <nlohmann/json_fwd.hpp>

class IAppScenarioHost;
class IAppThreading;

namespace smatchet {
namespace cmd {

enum class ParamType { String, Int, Bool, Number, Json };

struct ParamSpec {
    std::string Name;
    ParamType Type = ParamType::String;
    bool Required = false;
    std::string Description;
    std::shared_ptr<nlohmann::json> Default; ///< null ptr when no default
    std::vector<std::string> Enum;           ///< empty unless the param is enum-restricted
    /// Optional numeric bounds for Int/Number params (null ptr == unbounded). Enforced
    /// in ValidateAndResolveArgs after coercion; out-of-range yields ValidationError.
    std::shared_ptr<long long> MinInt;
    std::shared_ptr<long long> MaxInt;
    /// Optional max byte-length for String/Json string args (0 == unbounded).
    std::size_t MaxLen = 0;
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
    std::string Message;                     ///< human-readable
    std::string Hint;                        ///< actionable next step
    std::vector<std::string> Suggestions;    ///< e.g. fuzzy did-you-mean
    std::shared_ptr<nlohmann::json> Details; ///< optional structured detail (null ptr when none)

    nlohmann::json ToJson() const;
};

struct CommandResult {
    bool Ok = true;
    CommandError Error;
    std::shared_ptr<nlohmann::json> Data;

    static CommandResult Success(nlohmann::json data);
    static CommandResult Failure(ErrorCode code, std::string message, std::string hint = std::string(),
                                 std::vector<std::string> suggestions = std::vector<std::string>());

    /// Canonical wire envelope: `{ok, command, data?}` or `{ok:false, command, error}`.
    /// Caller (CLI / MCP / Lua bridge) supplies `commandName` so this struct stays
    /// independent of the registry's lookup state.
    nlohmann::json ToWireJson(const std::string& commandName, bool dryRun = false) const;
};

enum class CommandSource { Cli, Palette, Mcp, Lua, Unreal, Internal };

/// Stable lowercase tag for a `CommandSource` (audit logs / diagnostics). Never localized.
inline const char* CommandSourceString(CommandSource s) {
    switch (s) {
    case CommandSource::Cli:
        return "cli";
    case CommandSource::Palette:
        return "palette";
    case CommandSource::Mcp:
        return "mcp";
    case CommandSource::Lua:
        return "lua";
    case CommandSource::Unreal:
        return "unreal";
    case CommandSource::Internal:
        return "internal";
    }
    return "unknown";
}

/// True when a source is non-interactive automation (no human at a confirm
/// dialog): CLI, MCP, Lua. Palette + Unreal are interactive-UI (the front-end
/// itself gates destructive intent — palette Shift+Enter, Unreal console flag).
/// Internal is trusted system code (scenarios, in-process toolbar) and is NOT
/// automation. Security audit 2026-06-13 #2/#3: a token/handle-holding automation
/// source must not reach destructive commands without an explicit per-call signal.
inline bool IsAutomationSource(CommandSource s) {
    return s == CommandSource::Cli || s == CommandSource::Mcp || s == CommandSource::Lua;
}

/// Pure source-trust decision for the destructive guard. Returns true when the
/// dispatch MUST be blocked with `ErrorCode::ConfirmRequired`. The rule under the
/// "require-confirm destructive on non-UI sources" posture:
///   - non-destructive command            -> never blocked
///   - dry-run                            -> never blocked (no mutation happens)
///   - already-confirmed (explicit signal)-> never blocked
///   - destructive + unconfirmed:
///       * automation source (CLI/MCP/Lua) -> BLOCKED (needs explicit confirm)
///       * interactive-UI / Internal       -> BLOCKED too (registry has always
///         required ConfirmedDestructive; the UI sets it deliberately before
///         dispatch). Source only changes the *audit* posture, not the gate, so
///         the guard stays a single chokepoint with no per-source bypass.
/// The `source` parameter is retained so the predicate is the single place that
/// reasons about trust; callers also use `IsAutomationSource` for audit logging.
inline bool RequiresExplicitConfirm(CommandSource source, bool destructive, bool confirmed, bool dryRun) {
    (void)source; // trust posture is uniform: no source may bypass the confirm gate.
    if (!destructive)
        return false;
    if (dryRun)
        return false;
    return !confirmed;
}

struct CommandContext {
    /// Scenario drive path: the runner reads this facet when a scenario command
    /// starts and passes it to the scenario lifecycle hooks. Writers that hold
    /// the full app assign it to this field and to Threading below; contexts
    /// built inside scenario hooks carry ScenarioHost only, because a hook
    /// receives the host facet and never a concrete controller.
    IAppScenarioHost* ScenarioHost = nullptr;
    /// Worker-launch escape hatch for handlers that spawn a background task and
    /// post results back to the UI thread (today: the whisper model download).
    IAppThreading* Threading = nullptr;
    CommandSource Source = CommandSource::Internal;
    /// Set by `--yes` (CLI), `__confirm:true` (MCP/Unreal), or palette Shift+Enter.
    bool ConfirmedDestructive = false;
    /// Set by `--dry-run` / `__dry_run:true`. Handler must NOT mutate; returns a `wouldDo` payload.
    bool DryRun = false;
    /// Spawn-mode async wait ceiling. 0 = no cap. Surfaced as `__timeout_ms` over MCP.
    int TimeoutMs = 0;
};

struct Command {
    std::string Name;        ///< dotted, e.g. "tickets.search_active"
    std::string Category;    ///< first dotted segment, e.g. "tickets"
    std::string Summary;     ///< one-line, verb-first
    std::string Description; ///< multi-line; must include returns shape + examples
    std::vector<ParamSpec> Params;

    bool Destructive = false; ///< requires ConfirmedDestructive (unless DryRun)
    bool Idempotent = true;
    bool AsyncSafe = true;        ///< false => spawn-mode driver must wait on PendingAsyncResult()
    bool DryRunSupported = false; ///< honors ctx.DryRun by computing diff without mutating

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

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_COMMAND_H
