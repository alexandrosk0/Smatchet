// LuaBindings.test.cpp -- end-to-end round-trip of the `smatchet::lua::InitLuaCore`
// surface (`_plan-locks` § Phase-6-unblocker).
// Drives the `Ticket` usertype + `smatchet` / `tracker` / `mcp` / `commands`
// global tables through a `FakeLuaBindingHost`, asserting:
//   - `CachedTicket` round-trips C++ -> Lua -> C++ via `smatchet.get_ticket`
//     ([high-risk] -- the get_ticket glue is what `tracker-fixture` style tests
//     prove byte-clean).
//   - Lua-side errors propagate as the production-shape tuple `(nil, err)`.
//   - `Ticket:set_field` + `Ticket:transition` forward through to
//     `ILuaBindingHost::SubmitFieldEdit` with the right values.
//   - `mcp.register_tool` records the registration callback.
//   - `commands.invoke` returns the `{ok, data, error}` table shape and routes
//     through the host's `CommandRegistry`.
//   - The ImGui-free `InitLuaCore` does NOT regress the `LuaSandbox.test.cpp`
//     closure invariant -- see the dedicated case at the bottom.
//
// Test target: `SmatchetLuaTests`. Production .cpp linked into the same target
// (`AppController_LuaBindingsCore.cpp`) -- direct cast site exercised.

#include "FakeLuaBindingHost.h"
#include "LuaHostFixture.h"

#include "CachedTicketTypes.h"
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "ILuaBindingHost.h"
#include "TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet_test::LuaHostFixture;
using smatchet_tests::FakeLuaBindingHost;

namespace {

// Boots a sol::state mirroring production via `smatchet::lua::InitLuaCore`,
// then re-installs the panic-as-exception handler + the os whitelist (both of
// which `InitLuaCore` honours but the doctest runner relies on for safety).
// Returns by value so each TEST_CASE owns its own state -- no cross-case
// leakage.
struct CoreFixture {
    LuaHostFixture lua;
    FakeLuaBindingHost host;

    CoreFixture() {
        // InitLuaCore stashes `host` into state["__smatchet_app"]; the glues
        // resolve through that key. Tests do NOT set `__smatchet_app_ui` --
        // UI glues are not in this slice's scope.
        smatchet::lua::InitLuaCore(lua.state(), &host);
        // FakeLuaBindingHost needs the lua_State* to construct sol::object
        // instances inside the LuaGetTicketBind / LuaDecodeJsonBind callbacks.
        host.AttachState(lua.state());
    }

    sol::state& state() { return lua.state(); }
};

// Helper: run a Lua chunk, REQUIRE it didn't error. The chunk can leave a
// result on the stack (which doctest can pull via `.get<T>()`).
sol::protected_function_result Run(sol::state& L, const char* src) {
    sol::protected_function_result r = L.safe_script(src, sol::script_pass_on_error);
    REQUIRE(r.valid());
    return r;
}

} // namespace

// =============================================================================
// Ticket usertype round-trip
// =============================================================================

// * doctest::test_suite("[high-risk]") -- forces
// `AppController_LuaBindingsCore.cpp::LuaGetTicketGlue` (~line 188) -> the
// `host->LuaGetTicketBind(issueId)` round-trip. If the glue body ever stopped
// returning the host's tuple verbatim (e.g. a refactor that lost the field
// payload), the `t:get_field("summary") == "x"` assertion below fails.
// Mutation-sanity per backlog #48 taxonomy path 2.
TEST_CASE("Lua bindings · smatchet.get_ticket round-trips CachedTicket" * doctest::test_suite("[high-risk]")) {
    CoreFixture fx;

    CachedTicket t;
    t.id = "ABC-1";
    t.fieldValues["summary"] = "x";
    t.fieldValues["priority"] = "High";
    fx.host.TicketsById["ABC-1"] = t;

    sol::protected_function_result r = Run(fx.state(), R"(
        local tk, err = smatchet.get_ticket("ABC-1")
        if not tk then return tostring(err) end
        return tk.id .. "|" .. tk:get_field("summary") .. "|" .. tk:get_field("priority")
    )");
    CHECK_EQ(r.get<std::string>(), std::string("ABC-1|x|High"));
}

// * doctest::test_suite("[high-risk]") -- forces the failure-path tuple
// `(sol::nil, "not found")` from `LuaGetTicketGlue`. If the glue body lost the
// error-string passthrough (e.g. always returned `("", "")`), the script's
// `err ~= ""` assertion would flip false.
TEST_CASE("Lua bindings · smatchet.get_ticket returns (nil, err) on miss" * doctest::test_suite("[high-risk]")) {
    CoreFixture fx;
    // No tickets registered.

    sol::protected_function_result r = Run(fx.state(), R"(
        local tk, err = smatchet.get_ticket("MISSING")
        local tk_is_nil = (tk == nil)
        local err_nonempty = (type(err) == "string" and #err > 0)
        return tk_is_nil and err_nonempty
    )");
    CHECK_EQ(r.get<bool>(), true);
}

TEST_CASE("Lua bindings · smatchet.get_active_tickets exposes N-element list") {
    CoreFixture fx;

    CachedTicket a;
    a.id = "A-1";
    CachedTicket b;
    b.id = "B-2";
    CachedTicket c;
    c.id = "C-3";
    fx.host.ActiveTickets = {a, b, c};

    sol::protected_function_result r = Run(fx.state(), R"(
        local arr = smatchet.get_active_tickets()
        local ids = ""
        for i, t in ipairs(arr) do ids = ids .. t.id .. "," end
        return #arr, ids
    )");
    const int n = r.get<int>(0);
    const std::string ids = r.get<std::string>(1);
    CHECK_EQ(n, 3);
    CHECK_EQ(ids, std::string("A-1,B-2,C-3,"));
}

// =============================================================================
// log_info + decode_json
// =============================================================================

TEST_CASE("Lua bindings · log_info forwards into host LoggedInfo") {
    CoreFixture fx;

    Run(fx.state(), R"(log_info("hello"); log_info("world"))");

    REQUIRE_EQ(fx.host.LoggedInfo.size(), 2u);
    CHECK_EQ(fx.host.LoggedInfo[0], std::string("hello"));
    CHECK_EQ(fx.host.LoggedInfo[1], std::string("world"));
}

TEST_CASE("Lua bindings · decode_json valid -> table, invalid -> nil + err") {
    CoreFixture fx;

    sol::protected_function_result r1 = Run(fx.state(), R"(
        local t, err = decode_json('{"a":1,"b":"two"}')
        if not t then return "ERR:" .. tostring(err) end
        return tostring(t.a) .. "|" .. tostring(t.b)
    )");
    CHECK_EQ(r1.get<std::string>(), std::string("1.0|two"));

    sol::protected_function_result r2 = Run(fx.state(), R"(
        local t, err = decode_json('{{not-json')
        local t_is_nil = (t == nil)
        local err_nonempty = (type(err) == "string" and #err > 0)
        return t_is_nil and err_nonempty
    )");
    CHECK_EQ(r2.get<bool>(), true);
}

// Security finding A (SECURITY_AUDIT, Pillar 3 — Never crash): `decode_json`
// parses ATTACKER-CONTROLLED text. nlohmann's recursive parser overflows the C++
// stack on a deeply-nested payload BEFORE conversion. The bounded SAX parse must
// reject past the depth cap (and the node cap) with a graceful (nil, err) tuple —
// NOT a crash, NOT a C++ throw across the sol2 boundary. The deep string is built
// in Lua (not a C++ nlohmann::json) so there is no deep C++ tree to construct or
// recursively destruct in the fixture.
TEST_CASE("Lua bindings · decode_json rejects over-deep nesting without crashing" *
          doctest::test_suite("[high-risk]")) {
    CoreFixture fx;

    // 5000 '[' then 5000 ']' — depth 5000, well past kDecodeJsonMaxDepth (256).
    // Production stack-overflows here without the bound; with it we expect a clean
    // rejection.
    sol::protected_function_result r = Run(fx.state(), R"(
        local n = 5000
        local s = string.rep("[", n) .. string.rep("]", n)
        local t, err = decode_json(s)
        local t_is_nil = (t == nil)
        local err_nonempty = (type(err) == "string" and #err > 0)
        return t_is_nil and err_nonempty
    )");
    CHECK_EQ(r.get<bool>(), true);

    // Shallow-but-legitimate JSON still decodes after the deep rejection — the cap
    // did not poison the fixture or leave the parser in a broken state.
    sol::protected_function_result ok = Run(fx.state(), R"(
        local t, err = decode_json('{"a":[1,2,3]}')
        if not t then return "ERR:" .. tostring(err) end
        return tostring(t.a[1]) .. "|" .. tostring(t.a[3])
    )");
    CHECK_EQ(ok.get<std::string>(), std::string("1.0|3.0"));
}

// Security finding A — node-count cap. A wide-but-shallow payload (a single array
// with > kDecodeJsonMaxNodes elements) must also be rejected gracefully: depth
// alone does not bound total allocation.
TEST_CASE("Lua bindings · decode_json rejects excessive node count" *
          doctest::test_suite("[high-risk]")) {
    CoreFixture fx;

    // A flat array of 250000 zeros (> kDecodeJsonMaxNodes = 200000).
    sol::protected_function_result r = Run(fx.state(), R"(
        local s = "[" .. string.rep("0,", 249999) .. "0]"
        local t, err = decode_json(s)
        local t_is_nil = (t == nil)
        local err_nonempty = (type(err) == "string" and #err > 0)
        return t_is_nil and err_nonempty
    )");
    CHECK_EQ(r.get<bool>(), true);
}

// =============================================================================
// smatchet.create_issue — state-relative marshalling (cross-thread race fix)
// =============================================================================

// * doctest::test_suite("[high-risk]") — guards the state-relative marshalling
// contract introduced by docs/plans/shipped/mcp-lua-fresh-state-race.md. The three
// sol::object-returning binds (LuaGetTicketBind / LuaDecodeJsonBind /
// LuaCreateIssueBind) now take the *calling* sol::state_view and build their
// result on it, never on a member state — required so MCP / automation workers
// can run on a fresh per-call state without touching (or returning objects bound
// to) the UI-thread `lua`. This drives smatchet.create_issue end-to-end through
// LuaCreateIssueGlue → host->LuaCreateIssueBind(sv, spec): the result table is
// built on the script's state and read back by the script, so a regression that
// reverted the bind to marshal on a member state (cross-state object) would fail
// the readback here. create_issue's result-table path is otherwise uncovered.
TEST_CASE("Lua bindings · smatchet.create_issue marshals its result on the calling state" *
          doctest::test_suite("[high-risk]")) {
    CoreFixture fx;

    // The fake builds the result table on the sol::state_view it is handed — i.e.
    // the calling script's state when sv is threaded correctly through the glue.
    fx.host.CreateIssueScripter = [](sol::state_view sv) {
        sol::table t = sv.create_table();
        t["ok"] = true;
        t["issue_key"] = std::string("NEW-42");
        return t;
    };

    sol::protected_function_result r = Run(fx.state(), R"(
        local res, err = smatchet.create_issue({ summary = "hello", project = "ABC" })
        if not res then return "ERR:" .. tostring(err) end
        return tostring(res.ok) .. "|" .. tostring(res.issue_key)
    )");
    CHECK_EQ(r.get<std::string>(), std::string("true|NEW-42"));

    // The spec table marshalled inbound on the calling state too — assert the
    // actual keys arrived, not just that the call happened.
    REQUIRE_EQ(fx.host.CreateIssueSpecKeys.size(), 1u);
    const std::vector<std::string>& keys = fx.host.CreateIssueSpecKeys[0];
    bool sawSummary = false;
    bool sawProject = false;
    for (const std::string& k : keys) {
        if (k == "summary") {
            sawSummary = true;
        }
        if (k == "project") {
            sawProject = true;
        }
    }
    CHECK(sawSummary);
    CHECK(sawProject);
}

// =============================================================================
// Ticket:set_field + Ticket:transition
// =============================================================================

// * doctest::test_suite("[high-risk]") -- forces
// `AppController_LuaBindingsCore.cpp::TicketSetFieldGlue` (~line 145) ->
// `host->FindFieldById` + `host->SubmitFieldEdit`. If the glue stopped pushing
// the single-element vector (e.g. dropped `vals.push_back(val)`), the
// recorded `Values[0] == "High"` assertion would fail.
TEST_CASE("Lua bindings · Ticket:set_field forwards to SubmitFieldEdit" * doctest::test_suite("[high-risk]")) {
    CoreFixture fx;

    CachedTicket t;
    t.id = "ABC-5";
    fx.host.TicketsById["ABC-5"] = t;

    TrackerField prio;
    prio.Id = "priority";
    prio.Name = "Priority";
    fx.host.FieldsById["priority"] = prio;

    fx.host.SubmitFieldEditReturn = true;

    sol::protected_function_result r = Run(fx.state(), R"(
        local tk = smatchet.get_ticket("ABC-5")
        local ok, err = tk:set_field("priority", "High")
        return ok, tostring(err)
    )");
    CHECK_EQ(r.get<bool>(0), true);
    CHECK_EQ(r.get<std::string>(1), std::string(""));

    REQUIRE_EQ(fx.host.SubmitFieldEditCalls.size(), 1u);
    CHECK_EQ(fx.host.SubmitFieldEditCalls[0].IssueId, std::string("ABC-5"));
    CHECK_EQ(fx.host.SubmitFieldEditCalls[0].FieldId, std::string("priority"));
    REQUIRE_EQ(fx.host.SubmitFieldEditCalls[0].Values.size(), 1u);
    CHECK_EQ(fx.host.SubmitFieldEditCalls[0].Values[0], std::string("High"));
}

TEST_CASE("Lua bindings · Ticket:set_field surfaces failure path") {
    CoreFixture fx;

    CachedTicket t;
    t.id = "ABC-7";
    fx.host.TicketsById["ABC-7"] = t;
    TrackerField prio;
    prio.Id = "priority";
    prio.Name = "Priority";
    fx.host.FieldsById["priority"] = prio;

    fx.host.SubmitFieldEditReturn = false;
    fx.host.SubmitFieldEditError = "tracker rejected value";

    sol::protected_function_result r = Run(fx.state(), R"(
        local tk = smatchet.get_ticket("ABC-7")
        local ok, err = tk:set_field("priority", "Bogus")
        return ok, err
    )");
    CHECK_EQ(r.get<bool>(0), false);
    CHECK_EQ(r.get<std::string>(1), std::string("tracker rejected value"));
}

TEST_CASE("Lua bindings · Ticket:set_field unknown field returns error") {
    CoreFixture fx;

    CachedTicket t;
    t.id = "ABC-9";
    fx.host.TicketsById["ABC-9"] = t;
    // No fields registered -- FindFieldById returns nullptr.

    sol::protected_function_result r = Run(fx.state(), R"(
        local tk = smatchet.get_ticket("ABC-9")
        local ok, err = tk:set_field("does_not_exist", "v")
        return ok, err
    )");
    CHECK_EQ(r.get<bool>(0), false);
    CHECK(r.get<std::string>(1).find("does_not_exist") != std::string::npos);
    // SubmitFieldEdit was NOT called -- glue bails on the FindFieldById null.
    CHECK_EQ(fx.host.SubmitFieldEditCalls.size(), 0u);
}

TEST_CASE("Lua bindings · Ticket:transition uses status field") {
    CoreFixture fx;

    CachedTicket t;
    t.id = "ABC-11";
    fx.host.TicketsById["ABC-11"] = t;
    TrackerField status;
    status.Id = "status";
    status.Name = "Status";
    fx.host.FieldsById["status"] = status;

    Run(fx.state(), R"(
        local tk = smatchet.get_ticket("ABC-11")
        tk:transition("Done")
    )");
    REQUIRE_EQ(fx.host.SubmitFieldEditCalls.size(), 1u);
    CHECK_EQ(fx.host.SubmitFieldEditCalls[0].FieldId, std::string("status"));
    REQUIRE_EQ(fx.host.SubmitFieldEditCalls[0].Values.size(), 1u);
    CHECK_EQ(fx.host.SubmitFieldEditCalls[0].Values[0], std::string("Done"));
}

// =============================================================================
// mcp.register_tool
// =============================================================================

TEST_CASE("Lua bindings · mcp.register_tool forwards toolDef + callback") {
    CoreFixture fx;

    Run(fx.state(), R"(
        mcp.register_tool({name="echo", description="echo back"}, function(args)
            return args
        end)
        mcp.register_tool({name="ping"}, function() return "pong" end)
    )");
    REQUIRE_EQ(fx.host.McpRegistrations.size(), 2u);
    const std::string n0 = fx.host.McpRegistrations[0].ToolDef["name"];
    const std::string n1 = fx.host.McpRegistrations[1].ToolDef["name"];
    CHECK_EQ(n0, std::string("echo"));
    CHECK_EQ(n1, std::string("ping"));
}

// =============================================================================
// commands.invoke
// =============================================================================

TEST_CASE("Lua bindings · commands.invoke unknown command returns ok=false") {
    CoreFixture fx;
    // Empty registry.

    sol::protected_function_result r = Run(fx.state(), R"(
        local res = commands.invoke("does.not.exist")
        return res.ok, res.error.code
    )");
    CHECK_EQ(r.get<bool>(0), false);
    CHECK_EQ(r.get<std::string>(1), std::string("unknown-command"));
}

TEST_CASE("Lua bindings · commands.invoke routes through host registry") {
    CoreFixture fx;

    smatchet::cmd::Command cmd;
    cmd.Name = "test.echo";
    cmd.Category = "test";
    cmd.Summary = "echo a value back";
    cmd.Handler = [](const nlohmann::json& args, const smatchet::cmd::CommandContext& /*ctx*/) {
        nlohmann::json data;
        data["v"] = args.value("v", std::string("(none)"));
        return smatchet::cmd::CommandResult::Success(std::move(data));
    };
    fx.host.Commands().Register(std::move(cmd));

    sol::protected_function_result r = Run(fx.state(), R"(
        local res = commands.invoke("test.echo", {v="hello"})
        return res.ok, res.data.v
    )");
    CHECK_EQ(r.get<bool>(0), true);
    CHECK_EQ(r.get<std::string>(1), std::string("hello"));
}

// =============================================================================
// Tracker / commands / mcp table structural smokes
// =============================================================================

TEST_CASE("Lua bindings · top-level tables are present and well-formed") {
    CoreFixture fx;

    sol::protected_function_result r = Run(fx.state(), R"(
        local function isfn(v) return type(v) == "function" end
        local function istbl(v) return type(v) == "table" end
        return
            istbl(smatchet), isfn(smatchet.get_ticket), isfn(smatchet.get_active_tickets),
            istbl(tracker),  isfn(tracker.get_type),    isfn(tracker.create_issue),
            istbl(mcp),      isfn(mcp.register_tool),
            istbl(commands), isfn(commands.invoke),
            isfn(log_info),  isfn(decode_json)
    )");
    CHECK(r.get<bool>(0));  // smatchet table
    CHECK(r.get<bool>(1));  // smatchet.get_ticket
    CHECK(r.get<bool>(2));  // smatchet.get_active_tickets
    CHECK(r.get<bool>(3));  // tracker table
    CHECK(r.get<bool>(4));  // tracker.get_type
    CHECK(r.get<bool>(5));  // tracker.create_issue
    CHECK(r.get<bool>(6));  // mcp table
    CHECK(r.get<bool>(7));  // mcp.register_tool
    CHECK(r.get<bool>(8));  // commands table
    CHECK(r.get<bool>(9));  // commands.invoke
    CHECK(r.get<bool>(10)); // log_info global
    CHECK(r.get<bool>(11)); // decode_json global
}

TEST_CASE("Lua bindings · tracker.get_type returns config tracker type") {
    CoreFixture fx;
    // ConfigManager::Load() default is SmatchetDefaults::kDefaultBackendType
    // ("Jira"). The test does not write a smatchet_config.json so the default
    // path triggers. The assertion is contract-shape, not value-precise:
    // tracker.get_type() must return a non-empty trimmed string.

    sol::protected_function_result r = Run(fx.state(), R"(
        local t = tracker.get_type()
        return type(t), t
    )");
    CHECK_EQ(r.get<std::string>(0), std::string("string"));
    const std::string v = r.get<std::string>(1);
    CHECK_FALSE(v.empty());
}

// =============================================================================
// Sandbox-closure regression smoke (after InitLuaCore runs against the fake)
// =============================================================================

// Self-improvement notes flag the [high-risk] closure invariant from
// `LuaSandbox.test.cpp` -- assert here that running `InitLuaCore` against a
// FakeLuaBindingHost does NOT regress the closure (i.e. doesn't accidentally
// open `sol::lib::os` or expose `os.execute`). The dedicated [high-risk] case
// for that invariant lives in LuaSandbox.test.cpp; this is a cheap smoke that
// the test-side wiring didn't undermine it.
TEST_CASE("Lua bindings · InitLuaCore does not regress sandbox closure") {
    CoreFixture fx;

    sol::protected_function_result r = Run(fx.state(), R"(
        return os.execute == nil
           and os.remove  == nil
           and io         == nil
           and require    == nil
    )");
    CHECK_EQ(r.get<bool>(), true);
}
