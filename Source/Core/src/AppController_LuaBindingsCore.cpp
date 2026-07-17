// AppController_LuaBindingsCore.cpp -- ImGui-free portion of the Lua bindings.
//
// Hosts `smatchet::lua::InitLuaCore` plus the 11 glue functions whose receiver
// is the `ILuaBindingHost` interface (not `AppController*`). The glues here
// resolve `state["__smatchet_app"]` to `ILuaBindingHost*`, which means tests
// can drop in a `FakeLuaBindingHost` without dragging ImGui / GLFW / cpr /
// SQLite into the test binary -- a strict prerequisite for Phase 6b's
// `LuaBindings.test.cpp` re-dispatch (see
// docs/plans/shipped/test-suite-expansion-completion.md § Phase 6).
//
// **Strict no-ImGui rule.** This TU must not include `<imgui.h>` or any
// transitive ImGui surface. ImGui-touching glue (imgui.text / image / button /
// progress_bar / same_line / separator / get_content_region_avail) lives in
// AppController_LuaBindings.cpp::InitLuaUi where it stays UI-coupled.
//
// Behaviour-preservation contract: every glue body is byte-identical to the
// pre-lift version, modulo the receiver cast:
//   `AppController* app = lua["__smatchet_app"].get_or<AppController*>(nullptr);`
// becomes
//   `ILuaBindingHost* host = lua["__smatchet_app"].get_or<ILuaBindingHost*>(nullptr);`
// and the existing AppController method names match the interface verbatim.

#include "ILuaBindingHost.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "ConfigManager.h"
#include "FieldEditAuditSource.h"
#include "Logger.h"
#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "Json/LuaJsonConvert.h" // JsonToLua / LuaToJson — shared inline leaf

namespace smatchet_lua_init_detail {

// `ResolveHost` is the cast site that the slice exists to introduce. Every
// glue calls this; nothing else reaches across the binding/test boundary.
static ILuaBindingHost* ResolveHost(sol::this_state L) {
    sol::state_view lua(L);
    const sol::object hostObj = lua["__smatchet_app"];
    if (!hostObj.valid() || hostObj.get_type() == sol::type::lua_nil) {
        return nullptr;
    }
    return hostObj.as<ILuaBindingHost*>();
}

// JSON <-> Lua marshalling moved to the shared Json/LuaJsonConvert.h leaf
// (included above). The public JsonToLua / LuaToJson resolve there at global
// scope; the unqualified call sites below reach them by outward lookup.

// --- Glues with non-UI behaviour. Pre-lift home: AppController_LuaBindings.cpp
// inside the same namespace. Each body is byte-identical except for the
// AppController* -> ILuaBindingHost* cast site (`ResolveHost`).

std::tuple<bool, std::string> TicketSetFieldGlue(sol::this_state L, CachedTicket& t, const std::string& fieldId,
                                                 const std::string& val) {
    ILuaBindingHost* host = ResolveHost(L);
    if (!host) {
        return {false, "AppController not available for Ticket:set_field"};
    }
    LOG_TRACE("Lua Ticket:set_field audit_source=%s issue=%s field=%s val_len=%zu", FieldEditAuditSource::Current(),
              t.id.c_str(), fieldId.c_str(), val.size());
    const TrackerField* fieldMeta = host->FindFieldById(fieldId);
    if (!fieldMeta) {
        return {false, "Field not found in tracker catalog: " + fieldId};
    }
    std::vector<std::string> vals;
    if (!val.empty()) {
        vals.push_back(val);
    }
    const VoidResult r = host->SubmitFieldEdit(t.id, *fieldMeta, vals);
    return {r.has_value(), r.has_value() ? std::string() : r.error()};
}

std::tuple<bool, std::string> TicketTransitionGlue(sol::this_state L, CachedTicket& t, const std::string& statusName) {
    ILuaBindingHost* host = ResolveHost(L);
    if (!host) {
        return {false, "AppController not available for Ticket:transition"};
    }
    LOG_TRACE("Lua Ticket:transition audit_source=%s issue=%s status=%s", FieldEditAuditSource::Current(), t.id.c_str(),
              statusName.c_str());
    const TrackerField* statusField = host->FindFieldById("status");
    if (!statusField) {
        return {false, "Tracker 'status' field meta not found"};
    }
    const VoidResult r = host->SubmitFieldEdit(t.id, *statusField, {statusName});
    return {r.has_value(), r.has_value() ? std::string() : r.error()};
}

void LuaLogInfoGlue(sol::this_state L, std::string msg) {
    ILuaBindingHost* host = ResolveHost(L);
    if (host)
        host->LuaLogInfoBind(std::move(msg));
}

std::tuple<sol::object, std::string> LuaGetTicketGlue(sol::this_state L, const std::string& issueId) {
    ILuaBindingHost* host = ResolveHost(L);
    if (!host)
        return {sol::object(), "AppController not available for get_ticket"};
    return host->LuaGetTicketBind(sol::state_view(L), issueId);
}

std::vector<CachedTicket> LuaGetActiveTicketsGlue(sol::this_state L) {
    ILuaBindingHost* host = ResolveHost(L);
    if (!host)
        return {};
    return host->LuaGetActiveTicketsBind();
}

std::tuple<sol::object, std::string> LuaDecodeJsonGlue(sol::this_state L, const std::string& s) {
    ILuaBindingHost* host = ResolveHost(L);
    if (!host)
        return {sol::object(), "AppController not available for decode_json"};
    return host->LuaDecodeJsonBind(sol::state_view(L), s);
}

void LuaMcpRegisterToolGlue(sol::this_state L, sol::table toolDef, sol::function callback) {
    ILuaBindingHost* host = ResolveHost(L);
    if (host)
        host->LuaMcpRegisterToolBind(std::move(toolDef), std::move(callback));
}

std::tuple<sol::object, std::string> LuaCreateIssueGlue(sol::this_state L, sol::table spec) {
    ILuaBindingHost* host = ResolveHost(L);
    if (!host)
        return {sol::object(), "AppController not available for create_issue"};
    return host->LuaCreateIssueBind(sol::state_view(L), std::move(spec));
}

// `tracker.get_type` is pure -- reads ConfigManager which has no transitive
// ImGui / SQLite / cpr dependency.
std::string LuaTrackerGetTypeGlue() { return TrimCopy(ConfigManager::Load().TrackerType); }

std::tuple<std::string, std::string> LuaTrackerCreateIssueGlue(sol::this_state L, sol::table fields) {
    ILuaBindingHost* host = ResolveHost(L);
    if (!host) {
        return {std::string(), std::string("AppController not available for tracker.create_issue")};
    }
    std::tuple<sol::object, std::string> bindRet = host->LuaCreateIssueBind(sol::state_view(L), std::move(fields));
    const std::string& preflightErr = std::get<1>(bindRet);
    const sol::object& resObj = std::get<0>(bindRet);
    if (!preflightErr.empty()) {
        return {std::string(), preflightErr};
    }
    if (!resObj.valid() || resObj.get_type() == sol::type::lua_nil) {
        return {std::string(), std::string("create_issue returned no result")};
    }
    if (!resObj.is<sol::table>()) {
        return {std::string(), std::string("create_issue returned unexpected type")};
    }
    sol::table r = resObj.as<sol::table>();
    const sol::object oko = r["ok"];
    const bool ok = oko.valid() && oko.is<bool>() && oko.as<bool>();
    std::string key;
    const sol::object keyo = r["issue_key"];
    if (keyo.valid() && keyo.is<std::string>()) {
        key = keyo.as<std::string>();
    }
    std::string err;
    const sol::object erro = r["error"];
    if (erro.valid() && erro.is<std::string>()) {
        err = erro.as<std::string>();
    }
    if (ok && !key.empty()) {
        return {std::move(key), std::string()};
    }
    if (!err.empty()) {
        return {std::string(), std::move(err)};
    }
    return {std::string(), std::string("create_issue failed (no issue_key)")};
}

// commands.invoke -- wraps the host's CommandRegistry::Dispatch so Lua scripts
// can call any registered command and receive a plain table {ok, data, error}.
sol::table LuaCommandsInvokeGlue(sol::this_state L, const std::string& cmdName, sol::optional<sol::table> argsTable) {
    sol::state_view sv(L);
    sol::table result = sv.create_table();
    ILuaBindingHost* host = ResolveHost(L);
    if (!host) {
        result["ok"] = false;
        result["error"] = std::string("AppController not available");
        return result;
    }
    nlohmann::json args = (argsTable) ? LuaToJson(argsTable.value()) : nlohmann::json::object();
    smatchet::cmd::CommandContext ctx;
    // Behaviour-preservation: the host hands back the same underlying app that the
    // pre-lift glue stored, now upcast to the two narrow facets the command context
    // carries. FakeLuaBindingHost returns null from both accessors -- test handlers
    // ignore the facet fields, and null simply propagates into the context.
    ctx.ScenarioHost = host->ScenarioHostForCommandContext();
    ctx.Threading = host->ThreadingForCommandContext();
    ctx.Source = smatchet::cmd::CommandSource::Lua;
    // Security audit 2026-06-13 #3: Lua is a non-UI automation source. Confirmation
    // of a destructive command must be an EXPLICIT per-call signal from the script,
    // never auto-set by this binding. Mirror MCP's `__confirm` arg convention; absent
    // or non-true -> the registry returns ConfirmRequired.
    ctx.ConfirmedDestructive = args.value("__confirm", false);
    ctx.DryRun = args.value("__dry_run", false);
    smatchet::cmd::CommandResult cr = host->LuaCommands().Dispatch(cmdName, args, ctx);
    result["ok"] = cr.Ok;
    if (cr.Ok) {
        static const nlohmann::json kEmptyData;
        result["data"] = JsonToLua(sv, cr.Data ? *cr.Data : kEmptyData);
    } else {
        sol::table errTbl = sv.create_table();
        errTbl["code"] = std::string(smatchet::cmd::ErrorCodeString(cr.Error.Code));
        errTbl["message"] = cr.Error.Message;
        errTbl["hint"] = cr.Error.Hint;
        result["error"] = errTbl;
    }
    return result;
}

} // namespace smatchet_lua_init_detail

namespace smatchet {
namespace lua {

void InitLuaCore(sol::state& state, ILuaBindingHost* host) {
    state.open_libraries(sol::lib::base);
    state.open_libraries(sol::lib::string);
    state.open_libraries(sol::lib::table);
    // math is common in cell providers (math.floor for percent formatting). Without it,
    // scripts hit "attempt to index a nil value (global 'math')" -- non-obvious if you
    // assume a standard Lua env.
    state.open_libraries(sol::lib::math);
    // os: do NOT open the standard lib -- it ships with `os.execute`, `os.remove`,
    // `os.rename`, `os.exit`, `os.getenv`, `os.setlocale`, `os.tmpname`, which let a
    // script shell out, delete arbitrary files, read process env, or terminate the host.
    // Expose only the safe time/date subset as a whitelist. A future Lua version
    // adding a new dangerous os.* fn won't silently leak in (blacklist would).
    sol::table osSafe = state.create_table();
    osSafe.set_function("time", []() -> std::time_t { return std::time(nullptr); });
    osSafe.set_function("clock", []() -> double { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; });
    osSafe.set_function("difftime", [](std::time_t a, std::time_t b) -> double { return std::difftime(a, b); });
    osSafe.set_function("date", [](sol::optional<std::string> fmt, sol::optional<std::time_t> t) -> std::string {
        const std::time_t when = t.value_or(std::time(nullptr));
        const std::string format = fmt.value_or(std::string("%c"));
        std::tm tmbuf{};
#if defined(_WIN32)
        localtime_s(&tmbuf, &when);
#else
        localtime_r(&when, &tmbuf);
#endif
        char out[256] = {};
        std::strftime(out, sizeof(out), format.c_str(), &tmbuf);
        return std::string(out);
    });
    state["os"] = osSafe;

    // Store the host per-state so glue functions resolve it via sol::this_state without a
    // process-wide pointer. Each state (main or background) holds its own entry -- no
    // cross-state races and no lifetime hazard:
    //   - For the main `lua` state: ClearLuaTicketContextGlue (called from ~AppController
    //     before the lua dtor) nils the entry, so userdata that outlives the controller
    //     resolves to null.
    //   - For per-iteration `bgState` in AutomationWorkerLoop: bgState is a stack-local
    //     destroyed at the end of each iteration. ~AppController joins automationWorker_
    //     before any member destruction (see the automationWorker_ contract in
    //     AppController.h), so the worker can never observe a half-destroyed AppController
    //     through `__smatchet_app`.
    state["__smatchet_app"] = host;

    state.new_usertype<CachedTicket>("Ticket", "id", &CachedTicket::id, "get_field", &CachedTicket::GetFieldValue,
                                     "set_field", &smatchet_lua_init_detail::TicketSetFieldGlue, "transition",
                                     &smatchet_lua_init_detail::TicketTransitionGlue);

    sol::table smatchet = state.create_table();
    smatchet.set_function("get_ticket", &smatchet_lua_init_detail::LuaGetTicketGlue);
    smatchet.set_function("get_active_tickets", &smatchet_lua_init_detail::LuaGetActiveTicketsGlue);
    smatchet.set_function("create_issue", &smatchet_lua_init_detail::LuaCreateIssueGlue);
    state["smatchet"] = smatchet;

    sol::table tracker = state.create_table();
    tracker.set_function("get_type", &smatchet_lua_init_detail::LuaTrackerGetTypeGlue);
    tracker.set_function("create_issue", &smatchet_lua_init_detail::LuaTrackerCreateIssueGlue);
    state["tracker"] = tracker;

    state.set_function("log_info", &smatchet_lua_init_detail::LuaLogInfoGlue);
    state.set_function("decode_json", &smatchet_lua_init_detail::LuaDecodeJsonGlue);

    // UI-mutating bindings (cached renderers, icon maps, ui.* table) live in InitLuaUi for
    // the main state. Background automation states re-run setup scripts in this minimal
    // Core state to define helper functions/tables -- so the UI registrations need to be
    // present as no-ops here, otherwise SmatchetHooks.lua throws "attempt to call nil"
    // every time the worker spins up. The registrations are intentionally inert: the bgState
    // is destroyed at the end of each job, so any registration on it would be discarded
    // regardless. The real registrations happen against the main `lua` state via OnEarlyInit.
    auto noop = []() {};
    state.set_function("register_field_display_cached", noop);
    state.set_function("unregister_field_display_cached", noop);
    state.set_function("register_field_display_cached_by_name", noop);
    state.set_function("unregister_field_display_cached_by_name", noop);
    state.set_function("register_field_icon_map", noop);
    state.set_function("unregister_field_icon_map", noop);
    sol::table uiNoop = state.create_table();
    uiNoop.set_function("register_window", noop);
    uiNoop.set_function("unregister_window", noop);
    uiNoop.set_function("invalidate_window", noop);
    uiNoop.set_function("invalidate_field_cache", noop);
    uiNoop.set_function("invalidate_field_cache_for", noop);
    uiNoop.set_function("register_ticket_action", noop);
    uiNoop.set_function("register_global_action", noop);
    state["ui"] = uiNoop;

    sol::table mcp = state.create_table();
    mcp.set_function("register_tool", &smatchet_lua_init_detail::LuaMcpRegisterToolGlue);
    state["mcp"] = mcp;

    // Unified Command System: `commands.invoke("name", {arg=val})` -> `{ok, data, error}`.
    // Lets Lua scripts call any registered command without needing direct AppController access.
    sol::table commands = state.create_table();
    commands.set_function("invoke", &smatchet_lua_init_detail::LuaCommandsInvokeGlue);
    state["commands"] = commands;
}

} // namespace lua
} // namespace smatchet
