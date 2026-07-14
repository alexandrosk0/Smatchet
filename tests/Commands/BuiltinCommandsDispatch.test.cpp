// TEST_COVERAGE_GAP_MAP.md Tier 3 (commands-invocation-harness.md Slice 1) — table-driven
// dispatch-layer harness over EVERY registered builtin command. The 24 Builtin/
// BuiltinCommands_*.cpp TUs were compiled into no test target because their registration
// hooks take the AppController god-object; this target links the full-core static archive
// (SmatchetCore_PosixCheck) instead, constructs a real (cheap, headless) AppController,
// and registers all builtins for real — so every command TABLE (names, categories,
// aliases, ParamSpecs, Destructive flags) is exercised, while the matrices below stay
// strictly on the CommandRegistry::Dispatch pre-handler pipeline (unknown-command,
// required/coercion validation, destructive-confirm gate). No app-mutating handler body
// runs in this slice. Matrices are machine-enumerated from reg.All(), so commands added
// tomorrow are covered automatically. Characterization tests — they pin CURRENT behaviour.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "AppController.h"
#include "Commands/BuiltinCommands.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"

#include <nlohmann/json.hpp>

#include <set>
#include <string>
#include <vector>

using smatchet::cmd::Command;
using smatchet::cmd::CommandContext;
using smatchet::cmd::CommandRegistry;
using smatchet::cmd::CommandResult;
using smatchet::cmd::CommandSource;
using smatchet::cmd::ErrorCode;
using smatchet::cmd::ParamSpec;
using smatchet::cmd::ParamType;

namespace {

// One registry over a real (headless) AppController per test case. Registration itself
// is the first assertion: CommandRegistry::Register throws on a duplicate name, so a
// collision anywhere across the 24 builtin TUs fails construction loudly.
struct BuiltinsFixture {
    AppController App;
    CommandRegistry Reg;
    CommandContext Ctx;

    BuiltinsFixture() {
        smatchet::cmd::RegisterBuiltinCommands(Reg, App);
        Ctx.ScenarioHost = &App;
        Ctx.Threading = &App;
        Ctx.Source = CommandSource::Cli; // automation source — destructive gate armed
    }
};

const ParamSpec* FirstRequired(const Command& c) {
    for (const ParamSpec& p : c.Params) {
        if (p.Required) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("builtins — registration inventory invariants over every command table") {
    BuiltinsFixture fx;
    const std::vector<Command> all = fx.Reg.All();

    // Floor, not exact count: new commands must not break the harness, vanishing
    // categories must. 88 registered at the time this slice landed.
    CHECK(all.size() >= 80);

    std::set<std::string> categories;
    std::set<std::string> names;
    for (const Command& c : all) {
        CAPTURE(c.Name);
        CHECK_FALSE(c.Name.empty());
        CHECK_FALSE(c.Category.empty());
        // Category contract: every registered name is dotted and the category is the
        // first dotted segment (the palette/MCP/CLI grouping rule). The last dotless
        // name (`notifications`) was renamed to `view.toggle.notifications`; the bare
        // form lives on only as an alias, so a dotless CANONICAL name is a regression.
        CHECK(c.Name.find('.') != std::string::npos);
        if (c.Name.find('.') != std::string::npos) {
            CHECK(c.Name.rfind(c.Category + ".", 0) == 0);
        }
        CHECK_FALSE(c.Summary.empty());
        CHECK(names.insert(c.Name).second); // All() itself must not carry duplicates
        categories.insert(c.Category);

        std::set<std::string> paramNames;
        for (const ParamSpec& p : c.Params) {
            CAPTURE(p.Name);
            CHECK_FALSE(p.Name.empty());
            CHECK(paramNames.insert(p.Name).second); // no duplicate param names
        }
    }

    // The category floor is the FULL category set at the time this slice landed — a
    // bucket TU that silently stops registering is exactly the regression class this
    // harness exists for. New categories may appear; these must persist.
    for (const char* expected :
         {"app", "attach", "automation", "bug", "commands", "config", "debug", "fields", "grid", "offline", "perf",
          "scenario", "sync", "ticket", "tickets", "ui", "ui_test", "users", "view"}) {
        CAPTURE(expected);
        CHECK(categories.count(expected) == 1);
    }

    // Alias contract: every alias resolves back to its canonical command.
    for (const Command& c : all) {
        for (const std::string& alias : c.Aliases) {
            CAPTURE(c.Name);
            CAPTURE(alias);
            const Command* resolved = fx.Reg.FindLocked(alias);
            REQUIRE(resolved != nullptr);
            CHECK(resolved->Name == c.Name);
        }
    }
}

TEST_CASE("registry — Contains is alias-aware and mirrors FindLocked existence (BACKLOG N3)") {
    BuiltinsFixture fx;

    // Contains must agree with FindLocked's existence result for every canonical name AND
    // every alias. This is the invariant the MCP tools/call dispatch relies on after moving
    // its existence check off the FindLocked-pointer footgun: a legacy tool name registered
    // as an alias must still route to the registry, not fall through to a fallback handler.
    for (const Command& c : fx.Reg.All()) {
        CAPTURE(c.Name);
        CHECK(fx.Reg.Contains(c.Name));
        CHECK(fx.Reg.HasExact(c.Name)); // canonical names are also exact hits
        for (const std::string& alias : c.Aliases) {
            CAPTURE(alias);
            CHECK(fx.Reg.Contains(alias));       // alias-aware — the whole point
            CHECK_FALSE(fx.Reg.HasExact(alias)); // distinct from HasExact, which is exact-only
        }
    }

    // Pin the concrete legacy-MCP aliases the migration must not regress.
    CHECK(fx.Reg.Contains("list_active_tickets"));
    CHECK(fx.Reg.Contains("search_active_tickets"));

    // Misses: an unregistered name resolves to neither.
    CHECK_FALSE(fx.Reg.Contains("nope.not.a.command"));
    CHECK_FALSE(fx.Reg.Contains(""));
    CHECK_FALSE(fx.Reg.HasExact("nope.not.a.command"));
}

TEST_CASE("builtins — every command with a required param rejects {} before its handler") {
    BuiltinsFixture fx;
    int exercised = 0;
    for (const Command& c : fx.Reg.All()) {
        const ParamSpec* req = FirstRequired(c);
        if (req == nullptr) {
            continue;
        }
        CAPTURE(c.Name);
        const CommandResult r = fx.Reg.Dispatch(c.Name, nlohmann::json::object(), fx.Ctx);
        CHECK_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::MissingRequiredArg);
        // The failure names a real parameter of this command (the first required one in
        // declaration order — pinned so error UX stays stable).
        REQUIRE(r.Error.Details != nullptr);
        CHECK((*r.Error.Details)["param"].get<std::string>() == FirstRequired(c)->Name);
        ++exercised;
    }
    CHECK(exercised >= 25); // 28 at the time this slice landed
}

TEST_CASE("builtins — wrong-typed first param is a ValidationError before any handler") {
    BuiltinsFixture fx;
    int exercised = 0;
    for (const Command& c : fx.Reg.All()) {
        if (c.Params.empty() || c.Params.front().Type == ParamType::Json) {
            continue; // Json params accept arbitrary shapes by design
        }
        const ParamSpec& first = c.Params.front();
        CAPTURE(c.Name);
        CAPTURE(first.Name);
        // A JSON array fails coercion for String, Int, Number, and Bool alike — the
        // universal wrong-typed probe. Validation walks Params in order, so a present
        // first param that fails coercion errors deterministically regardless of any
        // later required params.
        const nlohmann::json args = {{first.Name, nlohmann::json::array()}};
        const CommandResult r = fx.Reg.Dispatch(c.Name, args, fx.Ctx);
        CHECK_FALSE(r.Ok);
        CHECK(r.Error.Code == ErrorCode::ValidationError);
        REQUIRE(r.Error.Details != nullptr);
        CHECK((*r.Error.Details)["param"].get<std::string>() == first.Name);
        ++exercised;
    }
    CHECK(exercised >= 30);
}

TEST_CASE("builtins — destructive-confirm gate fires for automation sources") {
    BuiltinsFixture fx;
    int gateChecked = 0;
    int validationFirst = 0;
    for (const Command& c : fx.Reg.All()) {
        if (!c.Destructive) {
            continue;
        }
        CAPTURE(c.Name);
        const CommandResult r = fx.Reg.Dispatch(c.Name, nlohmann::json::object(), fx.Ctx);
        CHECK_FALSE(r.Ok); // unconfirmed destructive from CLI must never reach the handler
        if (FirstRequired(c) == nullptr) {
            // No required params → validation passes → the confirm gate is the stopper.
            CHECK(r.Error.Code == ErrorCode::ConfirmRequired);
            ++gateChecked;
        } else {
            // Characterization: validation runs BEFORE the destructive gate, so missing
            // args surface first (better UX; the gate still holds once args validate).
            CHECK(r.Error.Code == ErrorCode::MissingRequiredArg);
            ++validationFirst;
        }
    }
    CHECK(gateChecked + validationFirst >= 3); // the registry carries real destructive commands
}

TEST_CASE("builtins — unknown command gets fuzzy suggestions, never a handler") {
    BuiltinsFixture fx;
    const CommandResult r = fx.Reg.Dispatch("tickets.list_actve", nlohmann::json::object(), fx.Ctx);
    CHECK_FALSE(r.Ok);
    CHECK(r.Error.Code == ErrorCode::UnknownCommand);
    CHECK_FALSE(r.Error.Message.empty());
}

TEST_CASE("builtins — ByCategory partitions the full registry") {
    BuiltinsFixture fx;
    const std::vector<Command> all = fx.Reg.All();
    std::set<std::string> categories;
    for (const Command& c : all) {
        categories.insert(c.Category);
    }
    std::size_t sum = 0;
    for (const std::string& cat : categories) {
        sum += fx.Reg.ByCategory(cat).size();
    }
    CHECK(sum == all.size());
}

TEST_CASE("builtins — issue.quick_create.open registers as a non-destructive opener") {
    // quick-create-issue-unreal-context plan: the popup opener must reach every
    // front-end from one registration (palette/CLI/MCP/Lua/Unreal console) and is
    // read-only until the user submits — Destructive would wrongly arm the confirm
    // gate for automation sources. Handler body (UI-thread flag write) is not run
    // here, matching this harness's dispatch-layer-only contract.
    BuiltinsFixture fx;
    const Command* c = fx.Reg.FindLocked("issue.quick_create.open");
    REQUIRE(c != nullptr);
    CHECK(c->Category == "issue");
    CHECK_FALSE(c->Destructive);
    CHECK(c->Idempotent);
    CHECK(c->AsyncSafe);
}
