#ifndef SMATCHET_INTERFACES_IAPP_SCENARIOS_H
#define SMATCHET_INTERFACES_IAPP_SCENARIOS_H

// Narrow facet exposing the ScenarioRunner accessor — AppController fan-in Phase 5
// (docs/plans/appcontroller-fan-in-phase5-facets.md). AppController implements it; the
// scenario.* + ui_test.* command TUs depend on this instead of the full AppController.h,
// so one facet migrates BOTH TUs. No per-frame method. Rank-0 leaf under Interfaces/.
// ScenarioRunner is only named by-reference, so a forward declaration suffices — callers
// that use the returned runner include its defining header directly (IWYU).

namespace smatchet {
namespace cmd {
class ScenarioRunner;
}
} // namespace smatchet

class IAppScenarios {
  public:
    virtual ~IAppScenarios() = default;

    virtual smatchet::cmd::ScenarioRunner& Scenarios() = 0;
    virtual const smatchet::cmd::ScenarioRunner& Scenarios() const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_SCENARIOS_H
