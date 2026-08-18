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
# ninja-test-msvc preset auto-enables it so `cmake --build && ctest`
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
    LuaBindings.test.cpp
    # PR #1271 — pure doctest for the shared depth/node-bounded parse helper
    # (smatchet::json_safe::ParseBounded, Source/Core/include/Json/BoundedJsonParse.h)
    # that every JSON ingress (decode_json, MCP REST/JSON-RPC, Lua-MCP params) routes
    # through. Header-only + nlohmann-only; no extra production .cpp to link.
    BoundedJsonParse.test.cpp
    # debt luajson-convert-leaf — direct doctest for the JSON<->Lua marshalling
    # leaf (Source/Core/include/Json/LuaJsonConvert.h) lifted out of the two
    # binding TUs. Header-only inline (sol2 + nlohmann already on this target);
    # no extra production .cpp to link.
    LuaJsonConvert.test.cpp
    # Phase 6b: link the lifted, ImGui-free production TU directly so the
    # round-trip test exercises the real `smatchet::lua::InitLuaCore` body
    # (per the Phase-5 mcp-pure recipe -- production .cpp on the test target's
    # source list, not a static lib). Transitive deps (ConfigManager,
    # FieldEditAuditSource, Logger, Commands::CommandRegistry/Command/FuzzyMatch)
    # are added below.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/AppController_LuaBindingsCore.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager.cpp"
    # ConfigManager god-file split (god-file-splits plan): companion Save/Load/Secret TUs.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_Save.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_Load.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_Secrets.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_PathUtils.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ConfigManager_Views.cpp"
    # C5 — ScopedFileLock now lives in the layer-0 FileIo module; ConfigManager_PathUtils.cpp
    # and ConfigManager_Views.cpp resolve it from there, so this rig needs the TU.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/FileIo.cpp"
    # close-gate-gaps Slice 1a: ConfigManager_PathUtils/Views now call into UiThreadAffinity.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/UiThreadAffinity.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/ToolbarConfig.cpp"
    # rebindable-shortcuts PR1: ConfigManager.cpp now constructs/serializes a
    # KeybindingsConfig (TrackerConfig member), so the Lua-test link needs this
    # TU too -- same transitive-dep rule as ToolbarConfig above (#1207 CI red).
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config/KeybindingsConfig.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence/FieldEditAuditSource.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Logger.cpp"
    # Logger.cpp now routes the file sink through RedactLogLine (audit
    # 2026-06-13 LOW) — link the pure redactor so the closure resolves.
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Privacy/TextRedaction.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/Command.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/CommandRegistry.cpp"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Commands/FuzzyMatch.cpp"
)

# AppController_LuaBindingsCore.cpp uses -mcmodel=large in the BFD-ld production
# build (CMakeLists.txt:976-989) to reach symbols past 2 GB. Under lld -- which
# the ninja-test-msvc preset selects via SMATCHET_LLD_PROGRAM -- the flag
# produces "unsupported relocation type 0xe" link errors and is explicitly
# skipped (CMakeLists.txt:976 gate). Mirror that gate here so the test binary
# links cleanly regardless of which linker is active. -Wa,-mbig-obj is still
# applied to keep COFF debug-section relocations under the 64K limit when
# sol2's templates expand inside this TU.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(NOT SMATCHET_LLD_PROGRAM)
        set_source_files_properties(
            "${CMAKE_SOURCE_DIR}/Source/Core/src/AppController_LuaBindingsCore.cpp"
            PROPERTIES
                COMPILE_OPTIONS "-mcmodel=large"
                SKIP_PRECOMPILE_HEADERS ON)
        target_link_options(SmatchetLuaTests PRIVATE -mcmodel=large)
    endif()
    if(SMATCHET_HAS_MBIG_OBJ)
        target_compile_options(SmatchetLuaTests PRIVATE -Wa,-mbig-obj)
    endif()
endif()

target_include_directories(SmatchetLuaTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Source/Core/include"
    "${CMAKE_SOURCE_DIR}/Source/Core/src"   # for ConfigManager_Internal.h
    # Subsystem subdirs from source-core-dir-reorg. All 5 are needed (not just
    # Config/Persistence): moved headers cross-include each other bare — e.g.
    # ILuaBindingHost.h -> TrackerFieldSchema.h (Tracker) — so the full subdir
    # set must be on the path.
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Tracker"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Sync"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Persistence"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Config"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Ui"
    "${CMAKE_SOURCE_DIR}/Source/Core/include/Privacy"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Tracker"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Sync"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Persistence"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Config"
    "${CMAKE_SOURCE_DIR}/Source/Core/src/Ui"
    "${CMAKE_SOURCE_DIR}/tests/support"
    "${LUA_SRC_DIR}/src"
)

target_include_directories(SmatchetLuaTests SYSTEM PRIVATE
    "${sol2_SOURCE_DIR}/single"
)

# Link against the production Lua static lib (Smatchet_Lua_Internal -- the
# 5.3 source built as C++ per CMakeLists.txt:336-343) so sol2's pcall +
# luaL_error path works the same way it does in the standalone exe.
# Phase 6b adds nlohmann_json (consumed by AppController_LuaBindingsCore.cpp
# + ConfigManager.cpp + CommandRegistry.cpp) and Windows crypt32 (consumed by
# ConfigManager_PathUtils.cpp's DPAPI calls). NO ImGui, NO cpr, NO SQLite --
# those would defeat the Lua-only isolation.
target_link_libraries(SmatchetLuaTests PRIVATE
    doctest::doctest
    Smatchet_Lua_Internal
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(SmatchetLuaTests PRIVATE crypt32)
endif()

target_compile_features(SmatchetLuaTests PRIVATE cxx_std_14)

# B8 (/EHsc on SmatchetLuaTests) — same flag-clobbering guard as SmatchetTests
# (tests/CMakeLists.txt). The Lua rig links sol2 (heavy templates + pcall/luaL_error
# unwinding) and doctest CHECK_THROWS*, so a missing /EHsc surfaces the same
# C2338/C4530 pair when CMAKE_CXX_FLAGS loses the MSVC default (stale-cache or cpr
# -Werror pollution). PRIVATE + MSVC-guarded so non-MSVC test presets are unaffected.
target_compile_options(SmatchetLuaTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)

# sol2 expects SOL_ALL_SAFETIES_ON when the host enables it (mirrors
# AppController.h:10). Keeps the safety-rail set identical between production
# and the test binary -- e.g. nil-checks on usertype accesses.
target_compile_definitions(SmatchetLuaTests PRIVATE
    SOL_ALL_SAFETIES_ON=1
    SMATCHET_WITH_LUA_AUTOMATION=1
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
