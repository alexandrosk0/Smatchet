// CommandRegistry::Dispatch coerces each declared argument to its ParamType before the handler
// runs. An Int param arriving as a JSON float outside long long's range (1e77 from an MCP, CLI
// or Lua caller) must be rejected, never cast: that cast is undefined behaviour.

#include <doctest/doctest.h>

#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include <nlohmann/json.hpp>

#include <string>

using smatchet::cmd::Command;
using smatchet::cmd::CommandContext;
using smatchet::cmd::CommandRegistry;
using smatchet::cmd::CommandResult;
using smatchet::cmd::CommandSource;
using smatchet::cmd::ErrorCode;
using smatchet::cmd::ParamSpec;
using smatchet::cmd::ParamType;

namespace {

// Registers `test.int_echo`, whose handler echoes its coerced `n` argument.
void RegisterIntEcho(CommandRegistry& reg) {
    Command c;
    c.Name = "test.int_echo";
    ParamSpec n;
    n.Name = "n";
    n.Type = ParamType::Int;
    n.Required = true;
    c.Params = {n};
    c.Handler = [](const nlohmann::json& args, const CommandContext&) {
        return CommandResult::Success(nlohmann::json{{"n", args["n"]}});
    };
    reg.Register(c);
}

CommandContext CliCtx() {
    CommandContext ctx;
    ctx.Source = CommandSource::Cli;
    return ctx;
}

} // namespace

TEST_CASE("CommandRegistry: an Int argument given as an in-range float truncates toward zero") {
    CommandRegistry reg;
    RegisterIntEcho(reg);
    const CommandResult r = reg.Dispatch("test.int_echo", {{"n", 42.9}}, CliCtx());
    REQUIRE(r.Ok);
    REQUIRE(r.Data != nullptr);
    CHECK((*r.Data)["n"] == 42);
}

TEST_CASE("CommandRegistry: an Int argument given as an out-of-range float is a validation error") {
    CommandRegistry reg;
    RegisterIntEcho(reg);
    const nlohmann::json bad[] = {nlohmann::json(1e77), nlohmann::json(-1e77), nlohmann::json(9.3e18)};
    for (const nlohmann::json& v : bad) {
        CAPTURE(v.dump());
        const CommandResult r = reg.Dispatch("test.int_echo", {{"n", v}}, CliCtx());
        CHECK_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ValidationError);
    }
}
