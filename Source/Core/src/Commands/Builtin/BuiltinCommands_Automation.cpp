// automation.* — drive the Lua automation pipeline (RunAutoScript / RunFlatScriptAsync /
// RunLuaSetupScript) from CLI + Palette + MCP without GUI interaction. Equivalent of the
// "Run on selected" / "Run" / "Reload hooks" buttons in LuaConsolePlugin.

#include "BuiltinCommands_Internal.h"

// fan-in Phase 5: depend on the narrow IAppAutomation facet, not the full AppController.h.
// Also depend on IMainThreadPoster (via MainThreadDispatch.h) for the reload-hooks UI-thread
// hop — see the comment on automation.reload-hooks below.
#include "Interfaces/IAppAutomation.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"

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

void RegisterAutomationRunScriptCommand(CommandRegistry& reg, IAppAutomation& app) {
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
                                const bool processAll = args.value("process_all", false);
                                app.RunAutoScript(script, ids, processAll);
                                nlohmann::json out;
                                out["queued"] = true;
                                out["script"] = script;
                                out["idCount"] = ids.size();
                                out["processAll"] = processAll;
                                return CommandResult::Success(std::move(out));
                            });
    c.Idempotent = false;
    c.AsyncSafe = true;
    c.Params = {
        PString("script", "Lua script path (e.g. 'Automation.lua') — resolved via ResolveLuaScriptPath", true),
        [] {
            ParamSpec p;
            p.Name = "ids";
            p.Description = "Array of ticket IDs to pass to process_ticket(). Empty + process_all unset "
                            "= the job refuses to run (Issue #824: no silent mass-modify, no silent no-op).";
            p.Required = false;
            p.Type = ParamType::Json;
            return p;
        }(),
        [] {
            ParamSpec p;
            p.Name = "process_all";
            p.Description = "Explicit opt-in to run process_ticket() across EVERY loaded ticket. Required "
                            "when 'ids' is empty; ignored (selection wins) when 'ids' is non-empty.";
            p.Required = false;
            p.Type = ParamType::Bool;
            return p;
        }(),
    };
    reg.Register(std::move(c));
}

void RegisterAutomationRunFlatCommand(CommandRegistry& reg, IAppAutomation& app) {
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

// Issue #1693: RunLuaSetupScript executes against the SHARED main sol::state (the same
// lua_State DrawLuaWindows / Lua cell-providers drive every frame on the UI thread).
// CommandRegistry::Dispatch has no thread affinity, so this command is reachable from an
// MCP/httplib worker thread — two threads through one lua_State is a genuine Lua-heap race
// (same failure class as the fixed mcp_lua_fresh_state_race bug). A fresh isolated
// sol::state is NOT a valid fix here: ui.register_global_action (and any other setup-script
// side effect) must land in the live main state's registries for the reload to actually
// take effect — a sol::function captured on a throwaway state can't be invoked safely from
// the UI thread's state, and the throwaway state is gone the instant this call returns. So
// the correct fix is the same UI-thread hop already used by every other UI-affecting command
// (AppViewCommands.cpp / PaneCommands.cpp / BuiltinCommands_Debug.cpp's dock.* / window.*
// commands): marshal onto the UI thread via RunOnUiThreadAsCommandResult, which runs inline
// with no extra latency when the caller is already on the UI thread (Palette / Lua-hook
// dispatch) and blocks on a one-shot future for the ~1-frame round trip otherwise
// (MCP/Lua-worker dispatch). ReloadSmatchetHooksSetupScript in LuaConsolePlugin.cpp calls
// RunLuaSetupScript directly (not through this command) but only from the UI-thread ImGui
// "Run" button draw call, so that path is unaffected.
void RegisterAutomationReloadHooksCommand(CommandRegistry& reg, IAppAutomation& app, IMainThreadPoster& poster) {
    Command c = MakeCommand("automation.reload-hooks",
                            "Re-run SmatchetHooks.lua on the main Lua state (equivalent of the hooks 'Run' button). "
                            "Marshals onto the UI thread — the main Lua state is UI-thread-owned.",
                            [&app, &poster](const nlohmann::json&, const CommandContext&) {
                                return RunOnUiThreadAsCommandResult(poster, [&app]() {
                                    app.RunLuaSetupScript("SmatchetHooks.lua");
                                    return CommandResult::Success(nlohmann::json{{"reloaded", "SmatchetHooks.lua"}});
                                });
                            });
    c.Idempotent = false;
    c.AsyncSafe = true;
    reg.Register(std::move(c));
}

void RegisterAutomationListGlobalsCommand(CommandRegistry& reg, IAppAutomation& app) {
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

} // namespace

void RegisterAutomationCommands(CommandRegistry& reg, IAppAutomation& app, IMainThreadPoster& poster) {
    RegisterAutomationRunScriptCommand(reg, app);
    RegisterAutomationRunFlatCommand(reg, app);
    RegisterAutomationReloadHooksCommand(reg, app, poster);
    RegisterAutomationListGlobalsCommand(reg, app);
}

} // namespace cmd
} // namespace smatchet
