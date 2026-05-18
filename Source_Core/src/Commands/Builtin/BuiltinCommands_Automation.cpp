// automation.* — drive the Lua automation pipeline (RunAutoScript / RunFlatScriptAsync /
// RunLuaSetupScript) from CLI + Palette + MCP without GUI interaction. Equivalent of the
// "Run on selected" / "Run" / "Reload hooks" buttons in LuaConsolePlugin.

#include "BuiltinCommands_Internal.h"

#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PString;

namespace {

std::vector<std::string> ExtractIdsArray(const nlohmann::json& args) {
    std::vector<std::string> out;
    if (!args.is_object() || !args.contains("ids")) {
        return out;
    }
    const nlohmann::json& ids = args["ids"];
    if (ids.is_array()) {
        out.reserve(ids.size());
        for (const auto& v : ids) {
            if (v.is_string()) {
                out.push_back(v.get<std::string>());
            } else if (v.is_number_integer()) {
                out.push_back(std::to_string(v.get<long long>()));
            }
        }
    } else if (ids.is_string()) {
        // Convenience: comma-separated string.
        const std::string s = ids.get<std::string>();
        std::string cur;
        for (char c : s) {
            if (c == ',' || c == ' ') {
                if (!cur.empty()) {
                    out.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            out.push_back(cur);
        }
    }
    return out;
}

} // namespace

void RegisterAutomationCommands(CommandRegistry& reg, AppController& app) {
    {
        Command c = MakeCommand("automation.run-script",
                                "Queue a Lua automation script for execution on the given ticket IDs "
                                "(equivalent of LuaConsolePlugin 'Run on selected' button).",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string script = args.value("script", std::string());
                                    if (script.empty()) {
                                        return CommandResult::Failure(ErrorCode::MissingRequiredArg,
                                                                      "missing required 'script' parameter");
                                    }
                                    const std::vector<std::string> ids = ExtractIdsArray(args);
                                    app.RunAutoScript(script, ids);
                                    nlohmann::json out;
                                    out["queued"] = true;
                                    out["script"] = script;
                                    out["idCount"] = ids.size();
                                    return CommandResult::Success(std::move(out));
                                });
        c.Idempotent = false;
        c.AsyncSafe = true;
        c.Params = {
            PString("script", "Lua script path (e.g. 'Automation.lua') — resolved via ResolveLuaScriptPath", true),
            [] {
                ParamSpec p;
                p.Name = "ids";
                p.Description = "Array of ticket IDs to pass to process_ticket(). Empty = no-op (script's "
                                "process_ticket() requires at least one selected id).";
                p.Required = false;
                p.Type = ParamType::Json;
                return p;
            }(),
        };
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("automation.run-flat",
                                "Queue a Lua script for one-shot background execution (equivalent of "
                                "LuaConsolePlugin 'Run' button with 'Background' checked).",
                                [&app](const nlohmann::json& args, const CommandContext&) {
                                    const std::string script = args.value("script", std::string());
                                    if (script.empty()) {
                                        return CommandResult::Failure(ErrorCode::MissingRequiredArg,
                                                                      "missing required 'script' parameter");
                                    }
                                    app.RunFlatScriptAsync(script);
                                    nlohmann::json out;
                                    out["queued"] = true;
                                    out["script"] = script;
                                    return CommandResult::Success(std::move(out));
                                });
        c.Idempotent = false;
        c.AsyncSafe = true;
        c.Params = {
            PString("script", "Lua script path — runs top-level statements once on the worker thread", true),
        };
        reg.Register(std::move(c));
    }

    {
        Command c =
            MakeCommand("automation.reload-hooks",
                        "Re-run SmatchetHooks.lua on the main Lua state (equivalent of the hooks 'Run' button).",
                        [&app](const nlohmann::json&, const CommandContext&) {
                            app.RunLuaSetupScript("SmatchetHooks.lua");
                            return CommandResult::Success(nlohmann::json{{"reloaded", "SmatchetHooks.lua"}});
                        });
        c.Idempotent = false;
        c.AsyncSafe = true;
        reg.Register(std::move(c));
    }

    {
        Command c = MakeCommand("automation.list-globals",
                                "List Lua global action names registered via ui.register_global_action(...).",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    const auto names = app.GetLuaGlobalActionNames();
                                    nlohmann::json out;
                                    out["actions"] = names;
                                    out["count"] = names.size();
                                    return CommandResult::Success(std::move(out));
                                });
        c.Idempotent = true;
        c.AsyncSafe = true;
        reg.Register(std::move(c));
    }
}

} // namespace cmd
} // namespace smatchet
