# SmatchetLuaTests — separate doctest binary that exercises the production
# Lua/sol2 surface. Lives in its own target (not SmatchetTests) because:
#   1. Pulling Smatchet_Lua_Internal + sol2 into SmatchetTests would link Lua
#      C source + sol2's heavy templates into every pure-logic test run.
#   2. The Lua bindings TU `AppController_LuaBindings.cpp` is Class C per
#      docs/design/_plan-locks.md § Phase 6 (drags ImGui via top-level
#      `#include "imgui.h"`), so LuaBindings.test.cpp is deferred -- this
#      target runs the sandbox + timeout + drift-sentinel facets that are
#      reachable without the binding TU.
#
# Gated by `SMATCHET_BUILD_LUA_TESTS=ON` from the root CMakeLists.txt; the
# ninja-test-msys2 preset auto-enables it so `cmake --build && ctest`
# exercises both SmatchetTests + SmatchetLuaTests in one shot.
#
# Requires SMATCHET_WITH_LUA_AUTOMATION (the production option that builds
# Smatchet_Lua_Internal + downloads sol2). The root CMakeLists.txt enforces
# this prerequisite.

add_executable(SmatchetLuaTests
    lua_main.cpp
    LuaSandbox.test.cpp
    LuaTimeout.test.cpp
    LuaStubsCompile.test.cpp
)

target_include_directories(SmatchetLuaTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Source_Core/include"
    "${CMAKE_SOURCE_DIR}/tests/support"
    "${LUA_SRC_DIR}/src"
)

target_include_directories(SmatchetLuaTests SYSTEM PRIVATE
    "${sol2_SOURCE_DIR}/single"
)

# Link against the production Lua static lib (Smatchet_Lua_Internal -- the
# 5.3 source built as C++ per CMakeLists.txt:336-343) so sol2's pcall +
# luaL_error path works the same way it does in the standalone exe.
# doctest is the only test-only dep. NO ImGui, NO cpr, NO SQLite -- those
# would defeat the Lua-only isolation.
target_link_libraries(SmatchetLuaTests PRIVATE
    doctest::doctest
    Smatchet_Lua_Internal
)

target_compile_features(SmatchetLuaTests PRIVATE cxx_std_14)

# sol2 expects SOL_ALL_SAFETIES_ON when the host enables it (mirrors
# AppController.h:10). Keeps the safety-rail set identical between production
# and the test binary -- e.g. nil-checks on usertype accesses.
target_compile_definitions(SmatchetLuaTests PRIVATE
    SOL_ALL_SAFETIES_ON=1
)

# Static-link the C++ + GCC runtime so ctest can run SmatchetLuaTests.exe from
# any directory (PATH-independent). Mirrors SmatchetTests' MinGW link options.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(SmatchetLuaTests PRIVATE
        -static-libgcc
        -static-libstdc++
        -static
    )
endif()

add_test(NAME smatchet_lua_tests COMMAND SmatchetLuaTests)
