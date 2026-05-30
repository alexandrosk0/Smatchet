#ifndef SMATCHET_COMMANDS_SCENARIOS_REGISTRY_H
#define SMATCHET_COMMANDS_SCENARIOS_REGISTRY_H

// Slice 5 of docs/plans/shipped/autonomous-debugging-no-creds.md — pure refactor:
// extract the 14-entry scenario-factory registration block out of
// AppController::Initialize so adding/removing a scenario is one edit in a
// single self-contained TU rather than threading a line through the giant
// Initialize function.
//
// Adding a new scenario = one new .cpp + one new entry in this registry
// (replacing the historical "edit AppController.cpp" step in IScenario.h's
// header comment — that comment will be updated in a follow-up doc-only pass).
//
// Behaviour contract: RegisterAllScenarios() must register exactly the same
// set of factories, in the same order, under the same names, with the same
// ifdef gating, as the pre-refactor block. The snapshot test
// tests/Core/SmatchetScenarioRegistry.test.cpp pins this contract.

namespace smatchet {
namespace cmd {

class ScenarioRunner;

/// Register every built-in scenario factory with `runner`. Idempotent only in
/// the sense that re-calling it will RegisterFactory the same names again
/// (which overwrites the prior entry in ScenarioRunner's std::unordered_map);
/// production code should call it exactly once during AppController init.
void RegisterAllScenarios(ScenarioRunner& runner);

}  // namespace cmd
}  // namespace smatchet

#endif  // SMATCHET_COMMANDS_SCENARIOS_REGISTRY_H
