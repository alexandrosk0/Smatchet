#ifndef SMATCHET_COMMANDS_SCENARIOS_ISCENARIO_H
#define SMATCHET_COMMANDS_SCENARIOS_ISCENARIO_H

// Scenario subsystem for the unified Command System.
// See docs/design/applied/command-system-plan.md §"Scenario subsystem".
//
// A scenario is a multi-frame automated test that runs inside the normal
// render loop. It is driven by ScenarioRunner::Tick (called once per frame
// from SmatchetUI::Draw) and lets the perf workflow measure real-frame timings
// under controlled scroll/interaction patterns without a human driver.
//
// Adding a new scenario = one new .cpp + one RegisterFactory line in
// BuiltinCommands.cpp.

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class AppController;

namespace smatchet {
namespace cmd {

struct CommandContext;
struct CommandResult;

/// Interface every scenario must implement.
class IScenario {
public:
    virtual ~IScenario() = default;

    virtual std::string Name() const = 0;

    /// Called once before the first Tick.
    /// @param outErr  Non-empty on failure; runner aborts without calling OnFrame/OnFinish.
    virtual void OnStart(AppController& app, const nlohmann::json& args, std::string& outErr) = 0;

    /// Called once per frame until IsDone returns true.
    virtual void OnFrame(AppController& app, int frameIndex) = 0;

    /// Return true when the scenario has completed its work.
    virtual bool IsDone(int frameIndex) const = 0;

    /// Called after the final frame. Return the result payload (written to disk + returned to caller).
    virtual nlohmann::json OnFinish(AppController& app) = 0;

    /// Called by ScenarioRunner::Cancel when the user aborts the scenario mid-run. Default
    /// no-op. Scenarios that mutate persistent app state in OnStart (e.g. registering a
    /// transient Lua provider) must override this to unwind that state — Cancel does NOT
    /// invoke OnFinish, so cleanup needs its own hook. The runner passes the same
    /// `AppController&` it stashed at Start time.
    virtual void OnCancel(AppController& /*app*/) {}

    /// Optional: return the scroll-Y target the runner should propagate to the grid this
    /// frame. Return -1 (default) to leave the grid alone. Scenarios that drive scroll
    /// override this; the runner reads it after each OnFrame call.
    virtual int CurrentScrollY() const { return -1; }
};

/// Owns the active scenario instance, drives it from SmatchetUI::Draw, and
/// optionally writes results to disk.
class ScenarioRunner {
public:
    using Factory = std::function<std::unique_ptr<IScenario>()>;

    void RegisterFactory(const std::string& name, Factory f);

    /// Start a named scenario. Returns error result immediately if not found or start fails.
    CommandResult Start(const std::string& name, const nlohmann::json& args, const CommandContext& ctx);

    /// Called once per frame from SmatchetUI::Draw.
    void Tick(AppController& app, bool& outScrollActiveOut, int& outScrollTargetOut);

    void Cancel();
    bool Active() const { return active_ != nullptr; }
    std::vector<std::string> ListNames() const;

private:
    std::unique_ptr<IScenario> active_;
    int frame_ = 0;
    std::string outPath_;
    std::unordered_map<std::string, Factory> factories_;
    /// Stashed AppController pointer captured at Start() time so Cancel() can drive the
    /// active scenario's OnCancel hook even though Cancel() takes no args. Cleared on
    /// scenario finish / cancel. Lifetime: AppController owns the runner, so the pointer is
    /// always valid while `active_` is set.
    AppController* app_ = nullptr;
};

}  // namespace cmd
}  // namespace smatchet

#endif  // SMATCHET_COMMANDS_SCENARIOS_ISCENARIO_H
