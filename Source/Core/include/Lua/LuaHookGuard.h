#ifndef SMATCHET_LUA_HOOK_GUARD_H
#define SMATCHET_LUA_HOOK_GUARD_H

// RAII Lua instruction-count timeout hook.
//
// Hoisted out of the src-private AppController_LuaBindings_detail.h so the
// timeout regression guard (tests/Lua/LuaTimeout.test.cpp, via
// tests/support/LuaHostFixture.h) installs THIS hook rather than a copy of it
// (DR29). The detail header still re-exports the name for the two binding TUs.
//
// The contract under test: lua_sethook(LUA_MASKCOUNT, count) + a hook body that
// calls luaL_error, which — in the Lua-as-C++ build (CMakeLists.txt, LUAI_THROW
// = throw) — unwinds into sol2's pcall and surfaces as an invalid
// protected_function_result rather than a panic/abort. The uniform budget is
// 100000 instructions per docs/plans/active/applied/lua-recorded-cmd-list.md
// (§LuaHookGuard, Q7); the count is a parameter so tests can make the abort fire
// in milliseconds without changing the mechanism.

// GCC 13+ / sol2 v2.20.6: sol2 reaches for std::numeric_limits<std::size_t>::max
// without including <limits>. Mirror AppController.h's ordering (limits + cstdint
// BEFORE sol/sol.hpp) so this header is safe to include first.
#include <limits>
#include <cstdint>

#include <sol/sol.hpp>

namespace smatchet {
namespace lua {

// Default instruction budget for one script/callback invocation.
constexpr int kLuaHookInstructionBudget = 100000;

struct LuaHookGuard {
    lua_State* L;

    explicit LuaHookGuard(sol::state& lua, int count = kLuaHookInstructionBudget) : L(lua.lua_state()) {
        lua_sethook(
            L, [](lua_State* s, lua_Debug*) { luaL_error(s, "Script execution timeout exceeded."); }, LUA_MASKCOUNT,
            count);
    }

    ~LuaHookGuard() { lua_sethook(L, nullptr, 0, 0); }

    LuaHookGuard(const LuaHookGuard&) = delete;
    LuaHookGuard& operator=(const LuaHookGuard&) = delete;
};

} // namespace lua
} // namespace smatchet

#endif // SMATCHET_LUA_HOOK_GUARD_H
