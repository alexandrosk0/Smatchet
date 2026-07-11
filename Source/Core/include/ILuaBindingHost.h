#ifndef SMATCHET_ILUA_BINDING_HOST_H
#define SMATCHET_ILUA_BINDING_HOST_H

// This header is included by TUs that compile unconditionally (e.g. AppController.h
// consumers). Guard all sol2 surface behind SMATCHET_WITH_LUA_AUTOMATION so the
// header remains includable when Lua is disabled and sol2 headers are not present.

#if defined(SMATCHET_WITH_LUA_AUTOMATION)

// Order matches AppController.h: limits + cstdint BEFORE sol/sol.hpp so GCC 13+
// std::numeric_limits picks up the right specialisation under -mcmodel=large.
#include <limits>
#include <cstdint>

#include <sol/sol.hpp>

#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "CachedTicketTypes.h" // POD: no SQLite, no HTTP, no ImGui
#include "Commands/CommandRegistry.h"
#include "TrackerFieldSchema.h" // TrackerField — POD

// Narrow app facets carried by the command context (declared in
// Commands/Command.h). Forward declarations suffice: the accessors below
// return them by pointer and never dereference them in this header.
class IAppScenarioHost;
class IAppThreading;

namespace smatchet {
namespace lua {

/// Definition of a Lua-registered MCP tool. Relocated here from `AppController`
/// (hardening #19c) so the MCP plugin + ILuaBindingHost surface can read tool
/// metadata (name / description / parametersSchema) without pulling sol2 into
/// AppController.h. `callback` is the sol::protected_function bound to the
/// registering state; McpPlugin reads only the first three fields.
struct McpToolDefinition {
    std::string name;
    std::string description;
    nlohmann::json parametersSchema;
    sol::protected_function callback;
};

} // namespace lua
} // namespace smatchet

/**
 * Pure-virtual host interface that the lifted `InitLuaCore` glue functions resolve
 * through `state["__smatchet_app"]`. Replaces the previous direct cast to
 * `AppController*`, which dragged ImGui + GLFW + cpr + SQLite into any test binary
 * that linked the binding TU.
 *
 * `AppController` implements this in production. A `FakeLuaBindingHost` impl can
 * sit in `tests/Lua/` and drive `LuaBindings.test.cpp` (Phase 6b) without linking
 * a single ImGui translation unit.
 *
 * **Strict no-ImGui rule**: any new method that has to touch ImGui state belongs
 * on `AppController` directly, NOT here. The `InitLuaUi`-resident glues remain
 * in `AppController_LuaBindings.cpp` for exactly that reason.
 *
 * Behaviour-preservation contract: the methods on this interface mirror the
 * `AppController::Lua*Bind` methods they replace. Exception: the sol::object-
 * returning marshallers (`LuaGetTicketBind`, `LuaDecodeJsonBind`,
 * `LuaCreateIssueBind`) take a leading `sol::state_view sv` (the glue's calling
 * state) so results are built on the caller's state rather than a member state —
 * required now that callers may be off-UI-thread fresh states.
 */
class ILuaBindingHost {
  public:
    virtual ~ILuaBindingHost() = default;

    // --- Logging ---
    virtual void LuaLogInfoBind(const std::string& msg) = 0;

    // --- Ticket reads ---
    // `sv` is the *calling* Lua state (the glue's sol::this_state). Marshal returned
    // sol::objects against `sv`, never against any member state — the caller may be an
    // off-UI-thread fresh state (MCP worker / automation worker). See
    // docs/plans/shipped/mcp-lua-fresh-state-race.md.
    virtual std::tuple<sol::object, std::string> LuaGetTicketBind(sol::state_view sv, const std::string& issueId) = 0;
    virtual std::vector<CachedTicket> LuaGetActiveTicketsBind() = 0;

    // --- JSON ---
    virtual std::tuple<sol::object, std::string> LuaDecodeJsonBind(sol::state_view sv, const std::string& s) = 0;

    // --- Field edits (Ticket:set_field / Ticket:transition) ---
    virtual const TrackerField* FindFieldById(const std::string& fieldId) const = 0;
    virtual bool SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                 const std::vector<std::string>& rawValues, std::string& outError) = 0;

    // --- Issue create ---
    // `sv` is the calling Lua state — see the LuaGetTicketBind note. `spec` is already
    // bound to `sv`; the returned result table MUST also be built on `sv`.
    virtual std::tuple<sol::object, std::string> LuaCreateIssueBind(sol::state_view sv, sol::table spec) = 0;

    // --- MCP tool registration ---
    virtual void LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback) = 0;

    // --- MCP tool snapshot (read by McpPlugin via AppController::GetLuaBindingHost) ---
    /** Thread-safe snapshot (e.g. MCP server thread vs Lua registration on the app thread). */
    virtual std::vector<smatchet::lua::McpToolDefinition> GetLuaMcpTools() const = 0;

    // --- Command registry pass-through (commands.invoke) ---
    virtual smatchet::cmd::CommandRegistry& LuaCommands() = 0;

    // --- app-facet propagation for the invoke glue ---
    // The invoke glue copies these pointers into the command context it builds,
    // one per facet field. Production returns the owning app upcast to each
    // facet; the upcast happens in the implementing translation unit, where the
    // concrete type is complete. Test fakes return null from both -- the command
    // handlers exercised there either ignore the facet fields or aren't
    // registered. Never dereferenced inside this interface.
    virtual IAppScenarioHost* ScenarioHostForCommandContext() = 0;
    virtual IAppThreading* ThreadingForCommandContext() = 0;
};

namespace smatchet {
namespace lua {

/**
 * Lift the ImGui-free portion of `AppController::InitLuaCore` (open libraries +
 * register the `Ticket` usertype + `smatchet.*` / `tracker.*` / `mcp.*` / `commands.*`
 * tables + the `log_info` / `decode_json` globals + the `os.*` whitelist).
 *
 * The host pointer is stashed under `state["__smatchet_app"]` so the lifted
 * glue functions can resolve it via `sol::this_state` without a process-wide
 * singleton.
 *
 * Behaviour-preservation contract: this body is byte-for-byte identical to the
 * pre-lift `AppController::InitLuaCore` body, modulo the receiver type
 * (`AppController*` → `ILuaBindingHost*`).
 */
void InitLuaCore(sol::state& state, ILuaBindingHost* host);

} // namespace lua
} // namespace smatchet

#endif // SMATCHET_WITH_LUA_AUTOMATION

#endif // SMATCHET_ILUA_BINDING_HOST_H
