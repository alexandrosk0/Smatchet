#ifndef SMATCHET_ILUA_BINDING_HOST_H
#define SMATCHET_ILUA_BINDING_HOST_H

// Order matches AppController.h: limits + cstdint BEFORE sol/sol.hpp so GCC 13+
// std::numeric_limits picks up the right specialisation under -mcmodel=large.
#include <limits>
#include <cstdint>

#include <sol/sol.hpp>

#include <string>
#include <tuple>
#include <vector>

#include "CachedTicketTypes.h"      // POD: no SQLite, no HTTP, no ImGui
#include "Commands/CommandRegistry.h"
#include "TrackerFieldSchema.h"     // TrackerField — POD

// Forward-declared in Commands/Command.h. Re-declared here so callers of
// `AppForCommandContext()` don't need to chase the include chain.
class AppController;

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
 * Behaviour-preservation contract: the methods on this interface have the
 * **same** signatures as the `AppController::Lua*Bind` methods they replace.
 * The only thing that changed at the glue site is the static type of the
 * resolved pointer (`AppController*` → `ILuaBindingHost*`).
 */
class ILuaBindingHost {
  public:
    virtual ~ILuaBindingHost() = default;

    // --- Logging ---
    virtual void LuaLogInfoBind(const std::string& msg) = 0;

    // --- Ticket reads ---
    virtual std::tuple<sol::object, std::string> LuaGetTicketBind(const std::string& issueId) = 0;
    virtual std::vector<CachedTicket> LuaGetActiveTicketsBind() = 0;

    // --- JSON ---
    virtual std::tuple<sol::object, std::string> LuaDecodeJsonBind(const std::string& s) = 0;

    // --- Field edits (Ticket:set_field / Ticket:transition) ---
    virtual const TrackerField* FindFieldById(const std::string& fieldId) const = 0;
    virtual bool SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                 const std::vector<std::string>& rawValues, std::string& outError) = 0;

    // --- Issue create ---
    virtual std::tuple<sol::object, std::string> LuaCreateIssueBind(sol::table spec) = 0;

    // --- MCP tool registration ---
    virtual void LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback) = 0;

    // --- Command registry pass-through (commands.invoke) ---
    virtual smatchet::cmd::CommandRegistry& LuaCommands() = 0;

    // --- ctx.App propagation for `commands.invoke` ---
    // `CommandContext::App` is an `AppController*` (per Commands/Command.h:99).
    // Production returns `this` cast to `AppController*`; test fakes return
    // `nullptr` -- test command handlers either ignore ctx.App or aren't
    // registered. Forward-declared only; never dereferenced inside ILuaBindingHost.
    virtual AppController* AppForCommandContext() = 0;
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

#endif // SMATCHET_ILUA_BINDING_HOST_H
