#ifndef SMATCHET_INTERFACES_IAPP_AUTOMATION_H
#define SMATCHET_INTERFACES_IAPP_AUTOMATION_H

// Narrow facet for the automation.* command surface (run auto/flat Lua scripts, the
// setup script, and enumerate global Lua actions) — AppController fan-in Phase 5
// (docs/plans/appcontroller-fan-in-phase5-facets.md). AppController implements it; the
// automation.* command TU depends on this instead of the full AppController.h.
// None of these are per-frame (each enqueues a background automation job or is a one-shot
// setup call). All are present with or without the Lua build (no-op / empty when
// SMATCHET_WITH_LUA_AUTOMATION is off), so the facet is unconditional. Rank-0 leaf: only
// std string/vector cross the boundary.

#include "SmatchetResult.h" // VoidResult (Lua-script consent gate — #21 AppController public-API flip)

#include <string>
#include <vector>

class IAppAutomation {
  public:
    virtual ~IAppAutomation() = default;

    virtual void RunAutoScript(const std::string& scriptPath, const std::vector<std::string>& selectedIds,
                               bool processAll = false) = 0;
    virtual void RunFlatScriptAsync(const std::string& scriptPath) = 0;
    virtual void RunLuaSetupScript(const std::string& scriptPath) = 0;
    virtual std::vector<std::string> GetLuaGlobalActionNames() const = 0;

    // First-run Lua script consent gate (LuaScriptConsent.h). Approve/revoke a Scripts/*.lua
    // basename at its current content so it may (or may not) execute; list the currently-approved
    // resolved paths. VoidResult: Err(reason) on failure. Present with or without the Lua build.
    virtual VoidResult ApproveLuaScript(const std::string& scriptName) = 0;
    virtual VoidResult RevokeLuaScript(const std::string& scriptName) = 0;
    virtual std::vector<std::string> ListApprovedLuaScriptPaths() const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_AUTOMATION_H
