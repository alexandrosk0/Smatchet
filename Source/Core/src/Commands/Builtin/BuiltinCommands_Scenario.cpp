// scenario.* — list / run / cancel registered automation scenarios.

#include "BuiltinCommands_Internal.h"

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/MainThreadDispatch.h"
#include "Commands/Scenarios/IScenario.h"

// fan-in Phase 5: depend on the narrow IAppScenarios facet, not the full AppController.h
// (ScenarioRunner's full definition comes from Commands/Scenarios/IScenario.h, included above).
#include "Interfaces/IAppScenarios.h"
#include <nlohmann/json.hpp> // this TU constructs nlohmann::json directly.

#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PaginateString;
using builtin_detail::PInt;
using builtin_detail::PString;

void RegisterScenarioCommands(CommandRegistry& reg, IAppScenarios& app) {
    // Register scenario factories on the runner owned by AppController.
    // The PriorityGridScrollScenario is the first built-in; others can be
    // added by appending more RegisterFactory calls here.
    // (Registration delegated to AppController::Scenarios().RegisterFactory so
    // the runner lives on AppController, not in the registry.)
    // We register scenario.* CLI commands through the registry as normal;
    // actual execution is delegated to the ScenarioRunner.

    {
        // scenario.list reads factories_ which is set up once in Initialize and never
        // mutated afterwards (built-in factories are registered before any worker is
        // spawned). Safe to read from any thread — no dispatch hop needed.
        Command c = MakeCommand("scenario.list", "List registered scenario names.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    const std::vector<std::string> names = app.Scenarios().ListNames();
                                    return CommandResult::Success(PaginateString(names, 500, 0));
                                });
        reg.Register(std::move(c));
    }

    {
        // scenario.run writes ScenarioRunner::active_ (a unique_ptr) which the UI thread
        // dereferences every frame inside Tick(). Mutating active_ from an MCP worker
        // races with Tick. Hop to UI thread so Start() runs while Tick() is paused
        // between frames.
        Command c =
            MakeCommand("scenario.run", "Run a named automation scenario (perf measurement, scroll driver, etc.).",
                        [&app](const nlohmann::json& args, const CommandContext& ctx) {
                            // Copy ctx into the closure so the UI-thread lambda has the dry-run /
                            // destructive flags (it cannot capture the const reference safely across
                            // a thread boundary — the original ctx may live on the worker stack).
                            CommandContext ctxCopy = ctx;
                            return RunOnUiThreadAsCommandResult(app, [&app, args, ctxCopy]() {
                                const std::string name = args.value("name", std::string());
                                return app.Scenarios().Start(name, args, ctxCopy);
                            });
                        });
        c.Destructive = true;
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        c.Params = {
            PString("name", "Scenario name (from scenario.list).", true),
            PInt("frames", "Frame count to run.", 600),
            PString("outPath", "Output JSON file path (default: auto-generated in <userData>/perf/)."),
        };
        reg.Register(std::move(c));
    }

    {
        // scenario.cancel resets ScenarioRunner::active_ while Tick() may be reading it.
        // Same race as scenario.run — must run on UI thread.
        Command c = MakeCommand("scenario.cancel", "Abort the active running scenario.",
                                [&app](const nlohmann::json&, const CommandContext&) {
                                    return RunOnUiThreadAsCommandResult(app, [&app]() {
                                        const bool was = app.Scenarios().Active();
                                        app.Scenarios().Cancel();
                                        return CommandResult::Success({{"wasCancelled", was}});
                                    });
                                });
        reg.Register(std::move(c));
    }
}

} // namespace cmd
} // namespace smatchet
