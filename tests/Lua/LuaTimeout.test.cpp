// LuaTimeout.test.cpp — assert the timeout-abort-not-panic invariant: an
// infinite-loop script must be terminated by the count-hook before it can
// hang the host, AND the termination must surface as a recoverable
// protected_function_result error rather than as a Lua panic (which would
// abort() the process per sol2 defaults).
//
// The hook is the PRODUCTION one: LuaHostFixture::InstallInstructionCountTimeout
// constructs `smatchet::lua::LuaHookGuard` (Source/Core/include/Lua/LuaHookGuard.h),
// the same RAII guard AppController_LuaBindings_Draw.cpp wraps every script and
// callback invocation in. Production's default budget is 100000 instructions per
// docs/plans/active/applied/lua-recorded-cmd-list.md §LuaHookGuard Q7; these cases
// pass a much smaller count (a parameter of that guard) so the abort fires in
// milliseconds. The mechanism is therefore not mirrored but executed:
// `lua_sethook(LUA_MASKCOUNT)` + a hook that calls `luaL_error` -> Lua throws
// (Lua-as-C++ build per CMakeLists.txt:336-343) -> sol2 catches the throw inside
// pcall -> returns invalid result. The literal instruction count is not the contract.

#include "LuaHostFixture.h"

#include <doctest/doctest.h>

using smatchet_test::LuaHostFixture;

TEST_CASE("Lua timeout · infinite loop is aborted by count hook") {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // 1000 instructions: long enough for short scripts to complete, short enough
    // that an unbounded loop hits the hook in a single doctest tick.
    fx.InstallInstructionCountTimeout(1000);

    sol::protected_function_result r = L.safe_script("while true do end",
                                                     sol::script_pass_on_error);
    fx.ClearInstructionCountTimeout();

    // The hook fires luaL_error which becomes an invalid protected_function_result.
    // If the hook were misconfigured (e.g. count = 0, or no hook installed), this
    // would hang the test runner — doctest would time out at the harness level,
    // not at this assertion. Reaching this CHECK at all is half the proof.
    CHECK_FALSE(r.valid());
}

TEST_CASE("Lua timeout · script under budget completes normally") {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // Generous budget: a 10-iteration accumulator burns ~100 instructions max.
    fx.InstallInstructionCountTimeout(100000);

    sol::protected_function_result r = L.safe_script(
        "local s = 0; for i = 1, 10 do s = s + i end; return s",
        sol::script_pass_on_error);
    fx.ClearInstructionCountTimeout();

    REQUIRE(r.valid());
    const int sum = r.get<int>();
    CHECK_EQ(sum, 55);
}

// * doctest::test_suite("[high-risk]") — timeout-abort-not-panic invariant.
// Mutation-sanity: production's `LuaHookGuard` is the object under test here, so
// mutating it lands directly on this case. If its hook body were rewritten to call
// `lua_error` without the Lua-as-C++ build (LUAI_THROW = longjmp instead of
// `throw`), the panic handler would fire, our handler `throw std::runtime_error`s,
// and sol2's pcall catches throws from C++ but NOT longjmp — the assertions below
// (CHECK_NOTHROW + CHECK_FALSE) would fail: either the doctest binary aborts
// (longjmp through pcall is UB), or the throw escapes and CHECK_NOTHROW fires.
// Both surface as a fail rather than a silent hang.
TEST_CASE("Lua timeout · hook abort does NOT escape as exception"
          * doctest::test_suite("[high-risk]")) {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    fx.InstallInstructionCountTimeout(500);

    // The hook's luaL_error must be caught inside pcall — not by the panic
    // handler. Wrap in CHECK_NOTHROW so the test fails loudly if the panic
    // handler ever fires (which would mean Lua-as-C++ regressed to longjmp,
    // or sol2's safe_script pipeline broke).
    sol::protected_function_result r;
    CHECK_NOTHROW(
        r = L.safe_script("local n = 0; while true do n = n + 1 end",
                          sol::script_pass_on_error));

    fx.ClearInstructionCountTimeout();

    // The script was aborted, so the result is invalid.
    CHECK_FALSE(r.valid());
}

TEST_CASE("Lua timeout · hook can be reinstalled across multiple scripts") {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // First script: aborts.
    fx.InstallInstructionCountTimeout(500);
    sol::protected_function_result r1 = L.safe_script("while true do end",
                                                      sol::script_pass_on_error);
    fx.ClearInstructionCountTimeout();
    CHECK_FALSE(r1.valid());

    // Second script: same state, generous budget, completes.
    fx.InstallInstructionCountTimeout(100000);
    sol::protected_function_result r2 = L.safe_script("return 42",
                                                      sol::script_pass_on_error);
    fx.ClearInstructionCountTimeout();
    REQUIRE(r2.valid());
    const int val = r2.get<int>();
    CHECK_EQ(val, 42);
}
