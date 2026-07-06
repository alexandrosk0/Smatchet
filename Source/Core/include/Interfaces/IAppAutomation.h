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
};

#endif // SMATCHET_INTERFACES_IAPP_AUTOMATION_H
