#ifndef SMATCHET_COMMANDS_SCENARIOS_PROBE_SCENARIO_H
#define SMATCHET_COMMANDS_SCENARIOS_PROBE_SCENARIO_H

// ProbeScenario — a programmatic (NOT registry-listed) IScenario whose behaviour
// is supplied by caller-provided lambdas, plus a stack-scoped RAII (`ProbeScope`)
// that installs the probe factory into a ScenarioRunner for the duration of a
// scope and removes it on exit.
//
// Motivation: ad-hoc perf / interaction probes (e.g. "drive 120 frames calling
// app.Foo() each frame and snapshot perf") previously needed a one-off .cpp +
// registry edit + stubs edit. ProbeScope lets a test or a debugging session
// register a transient scenario inline, run it, and have it unregister itself —
// no registry-snapshot churn. Behaviour-additive: existing named scenarios and
// every current ProbeScenario-free call site are unaffected.
//
// Core-layer header: no ImGui / GLFW includes (compiles in the DX12 dual-target).

#include <functional>
#include <memory>
#include <string>

class IAppScenarioHost; // narrow scenario-host facet — see Interfaces/IAppScenarioHost.h

namespace smatchet {
namespace cmd {

class IScenario;
class ScenarioRunner;

/// Per-frame callback invoked once per OnFrame with the zero-based frame index.
using ProbeFrameFn = std::function<void(int frameIndex)>;

/// Optional one-shot setup callback invoked from OnStart (before the first frame).
/// Receives the live scenario host so the probe can seed state.
using ProbeSetupFn = std::function<void(IAppScenarioHost& app)>;

/// Build a ProbeScenario. `name` is the scenario name (Start() key). `onFrame`
/// runs each frame for `frames` frames (clamped to >= 1). `setup` (may be null)
/// runs once in OnStart. The returned scenario reports IsDone after `frames`
/// frames and returns a small JSON summary ({name, frames}) from OnFinish.
std::unique_ptr<IScenario> MakeProbeScenario(std::string name, ProbeFrameFn onFrame,
                                             ProbeSetupFn setup = ProbeSetupFn(), int frames = 60);

/// Stack-scoped RAII: registers a ProbeScenario factory under `name` on a
/// ScenarioRunner at construction and unregisters it at destruction. The factory
/// builds a fresh ProbeScenario (via MakeProbeScenario) each time the runner
/// starts it, so the captured lambdas must out-live every Start within the scope
/// (they do — they live in this object). Non-copyable; move is not needed for the
/// stack-scoped usage and is therefore deleted to keep ownership obvious.
/// Name-collision safety: if a factory with `name` is already registered when the
/// scope is constructed, this scope does NOT overwrite or take ownership of it —
/// the dtor then leaves the pre-existing factory untouched. Only a scope that
/// actually installed the factory (registered_ == true) removes it on exit.
class ProbeScope {
  public:
    ProbeScope(ScenarioRunner& runner, std::string name, ProbeFrameFn onFrame, ProbeSetupFn setup = ProbeSetupFn(),
               int frames = 60);
    ~ProbeScope();

    ProbeScope(const ProbeScope&) = delete;
    ProbeScope& operator=(const ProbeScope&) = delete;
    ProbeScope(ProbeScope&&) = delete;
    ProbeScope& operator=(ProbeScope&&) = delete;

    /// The registered scenario name (Start() key).
    const std::string& Name() const { return name_; }

  private:
    ScenarioRunner& runner_;
    std::string name_;
    /// True iff THIS scope's ctor installed the factory (i.e. no factory under
    /// `name_` pre-existed). Guards the dtor so a name collision never erases a
    /// factory this scope did not create.
    bool registered_;
};

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_PROBE_SCENARIO_H
