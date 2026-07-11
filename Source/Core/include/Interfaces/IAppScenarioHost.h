#ifndef SMATCHET_INTERFACES_IAPP_SCENARIO_HOST_H
#define SMATCHET_INTERFACES_IAPP_SCENARIO_HOST_H

// Narrow scenario-host facet of AppController — fan-in Phase 6 T5
// (docs/plans/shipped/appcontroller-fan-in-phase6-dissolution.md). The scenario
// lifecycle hooks and the runner tick take this instead of the concrete
// AppController, so scenario TUs whose only app dependency is spinning live
// pane contexts or toggling the Lua cached-provider hooks stop including
// AppController.h. Scenarios that genuinely need the concrete object — today
// only the ui-test active-app seam — downcast at the top of the hook, which
// is safe because the runner only ever drives scenarios with the owning
// AppController, the sole implementer.
// Rank-0 leaf under Interfaces/: GridLiveContext is returned by pointer only,
// so a forward declaration suffices, and the command registry is returned by
// reference to a forward-declared class.

#include <string>
#include <vector>

struct GridLiveContext;

namespace smatchet {
namespace cmd {
class CommandRegistry;
}
} // namespace smatchet

class IAppScenarioHost {
  public:
    virtual ~IAppScenarioHost() = default;

    /// Ensure a live per-pane grid context exists for `paneId` on `backendKey`,
    /// creating + wiring it if needed. Mirrors AppController::EnsurePaneContextLive.
    virtual GridLiveContext* EnsurePaneContextLive(const std::string& paneId, const std::string& backendKey) = 0;

    /// Scenario hook: displace the user-side Lua cached-field provider for `fieldId`
    /// with `luaFnName`, loading `extraScripts` if the function is not yet defined.
    /// Mirrors AppController::ScenarioRegisterLuaCachedProvider (no-op stub in the
    /// no-Lua build).
    virtual bool ScenarioRegisterLuaCachedProvider(const std::string& fieldId, const std::string& luaFnName,
                                                   const std::vector<std::string>& extraScripts,
                                                   std::string& outError) = 0;
    /// Convenience overload — equivalent to passing an empty `extraScripts`.
    virtual bool ScenarioRegisterLuaCachedProvider(const std::string& fieldId, const std::string& luaFnName,
                                                   std::string& outError) = 0;

    /// Inverse of `ScenarioRegisterLuaCachedProvider` — restores the displaced
    /// user-side provider (if any) and drops that field's cache entries.
    virtual void ScenarioUnregisterLuaCachedProvider(const std::string& fieldId) = 0;

    /// Scenario hook: clear the Lua field cache so subsequent cells re-record.
    virtual void ScenarioInvalidateLuaFieldCache() = 0;

    /// Command registry owned by the app, so scenario code can dispatch commands
    /// through the host without a second interface parameter. The single override
    /// on AppController satisfies both this slot and the identical one declared
    /// on IAppCommands — an override with a matching signature implements the
    /// corresponding virtual inherited from every base. By-ref return of a
    /// forward-declared registry needs no completeness here; callers include the
    /// registry header themselves.
    virtual smatchet::cmd::CommandRegistry& Commands() = 0;
    virtual const smatchet::cmd::CommandRegistry& Commands() const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_SCENARIO_HOST_H
