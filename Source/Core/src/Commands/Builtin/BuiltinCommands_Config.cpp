// config.* — read/write/reload the persisted Smatchet config.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/TicketsMonitorCommandPure.h"

#include "ConfigManager.h"
#include "Logger.h"
#include "VsyncControl.h"

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "Json/BoundedJsonParse.h"

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::ObservedSmatchetEnv;
using builtin_detail::PString;

namespace {

void RegisterConfigPathCommand(CommandRegistry& reg) {
    {
        Command c = MakeCommand("config.path", "Paths + observed SMATCHET_* env vars (tokens redacted).",
                                [](const nlohmann::json&, const CommandContext&) {
                                    nlohmann::json out;
                                    out["userData"] = ConfigManager::GetUserDataDirectory();
                                    out["runtimeAssets"] = ConfigManager::GetRuntimeAssetDirectory();
                                    out["imGuiSettings"] = ConfigManager::GetImGuiSettingsPath();
                                    nlohmann::json envObj;
                                    envObj["observed"] = ObservedSmatchetEnv();
                                    out["env"] = std::move(envObj);
                                    return CommandResult::Success(std::move(out));
                                });
        reg.Register(std::move(c));
    }
}

void RegisterConfigGetCommand(CommandRegistry& reg) {
    {
        Command c = MakeCommand("config.get", "Get one or all keys from the loaded config.",
                                [](const nlohmann::json& args, const CommandContext&) {
                                    const TrackerConfig cfg = ConfigManager::Load();
                                    const std::string key = args.value("key", std::string());
                                    // Build a full projection of commonly-queried fields.
                                    nlohmann::json all;
                                    all["trackerType"] = cfg.TrackerType;
                                    all["domain"] = cfg.Domain;
                                    all["email"] = cfg.Email;
                                    all["jqlQuery"] = cfg.JqlQuery;
                                    all["mcpEnabled"] = cfg.McpEnabled;
                                    all["mcpPort"] = cfg.McpPort;
                                    all["mcpAllowRemote"] = cfg.McpAllowRemote;
                                    all["mcpAllowLuaExecution"] = cfg.McpAllowLuaExecution;
                                    all["readOnlyMode"] = cfg.ReadOnlyMode;
                                    all["vsync"] = cfg.VsyncEnabled;
                                    all["defaultLongTextEditorPreview"] = cfg.DefaultLongTextEditorPreview;
                                    all["logMinLevel"] = cfg.LogMinLevel;
                                    all["logTrackerHttpBodies"] = cfg.LogTrackerHttpBodies;
                                    all["dbPath"] = cfg.DbPath;
                                    all["planeUrl"] = cfg.PlaneUrl;
                                    all["planeWorkspaceSlug"] = cfg.PlaneWorkspaceSlug;
                                    // Never expose token/password in output.
                                    if (!key.empty()) {
                                        if (all.contains(key)) {
                                            return CommandResult::Success({{key, all[key]}});
                                        }
                                        return CommandResult::Failure(
                                            ErrorCode::NotFound, "Config key '" + key + "' not found or not exposed.");
                                    }
                                    return CommandResult::Success(std::move(all));
                                });
        c.Params = {PString("key", "Config key name (omit for all safe keys).")};
        reg.Register(std::move(c));
    }
}

void RegisterConfigReloadCommand(CommandRegistry& reg) {
    {
        Command c =
            MakeCommand("config.reload", "Reload config from disk (picks up manual edits to smatchet_config.json).",
                        [](const nlohmann::json&, const CommandContext&) {
                            ConfigManager::Load(); // no-op cache flush in current impl; triggers disk read
                            return CommandResult::Success({{"triggered", true}});
                        });
        reg.Register(std::move(c));
    }
}

// config.set — bidirectional key table: cmd key name maps to JSON file key name.
// Precedence contract: env vars beat the JSON file. config.set writes to the JSON
// file, so it is overridden by SMATCHET_* env vars if both are set simultaneously.
// Keys marked restart-required need the app or MCP plugin to restart to take effect.
// All others are picked up on the next config load call, such as the next sync.
// Credentials (ApiToken, PlaneApiKey, McpAuthToken) are NOT in this table — use
// SMATCHET_TRACKER_TOKEN / SMATCHET_MCP_AUTH_TOKEN env vars for secrets.
struct CfgKey {
    const char* cmd;
    const char* json;
    const char* hint;
};

const CfgKey* ConfigSetKeyTable() {
    static const CfgKey kKeys[] = {
        {"readOnlyMode", "read_only_mode", ""},
        {"logMinLevel", "log_min_level", ""},
        {"logTrackerHttpBodies", "log_tracker_http_bodies", ""},
        {"showPerformance", "show_performance_window", ""},
        {"showLogWindow", "show_log_window", ""},
        {"enableFieldOverflowTooltips", "field_overflow_tooltips", ""},
        {"vsync", "vsync_enabled", "applies immediately"},
        {"singleClickToEditGridCells", "single_click_to_edit_grid_cells", ""},
        {"defaultLongTextEditorPreview", "default_long_text_editor_preview", ""},
        {"jqlQuery", "jql", "takes effect on next sync"},
        {"domain", "domain", "restart required to reconnect"},
        {"email", "email", "restart required to reconnect"},
        // projectKey / planeProjectId writable keys no longer exist. Project is now
        // per-operation. Pass it on ticket.create (required) or pick via the in-app picker.
        {"trackerType", "tracker_type", "restart required"},
        {"planeUrl", "plane_url", "restart required to reconnect"},
        {"planeWorkspaceSlug", "plane_workspace_slug", "restart required to reconnect"},
        // User Info window keys — previously UI-only-settable; this closes the
        // CLI/MCP/Lua parity gap. String + int fields read back through ConfigManager's
        // kStringFields / kIntFields tables; values take effect on the next config load / sync.
        {"gitCommitRepos", "git_commit_repos", "comma-separated owner/repo list; next sync"},
        {"productionGroupKeyword", "production_group_keyword", "next sync"},
        {"userActivityDayWindow", "user_activity_day_window", "integer; next sync"},
        {"maxUserChanges", "max_user_changes", "integer; next sync"},
        {"vcsFeedLayout", "vcs_feed_layout", "unified|separate; applies immediately"},
        {"mcpEnabled", "mcp_enabled", "MCP plugin restart required"},
        {"mcpPort", "mcp_port", "MCP plugin restart required"},
        {"mcpAllowRemote", "mcp_allow_remote", "MCP plugin restart required"},
        {"mcpAllowLuaExecution", "mcp_allow_lua_execution", "MCP plugin restart required"},
        // Ticket-change monitor (docs/plans/active/ticket-change-monitor.md). The enabled flag
        // is also flippable via the ergonomic `tickets.monitor on|off`; the interval is set
        // here. Both take effect on the next monitor tick (no restart).
        {"ticketChangeMonitorEnabled", "ticket_change_monitor_enabled", "applies on next poll"},
        {"ticketChangeMonitorIntervalSec", "ticket_change_monitor_interval_sec",
         "integer 30..3600; clamped on load; applies on next poll"},
        {nullptr, nullptr, nullptr},
    };
    return kKeys;
}

CommandResult RunConfigSet(const nlohmann::json& args, const CommandContext& ctx) {
    const CfgKey* kKeys = ConfigSetKeyTable();
    const std::string key = args.value("key", std::string());
    // Parse the string value as JSON first (handles true/false/integers).
    // Fall back to a plain JSON string so bare values like debug work.
    const std::string rawVal = args.value("value", std::string());
    // Bounded parse: a config value is untrusted (CLI/MCP/Lua); a deeply-nested string
    // must not stack-overflow the recursive parser. ParseBounded never throws — an
    // oversized/too-deep or non-JSON value falls back to a plain string.
    std::string parseErr;
    nlohmann::json val = json_safe::ParseBounded(rawVal, parseErr);
    if (!parseErr.empty()) {
        val = rawVal; // not valid (or too large/deep) JSON — treat as a plain string
    }
    const CfgKey* found = nullptr;
    for (int i = 0; kKeys[i].cmd; ++i) {
        if (key == kKeys[i].cmd) {
            found = &kKeys[i];
            break;
        }
    }
    if (!found) {
        std::string allowed;
        for (int i = 0; kKeys[i].cmd; ++i) {
            if (i)
                allowed += ", ";
            allowed += kKeys[i].cmd;
        }
        return CommandResult::Failure(ErrorCode::ValidationError,
                                      "Key '" + key + "' is not in the config.set allowlist.", "Allowed: " + allowed);
    }
    // vsync convenience: accept on/off (and 1/0 after the JSON parse above) in
    // addition to true/false, normalised to a bool so the kBoolFields round-trip
    // and the live hub both see a real boolean.
    if (key == "vsync") {
        if (val.is_string()) {
            // Quoted forms too ("true"/"false"/"1"/"0" arrive as strings when the
            // caller quotes them past the JSON parse) — the validation hint lists
            // them all, so all of them must actually normalise (CR-953 review).
            const std::string& s = val.get_ref<const std::string&>();
            if (s == "on" || s == "true" || s == "1") {
                val = true;
            } else if (s == "off" || s == "false" || s == "0") {
                val = false;
            }
        } else if (val.is_number_integer()) {
            val = (val.get<int>() != 0);
        }
        if (!val.is_boolean()) {
            return CommandResult::Failure(ErrorCode::ValidationError, "Value for 'vsync' must be a boolean.",
                                          "Use: on, off, true, false, 1, 0");
        }
    }
    if (ctx.DryRun) {
        return CommandResult::Success({{"wouldDo", {{"cmdKey", key}, {"jsonKey", found->json}, {"value", val}}}});
    }
    nlohmann::json cfgJson = ConfigManager::LoadMergedConfigJson();
    cfgJson[found->json] = val;
    ConfigManager::WriteConfigJson(cfgJson);
    // Invalidate the Load() cache so next call picks up the new value.
    ConfigManager::InvalidateCache();
    // Live apply: a running instance flips its swapchain on the next frame —
    // the file write above covers the next launch.
    if (key == "vsync") {
        smatchet::vsync::SetEnabled(val.get<bool>());
        LOG_DEBUG("config.set vsync — live hub set to %s", val.get<bool>() ? "enabled" : "disabled");
    }
    nlohmann::json out;
    out["key"] = key;
    out["jsonKey"] = found->json;
    out["value"] = val;
    out["written"] = true;
    if (found->hint && found->hint[0]) {
        out["hint"] = std::string(found->hint);
    }
    return CommandResult::Success(std::move(out));
}

void RegisterConfigSetCommand(CommandRegistry& reg) {
    {
        Command c = MakeCommand("config.set",
                                "Persist one config key to smatchet_config.json (allowlisted; env vars override).",
                                &RunConfigSet);
        c.Destructive = false; // Config edits are easily undone; --yes would be friction.
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.Description = "cmd-key → JSON-key mapping is bidirectional with config.get. "
                        "Credentials (token, apiKey) are not settable via this command; "
                        "use SMATCHET_TRACKER_TOKEN env var instead.";
        c.Params = {
            PString("key", "Config key name (camelCase, from config.get).", true),
            // String type: bare values like `debug` and `true` both work.
            // The handler parses JSON (for bools/ints) then falls back to treating
            // the input as a plain string so users don't need to escape quotes.
            PString("value", "New value — plain string, bool (true/false), or integer.", true),
        };
        reg.Register(std::move(c));
    }
}

// tickets.monitor on|off|status — ergonomic toggle/read for the ticket-change monitor pref
// (ticket-change-monitor plan, S1c). The on and off verbs persist the enabled flag, while
// status (or no action) reports the current enabled flag plus the poll interval. The verb is
// resolved by the pure DecideTicketsMonitor; persistence mirrors the config.set JSON path.
CommandResult RunTicketsMonitor(const nlohmann::json& args, const CommandContext& ctx) {
    const std::string action = args.value("action", std::string());
    const TicketsMonitorDecision decision = DecideTicketsMonitor(action);
    if (decision.Action == TicketsMonitorAction::Invalid) {
        return CommandResult::Failure(ErrorCode::ValidationError, decision.Error);
    }

    const TrackerConfig cfg = ConfigManager::Load();
    const bool currentEnabled = cfg.TicketChangeMonitorEnabled;
    const int intervalSec = cfg.TicketChangeMonitorIntervalSec;

    if (!decision.WriteEnabled) {
        // status / no-arg — read-only projection.
        nlohmann::json out;
        out["action"] = "status";
        out["enabled"] = currentEnabled;
        out["intervalSec"] = intervalSec;
        return CommandResult::Success(std::move(out));
    }

    if (ctx.DryRun) {
        return CommandResult::Success(
            {{"wouldDo", {{"key", "ticket_change_monitor_enabled"}, {"value", decision.EnabledValue}}}});
    }

    nlohmann::json cfgJson = ConfigManager::LoadMergedConfigJson();
    cfgJson["ticket_change_monitor_enabled"] = decision.EnabledValue;
    ConfigManager::WriteConfigJson(cfgJson);
    ConfigManager::InvalidateCache();
    LOG_DEBUG("tickets.monitor — change monitor %s", decision.EnabledValue ? "enabled" : "disabled");

    nlohmann::json out;
    out["action"] = decision.EnabledValue ? "on" : "off";
    out["enabled"] = decision.EnabledValue;
    out["intervalSec"] = intervalSec;
    out["written"] = true;
    return CommandResult::Success(std::move(out));
}

void RegisterTicketsMonitorCommand(CommandRegistry& reg) {
    {
        Command c = MakeCommand(
            "tickets.monitor",
            "Toggle or query the ticket-change monitor (on|off|status). Persists the enabled pref.",
            &RunTicketsMonitor);
        c.Destructive = false;
        c.Idempotent = false;
        c.DryRunSupported = true;
        c.Params = {PString("action", "on, off, or status (default: status).")};
        reg.Register(std::move(c));
    }
}

} // namespace

void RegisterConfigCommands(CommandRegistry& reg) {
    RegisterConfigPathCommand(reg);
    RegisterConfigGetCommand(reg);
    RegisterConfigReloadCommand(reg);
    RegisterConfigSetCommand(reg);
    RegisterTicketsMonitorCommand(reg);
}

} // namespace cmd
} // namespace smatchet
