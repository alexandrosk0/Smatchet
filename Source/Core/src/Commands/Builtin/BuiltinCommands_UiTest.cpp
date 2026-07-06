// ui_test.* — ImGui Test Engine bucket-E test runner.

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

namespace smatchet {
namespace cmd {

using builtin_detail::MakeCommand;
using builtin_detail::PString;

void RegisterUiTestCommands(CommandRegistry& reg, IAppScenarios& app) {
    {
        // ui_test.run wraps ui-test scenario start. Registered unconditionally
        // so production builds (SMATCHET_BUILD_UI_TESTS=OFF) still expose the
        // command and return a clear sentinel rather than "command not found".
        // The scenario handler internally pivots on the build gate.
        Command c = MakeCommand("ui_test.run",
                                "Run ImGui Test Engine UI tests (bucket E). --name=<test> for one; --all for every.",
                                [&app](const nlohmann::json& args, const CommandContext& ctx) {
                                    CommandContext ctxCopy = ctx;
                                    return RunOnUiThreadAsCommandResult(app, [&app, args, ctxCopy]() {
                                        nlohmann::json injectedArgs = args;
                                        injectedArgs["name"] = args.value("name", std::string());
                                        return app.Scenarios().Start("ui-test", injectedArgs, ctxCopy);
                                    });
                                });
        c.Destructive = false;
        c.Idempotent = false;
        c.AsyncSafe = false;
        c.DryRunSupported = true;
        ParamSpec allParam;
        allParam.Name = "all";
        allParam.Description = "Run every registered test (ignores --name).";
        allParam.Type = ParamType::Bool;
        allParam.Default = std::make_shared<nlohmann::json>(false);
        c.Params = {
            PString(
                "name",
                "Test filter (wildcard, e.g. 'Views/ColumnsReorder_*'). Empty + all=true runs every registered test."),
            allParam,
            PString("outPath", "Output JSON file path (default: auto-generated in <userData>/ui-test/)."),
            PString("outLog", "Per-test verbose-log dump file path (every test's Output.Log, not just "
                              "failures). Empty = no dump. Used by the bucket-E bash drivers for failure "
                              "diagnosis."),
        };
        reg.Register(std::move(c));
    }
}

} // namespace cmd
} // namespace smatchet
