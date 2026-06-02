// LuaSandbox.test.cpp — assert the sandbox closure invariant: a script running
// in the same library subset that production's `AppController::InitLuaCore`
// opens (see AppController_LuaBindings.cpp:725-755) cannot reach the
// host-process surface (filesystem, subprocess, env, dynamic code load).
//
// This is a CONTRACT test, not an INTEGRATION test — `InitLuaCore` is Class C
// per _plan-locks § Phase 6 (binding TU includes "imgui.h" + the
// `__smatchet_app = this` capture). LuaHostFixture mirrors the same lib
// opens + os whitelist so the sandbox invariant is asserted at the sol2 layer
// rather than by linking the binding TU.

#include "LuaHostFixture.h"

#include <doctest/doctest.h>

using smatchet_test::LuaHostFixture;

namespace {

// Returns true if the Lua expression evaluates to `nil`. Used for the "global
// is not exposed" assertions — production's sandbox closure is "we never opened
// the library", which manifests in Lua as the global table entry being absent
// (i.e. nil), NOT as a thrown error.
bool ExprIsNil(sol::state& L, const std::string& expr) {
    sol::protected_function_result r = L.safe_script(std::string("return ") + expr,
                                                     sol::script_pass_on_error);
    if (!r.valid()) {
        return false;
    }
    sol::object obj = r;
    return !obj.valid() || obj.get_type() == sol::type::lua_nil;
}

} // namespace

TEST_CASE("Lua sandbox · subprocess-execution globals are absent") {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // os.execute would let a script shell out (`os.execute("rm -rf /")` family).
    // Production whitelists os to {time, clock, difftime, date}; execute is not
    // in that list and so resolves to nil.
    CHECK(ExprIsNil(L, "os.execute"));
    CHECK(ExprIsNil(L, "os.remove"));
    CHECK(ExprIsNil(L, "os.rename"));
    CHECK(ExprIsNil(L, "os.exit"));
    CHECK(ExprIsNil(L, "os.getenv"));
    CHECK(ExprIsNil(L, "os.setlocale"));
    CHECK(ExprIsNil(L, "os.tmpname"));
}

TEST_CASE("Lua sandbox · filesystem-access globals are absent") {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // The `io` library (open / read / write / lines / etc.) is never opened.
    CHECK(ExprIsNil(L, "io"));
    CHECK(ExprIsNil(L, "io and io.open"));
    CHECK(ExprIsNil(L, "io and io.read"));
    CHECK(ExprIsNil(L, "io and io.lines"));
    CHECK(ExprIsNil(L, "io and io.popen"));
}

TEST_CASE("Lua sandbox · dynamic code-load globals are absent") {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // require / package come from sol::lib::package which production does NOT open.
    // dofile / loadfile come from sol::lib::base BUT production's base lib open
    // documents that scripts never use these — they read files at C++-level only.
    // sol::lib::base unfortunately exposes them; assert their presence to
    // document the gap, then expect them to *fail* with the empty package.path.
    CHECK(ExprIsNil(L, "require"));
    CHECK(ExprIsNil(L, "package"));
}

// Helper: extract a string return from a sol::protected_function_result. sol2's
// implicit conversion ambiguates against std::string's multiple ctors under GCC
// 16, so we go through .get<std::string>() explicitly.
namespace {
std::string ResultAsString(sol::protected_function_result& r) {
    return r.get<std::string>();
}
} // namespace

// * doctest::test_suite("[high-risk]") — sandbox closure invariant. If production's
// `state.open_libraries(sol::lib::os)` ever creeps back in (a refactor mistake),
// this test fails on the very first assertion. Mutation-sanity (deferred prod
// mutation, taxonomy path 2): replacing the production InitLuaCore `osSafe` table
// with `state.open_libraries(sol::lib::os)` would expose `os.execute` -- the
// `CHECK(ExprIsNil(L, "os.execute"))` above forces the test to fail.
TEST_CASE("Lua sandbox · allowed globals are present" * doctest::test_suite("[high-risk]")) {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // The libs production DOES open: base, string, table, math.
    sol::protected_function_result r1 = L.safe_script("return type(tostring)");
    REQUIRE(r1.valid());
    CHECK_EQ(ResultAsString(r1), std::string("function"));

    sol::protected_function_result r2 = L.safe_script("return type(string.format)");
    REQUIRE(r2.valid());
    CHECK_EQ(ResultAsString(r2), std::string("function"));

    sol::protected_function_result r3 = L.safe_script("return type(table.concat)");
    REQUIRE(r3.valid());
    CHECK_EQ(ResultAsString(r3), std::string("function"));

    sol::protected_function_result r4 = L.safe_script("return type(math.floor)");
    REQUIRE(r4.valid());
    CHECK_EQ(ResultAsString(r4), std::string("function"));

    // pairs / ipairs / type / tostring / tonumber are part of base.
    sol::protected_function_result r5 = L.safe_script("return type(pairs)");
    REQUIRE(r5.valid());
    CHECK_EQ(ResultAsString(r5), std::string("function"));

    sol::protected_function_result r6 = L.safe_script("return type(ipairs)");
    REQUIRE(r6.valid());
    CHECK_EQ(ResultAsString(r6), std::string("function"));
}

TEST_CASE("Lua sandbox · whitelisted os.* members work") {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // The four whitelisted os fns must exist and be callable.
    sol::protected_function_result r1 = L.safe_script("return type(os.time)");
    REQUIRE(r1.valid());
    CHECK_EQ(ResultAsString(r1), std::string("function"));

    sol::protected_function_result r2 = L.safe_script("return type(os.clock)");
    REQUIRE(r2.valid());
    CHECK_EQ(ResultAsString(r2), std::string("function"));

    sol::protected_function_result r3 = L.safe_script("return type(os.difftime)");
    REQUIRE(r3.valid());
    CHECK_EQ(ResultAsString(r3), std::string("function"));

    sol::protected_function_result r4 = L.safe_script("local t = os.time(); return type(t)");
    REQUIRE(r4.valid());
    CHECK_EQ(ResultAsString(r4), std::string("number"));
}

TEST_CASE("Lua sandbox · script syntax error returns recoverable result, not panic") {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // The panic handler installed in LuaHostFixture throws std::runtime_error.
    // A malformed script should NEVER reach panic — sol's safe_script wraps the
    // parse + run in pcall and returns an invalid protected_function_result.
    sol::protected_function_result r = L.safe_script("invalid lua syntax {{{",
                                                     sol::script_pass_on_error);
    CHECK_FALSE(r.valid());
}

TEST_CASE("Lua sandbox · runtime error in script returns recoverable result") {
    LuaHostFixture fx;
    sol::state& L = fx.state();

    // Indexing nil is a runtime error. Must be catchable by the host.
    sol::protected_function_result r = L.safe_script("local x = nil; return x.y",
                                                     sol::script_pass_on_error);
    CHECK_FALSE(r.valid());
}
