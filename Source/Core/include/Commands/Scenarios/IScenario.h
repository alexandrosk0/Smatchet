#ifndef SMATCHET_COMMANDS_SCENARIOS_ISCENARIO_H
#define SMATCHET_COMMANDS_SCENARIOS_ISCENARIO_H

// Scenario subsystem for the unified Command System.
// See docs/plans/shipped/command-system-plan.md §"Scenario subsystem".
// A scenario is a multi-frame automated test that runs inside the normal
// render loop. It is driven by ScenarioRunner::Tick (called once per frame
// from SmatchetUI::Draw) and lets the perf workflow measure real-frame timings
// under controlled scroll/interaction patterns without a human driver.
// Adding a new scenario = one new .cpp + one RegisterFactory line in
// Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp (the
// snapshot test in tests/Core/SmatchetScenarioRegistry.test.cpp
// pins the registered name set — also update the stub list in
// tests/Core/SmatchetScenarioRegistry.stubs.cpp).

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Build-time win: this header names nlohmann::json only by const-reference
// (OnStart/Start args) and as an out-of-line return-type declaration
// (OnFinish) — never by value, never in an inline body — so the forward-decl
// header suffices. Scenario .cpp implementations that build/return json values
// include the full <nlohmann/json.hpp> directly.
#include <nlohmann/json_fwd.hpp>

class AppController;
class IAppScenarioHost; // narrow scenario-host facet of AppController — see Interfaces/IAppScenarioHost.h

namespace smatchet {
namespace cmd {

struct CommandContext;
struct CommandResult;

/// Recover the concrete AppController inside a scenario hook. The runner only
/// ever drives scenarios with the owning AppController — the sole
/// IAppScenarioHost implementer — so the downcast is safe from any hook the
/// runner invokes. Single seam for that invariant; today only the ui-test
/// scenario needs it, to feed the active-app pointer its bucket-E test
/// functions read back. Defined in UiTestScenario.cpp, the sole remaining
/// caller, which already holds the complete type.
AppController& RequireConcreteController(IAppScenarioHost& app);

/// Interface every scenario must implement.
class IScenario {
  public:
    virtual ~IScenario() = default;

    virtual std::string Name() const = 0;

    /// Called once before the first Tick.
    /// @param outErr  Non-empty on failure; runner aborts without calling OnFrame/OnFinish.
    virtual void OnStart(IAppScenarioHost& app, const nlohmann::json& args, std::string& outErr) = 0;

    /// Called once per frame until IsDone returns true.
    virtual void OnFrame(IAppScenarioHost& app, int frameIndex) = 0;

    /// Return true when the scenario has completed its work.
    virtual bool IsDone(int frameIndex) const = 0;

    /// Called after the final frame. Return the result payload (written to disk + returned to caller).
    virtual nlohmann::json OnFinish(IAppScenarioHost& app) = 0;

    /// Called by ScenarioRunner::Cancel when the user aborts the scenario mid-run. Default
    /// no-op. Scenarios that mutate persistent app state in OnStart (e.g. registering a
    /// transient Lua provider) must override this to unwind that state — Cancel does NOT
    /// invoke OnFinish, so cleanup needs its own hook. The runner passes the same
    /// scenario-host reference it stashed at Start time.
    virtual void OnCancel(IAppScenarioHost& /*app*/) {}

    /// Optional: return the scroll-Y target the runner should propagate to the grid this
    /// frame. Return -1 (default) to leave the grid alone. Scenarios that drive scroll
    /// override this; the runner reads it after each OnFrame call.
    virtual int CurrentScrollY() const { return -1; }

    /// Leading frames whose perf samples are warmup and must be excluded from
    /// the snapshot p99 population. Default 0 = no exclusion (unchanged for
    /// scenarios that don't opt in, incl. the ten that Reset() in OnStart). When
    /// > 0, ScenarioRunner::Tick calls UiPerfMonitor::Reset() once after this
    /// many frames, so the rings feeding ComputeP99 hold only steady-state — the
    /// one-time cold-start spikes (font-atlas, first-frame layout, initial sync)
    /// that otherwise dominate p99 and trip the absolute ceiling are dropped.
    /// See tooling.md `p99-gate-warmup-frame-exclusion`.
    virtual int WarmupFrames() const { return 0; }
};

/// Owns the active scenario instance, drives it from SmatchetUI::Draw, and
/// optionally writes results to disk.
class ScenarioRunner {
  public:
    using Factory = std::function<std::unique_ptr<IScenario>()>;

    void RegisterFactory(const std::string& name, Factory f);

    /// Remove a previously-registered factory. No-op if `name` was never registered.
    /// Used by ProbeScope (the stack-scoped programmatic-scenario RAII) to unwind a
    /// transient factory it installed; never touches the built-in registry entries
    /// unless a caller passes one of their names. Returns the number erased (0 or 1).
    std::size_t UnregisterFactory(const std::string& name);

    /// Start a named scenario. Returns error result immediately if not found or start fails.
    CommandResult Start(const std::string& name, const nlohmann::json& args, const CommandContext& ctx);

    /// Called once per frame from SmatchetUI::Draw.
    void Tick(IAppScenarioHost& app, bool& outScrollActiveOut, int& outScrollTargetOut);

    void Cancel();
    bool Active() const { return active_ != nullptr; }
    std::vector<std::string> ListNames() const;

  private:
    std::unique_ptr<IScenario> active_;
    int frame_ = 0;
    /// Latched true once the runner has performed the one-time warmup-frame
    /// UiPerfMonitor::Reset() for the active scenario (WarmupFrames() > 0).
    /// Prevents re-clearing the ring every frame after the warmup boundary.
    /// Reset to false on each Start()/finish/Cancel so the next run re-arms.
    bool warmupResetDone_ = false;
    std::string outPath_;
    std::unordered_map<std::string, Factory> factories_;
    /// Stashed scenario-host pointer captured at Start() time so Cancel() can drive the
    /// active scenario's OnCancel hook even though Cancel() takes no args. Cleared on
    /// scenario finish / cancel. Lifetime: AppController owns the runner, so the pointer is
    /// always valid while `active_` is set.
    IAppScenarioHost* app_ = nullptr;
};

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_SCENARIOS_ISCENARIO_H
