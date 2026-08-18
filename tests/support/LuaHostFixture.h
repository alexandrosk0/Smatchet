// LuaHostFixture — test-side scaffolding that builds a sol::state mirroring the
// safe-libs subset that production's AppController::InitLuaCore opens
// (`base`, `string`, `table`, `math`; whitelisted `os.*`; no `io`, no `os.execute`,
// no `package`, no `require`, no `dofile` / `loadfile`).
//
// This fixture does NOT call `AppController::InitLuaCore` directly: that fn lives
// in `AppController_LuaBindings.cpp` whose top-level `#include "imgui.h"` (line 32)
// + the `state["__smatchet_app"] = this` capture (line 766) drag the full
// AppController + ImGui surface — Class C per the Phase 6 InitLuaCore
// classification in _plan-locks. Tests instead replicate the
// sandbox closure invariant (which globals are denied) and the timeout invariant
// (debug-hook count -> luaL_error -> recoverable C++ exception) at the
// sol::state level, so production's sandbox + timeout policy stays the single
// source of truth and tests assert the contract rather than the implementation.
//
// sol2 v2.20.6 + Lua 5.3-as-C++ invariants (agents/lua-binder.md):
//   - Include lua via <lua.hpp> (the C++-friendly umbrella) so the SMATCHET
//     extern "C" patch in luaconf.h applies.
//   - Install a panic handler that throws — sol2 defaults to abort() on panic,
//     which would kill the doctest runner outright.
//   - One sol::state per test. The fixture's dtor is trivial: sol::state owns
//     the lua_State* and closes it on destruction.

#pragma once

// GCC 16 + sol2 v2.20.6 — sol2's `align_usertype_unique` / `align_user` /
// `align_usertype` (single-header lines ~7270-7340) reference
// `std::numeric_limits<std::size_t>::max` without including <limits>. Production
// AppController.h:5 includes <limits> + <cstdint> BEFORE sol/sol.hpp at :11 for
// this reason (GCC 13+ compatibility comment in that header). Mirror the
// ordering here so SmatchetLuaTests compiles on UCRT64 GCC 16.
#include <limits>
#include <cstdint>

#include <sol/sol.hpp>

#include "Lua/LuaHookGuard.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace smatchet_test {

class LuaHostFixture {
  public:
    LuaHostFixture() {
        // Panic-as-exception: without this, a Lua panic (unhandled error outside a
        // pcall) calls abort() — doctest's process dies and the runner reports a
        // crash rather than a failing assertion. The lambda must NOT be a stateful
        // closure (sol2 stores it as a free C function pointer-equivalent).
        state_.set_panic([](lua_State* L) -> int {
            const char* msg = lua_tostring(L, -1);
            throw std::runtime_error(msg ? msg : "lua panic (no message)");
        });

        // Mirror the production sandbox in InitLuaCore (AppController_LuaBindings.cpp:725-755).
        // Open ONLY the safe libs. Notably absent: `io`, `os` (whitelisted subset only),
        // `package`, `debug`. `require`, `dofile`, `loadfile` come from `base` but production
        // expects scripts not to use them; we leave the assertion to LuaSandbox.test.cpp.
        state_.open_libraries(sol::lib::base);
        state_.open_libraries(sol::lib::string);
        state_.open_libraries(sol::lib::table);
        state_.open_libraries(sol::lib::math);

        // Production replaces `os` with a whitelisted table (time/clock/difftime/date only)
        // and DOES NOT open sol::lib::os. Mirror that here so the sandbox test's "os.execute
        // is nil" assertion holds.
        sol::table osSafe = state_.create_table();
        osSafe.set_function("time",     []() -> std::time_t { return std::time(nullptr); });
        osSafe.set_function("clock",    []() -> double { return static_cast<double>(std::clock()) / CLOCKS_PER_SEC; });
        osSafe.set_function("difftime", [](std::time_t a, std::time_t b) -> double { return std::difftime(a, b); });
        state_["os"] = osSafe;
    }

    sol::state& state() { return state_; }
    const sol::state& state() const { return state_; }

    // Install the PRODUCTION timeout guard (smatchet::lua::LuaHookGuard,
    // Source/Core/include/Lua/LuaHookGuard.h) — the same RAII object the binding
    // TUs wrap every script/callback invocation in. Held by unique_ptr only because
    // doctest cases want install/clear as statements rather than a lexical scope;
    // the hook body, mask and teardown are production's, not a copy. Tests pass a
    // much smaller count than the production default so the abort fires in
    // milliseconds — the count is a parameter of the production guard, and the
    // mechanism under test (count-hook -> luaL_error -> invalid
    // protected_function_result) is unchanged by it.
    void InstallInstructionCountTimeout(int count) {
        hook_.reset();
        hook_.reset(new smatchet::lua::LuaHookGuard(state_, count));
    }

    // Runs ~LuaHookGuard (production's lua_sethook(nullptr, 0, 0) teardown).
    void ClearInstructionCountTimeout() { hook_.reset(); }

  private:
    sol::state state_;
    // Declared after state_ so the guard is torn down before the lua_State closes.
    std::unique_ptr<smatchet::lua::LuaHookGuard> hook_;
};

} // namespace smatchet_test
