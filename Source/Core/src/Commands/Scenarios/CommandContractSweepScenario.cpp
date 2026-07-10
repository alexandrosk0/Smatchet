// CommandContractSweepScenario — registry-wide command CONTRACT sweep (gap map Tier 3).
// The 24 BuiltinCommands_* registration TUs feed four frontends (CLI, palette, MCP, Lua)
// yet their ParamSpecs and guard behaviour had no automated exercise. This scenario
// dispatches EVERY registered command through the real CommandRegistry guard path with
// deliberately invalid / unconfirmed inputs and asserts the error-envelope contract:
//   A. missing required arg               -> missing-required-arg naming the param
//   B. un-coercible arg (array for scalar) -> validation-error
//   C. destructive + unconfirmed automation source -> confirm-required
//   D. unknown command name               -> unknown-command
// Every probe is rejected BEFORE the handler runs (Dispatch guards 2-4 precede invoke),
// so the sweep is mutation-free by construction. Commands with no probeable surface
// (zero params, non-destructive) are counted as skipped, never silently "covered" —
// dispatching them would execute their handler.

#include "Commands/Scenarios/IScenario.h"

// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=IScenario passes AppController& and Commands() has no narrower
// interface yet; owner=command-system; revisit=2026-12-31)
#include "AppController.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/ParamValueSynthPure.h" // SynthValidValue / FirstRequiredParam / SynthValidRequiredArgs / FirstScalarParam

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace smatchet {
namespace cmd {

// SynthValidValue / FirstRequiredParam / SynthValidRequiredArgs / FirstScalarParam now
// live in Commands/ParamValueSynthPure.h (shared with tests/monkey). Behaviour is
// unchanged — the definitions moved verbatim; the calls below resolve to them via ADL
// within namespace smatchet::cmd.

class CommandContractSweepScenario : public IScenario {
  public:
    std::string Name() const override { return "command-contract-sweep"; }

    void OnStart(IAppScenarioHost& /*app*/, const nlohmann::json& /*args*/, std::string& /*outErr*/) override {}

    void OnFrame(IAppScenarioHost& app, int frameIndex) override {
        if (frameIndex > 0) {
            return;
        }
        RunSweep(RequireConcreteController(app));
    }

    bool IsDone(int frameIndex) const override { return frameIndex >= 1; }

    nlohmann::json OnFinish(IAppScenarioHost& /*app*/) override { return summary_; }

  private:
    void AddViolation(const std::string& command, const std::string& probe, const std::string& expected,
                      const CommandResult& got) {
        nlohmann::json v;
        v["command"] = command;
        v["probe"] = probe;
        v["expected"] = expected;
        v["gotOk"] = got.Ok;
        v["gotCode"] = ErrorCodeString(got.Error.Code);
        v["gotMessage"] = got.Error.Message;
        violations_.push_back(std::move(v));
    }

    void RunSweep(AppController& app) {
        CommandRegistry& reg = app.Commands();

        CommandContext automationCtx;
        automationCtx.App = &app;
        automationCtx.Source = CommandSource::Mcp; // automation posture: audit-logged, no confirm bypass

        int checked = 0;
        int skippedNoProbe = 0;
        int inconclusive = 0;

        // Probe D (once): a name that cannot exist must produce unknown-command.
        {
            const CommandResult r =
                reg.Dispatch("contract.sweep.no_such_command", nlohmann::json::object(), automationCtx);
            if (r.Ok || r.Error.Code != ErrorCode::UnknownCommand) {
                AddViolation("contract.sweep.no_such_command", "unknown-name", "unknown-command", r);
            }
        }

        const std::vector<Command> all = reg.All();
        for (const Command& c : all) {
            bool probed = false;

            // Probe A: omit a required-without-default param entirely.
            if (const ParamSpec* req = FirstRequiredParam(c)) {
                const CommandResult r = reg.Dispatch(c.Name, nlohmann::json::object(), automationCtx);
                const bool namesParam = r.Error.Message.find("'" + req->Name + "'") != std::string::npos;
                if (r.Ok || r.Error.Code != ErrorCode::MissingRequiredArg || !namesParam) {
                    AddViolation(c.Name, "missing-required:" + req->Name, "missing-required-arg naming the param", r);
                }
                probed = true;
            }

            // Probe B: a JSON array can never coerce to a scalar param type, so this must
            // die in validation regardless of the other args (required ones are filled so
            // the failure is attributable to the probed param).
            if (const ParamSpec* scalar = FirstScalarParam(c)) {
                nlohmann::json args = SynthValidRequiredArgs(c);
                args[scalar->Name] = nlohmann::json::array();
                const CommandResult r = reg.Dispatch(c.Name, args, automationCtx);
                if (r.Ok || r.Error.Code != ErrorCode::ValidationError) {
                    AddViolation(c.Name, "wrong-type:" + scalar->Name, "validation-error", r);
                }
                probed = true;
            }

            // Probe C: a destructive command dispatched from an automation source without
            // the explicit confirm signal must stop at the confirm gate. Synthesized args
            // aim to clear validation; when a spec is too constrained for the synthesizer
            // the probe is inconclusive (recorded, not a violation).
            if (c.Destructive) {
                const CommandResult r = reg.Dispatch(c.Name, SynthValidRequiredArgs(c), automationCtx);
                if (r.Ok) {
                    // A destructive handler EXECUTED without confirmation — the exact
                    // failure mode the security-audit confirm gate exists to prevent.
                    AddViolation(c.Name, "destructive-unconfirmed", "confirm-required", r);
                } else if (r.Error.Code != ErrorCode::ConfirmRequired) {
                    if (r.Error.Code == ErrorCode::MissingRequiredArg || r.Error.Code == ErrorCode::ValidationError) {
                        ++inconclusive; // synthesizer couldn't satisfy this spec; guard untested here
                    } else {
                        AddViolation(c.Name, "destructive-unconfirmed", "confirm-required", r);
                    }
                }
                probed = true;
            }

            if (probed) {
                ++checked;
            } else {
                ++skippedNoProbe; // zero-param non-destructive: any dispatch would run the handler
            }
        }

        summary_["commandsTotal"] = all.size();
        summary_["checked"] = checked;
        summary_["skippedNoProbe"] = skippedNoProbe;
        summary_["inconclusive"] = inconclusive;
        summary_["violations"] = violations_;
        summary_["ok"] = violations_.empty();
    }

    nlohmann::json summary_ = nlohmann::json::object();
    nlohmann::json violations_ = nlohmann::json::array();
};

} // namespace cmd
} // namespace smatchet

std::unique_ptr<smatchet::cmd::IScenario> MakeCommandContractSweepScenario() {
    return std::make_unique<smatchet::cmd::CommandContractSweepScenario>();
}
