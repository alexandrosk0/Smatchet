// config.* — read/write/reload the persisted Smatchet config.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include "ConfigManager.h"

#include <string>
#include <utility>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::ObservedSmatchetEnv;
using builtin_detail::PString;

void RegisterConfigCommands(CommandRegistry& reg, AppController& /*app*/) {
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

    {
        Command c =
            MakeCommand("config.reload", "Reload config from disk (picks up manual edits to smatchet_config.json).",
                        [](const nlohmann::json&, const CommandContext&) {
                            ConfigManager::Load(); // no-op cache flush in current impl; triggers disk read
                            return CommandResult::Success({{"triggered", true}});
                        });
        reg.Register(std::move(c));
    }

    {
        // config.set — bidirectional key table: cmd key name → JSON file key name.
        // Precedence contract: env vars beat the JSON file. config.set writes to the JSON
        // file, so it is overridden by SMATCHET_* env vars if both are set simultaneously.
        // Keys marked (restart) require the app or MCP plugin to restart to take effect;
        // all others are picked up on the next ConfigManager::Load() call (next sync, etc.).
        // Credentials (ApiToken, PlaneApiKey, McpAuthToken) are NOT in this table — use
        // SMATCHET_TRACKER_TOKEN / SMATCHET_MCP_AUTH_TOKEN env vars for secrets.
        struct CfgKey {
            const char* cmd;
            const char* json;
            const char* hint;
        };
        static const CfgKey kKeys[] = {
            {"readOnlyMode", "read_only_mode", ""},
            {"logMinLevel", "log_min_level", ""},
            {"logTrackerHttpBodies", "log_tracker_http_bodies", ""},
            {"showPerformance", "show_performance_window", ""},
            {"showLogWindow", "show_log_window", ""},
            {"enableFieldOverflowTooltips", "field_overflow_tooltips", ""},
            {"singleClickToEditGridCells", "single_click_to_edit_grid_cells", ""},
            {"jqlQuery", "jql", "takes effect on next sync"},
            {"domain", "domain", "restart required to reconnect"},
            {"email", "email", "restart required to reconnect"},
            // PR 6: `projectKey` / `planeProjectId` writable keys removed. Project is now
            // per-operation; pass it on `ticket.create` (required) or pick via the in-app picker.
            {"trackerType", "tracker_type", "restart required"},
            {"planeUrl", "plane_url", "restart required to reconnect"},
            {"planeWorkspaceSlug", "plane_workspace_slug", "restart required to reconnect"},
            {"mcpEnabled", "mcp_enabled", "MCP plugin restart required"},
            {"mcpPort", "mcp_port", "MCP plugin restart required"},
            {"mcpAllowRemote", "mcp_allow_remote", "MCP plugin restart required"},
            {"mcpAllowLuaExecution", "mcp_allow_lua_execution", "MCP plugin restart required"},
            {nullptr, nullptr, nullptr},
        };

        Command c = MakeCommand(
            "config.set", "Persist one config key to smatchet_config.json (allowlisted; env vars override).",
            [](const nlohmann::json& args, const CommandContext& ctx) {
                const std::string key = args.value("key", std::string());
                // Parse the string value as JSON first (handles true/false/integers).
                // Fall back to a plain JSON string so bare values like `debug` work.
                const std::string rawVal = args.value("value", std::string());
                nlohmann::json val;
                try {
                    val = nlohmann::json::parse(rawVal);
                } catch (...) {
                    val = rawVal; // treat as plain string
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
                                                  "Key '" + key + "' is not in the config.set allowlist.",
                                                  "Allowed: " + allowed);
                }
                if (ctx.DryRun) {
                    return CommandResult::Success(
                        {{"wouldDo", {{"cmdKey", key}, {"jsonKey", found->json}, {"value", val}}}});
                }
                nlohmann::json cfgJson = ConfigManager::LoadMergedConfigJson();
                cfgJson[found->json] = val;
                ConfigManager::WriteConfigJson(cfgJson);
                // Invalidate the Load() cache so next call picks up the new value.
                ConfigManager::InvalidateCache();
                nlohmann::json out;
                out["key"] = key;
                out["jsonKey"] = found->json;
                out["value"] = val;
                out["written"] = true;
                if (found->hint && found->hint[0]) {
                    out["hint"] = std::string(found->hint);
                }
                return CommandResult::Success(std::move(out));
            });
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

} // namespace cmd
} // namespace smatchet
