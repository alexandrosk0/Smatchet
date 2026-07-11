# Source/Plugins/LuaConsole — Lua console static plugin (SmatchetPlugin_LuaConsole).
#
# INCLUDED from the root CMakeLists.txt via include() — deliberately NOT
# add_subdirectory(). All targets, variables, and source-file properties defined
# here live in the ROOT directory scope, exactly as they did when this content
# was part of the monolithic root CMakeLists.txt (pure mechanical split — zero
# behavior change: artifact/object paths, target names, options, and flags are
# identical). Consequences to keep in mind when editing:
#   * relative paths resolve against the repo root (CMAKE_CURRENT_SOURCE_DIR ==
#     the repo root here), NOT against this file's directory;
#   * variables set here are plain root-scope variables — visible to every
#     component file included after this one (no PARENT_SCOPE needed);
#   * do NOT add cmake_minimum_required() / project() here.
#
# Include order (root CMakeLists.txt § component includes):
#   ThirdParty/App deps -> Source/Core -> Source/Mobile -> Source/Plugins/Mcp
#   -> Source/Plugins/LuaConsole -> Source/Plugins/Whisper (add_subdirectory)
#   -> Source/UnrealPlugins -> Source/Standalone
#
# Defines (SMATCHET_WITH_LUA_AUTOMATION only): SmatchetPlugin_LuaConsole. The
# Unreal/DX12 sibling (SmatchetPlugin_LuaConsole_DX12) lives in
# Source/UnrealPlugins/CMakeLists.txt.

if(SMATCHET_WITH_LUA_AUTOMATION)
    # TextEditor source lives under Source/Core/ThirdParty/ImGuiColorTextEdit (promoted from
    # Source/Plugins/LuaConsole/ThirdParty so Source/Core can consume it for the AI chat panel).
    # SmatchetCore now compiles + links the TU; LuaConsole inherits it via SmatchetCoreInterface.
    # LuaConsolePlugin.cpp instantiates sol2 templates and trips the same COFF
    # relocation overflow as AppController_LuaBindings.cpp under BFD ld. Skip
    # the flag when lld is the active linker — lld uses 64-bit relocations by
    # default and rejects -mcmodel=large objects (unsupported reloc type 0xe).
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT SMATCHET_LLD_PROGRAM)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Plugins/LuaConsole/LuaConsolePlugin.cpp"
            PROPERTIES COMPILE_OPTIONS "-mcmodel=large")
    endif()
    add_library(SmatchetPlugin_LuaConsole STATIC Source/Plugins/LuaConsole/LuaConsolePlugin.cpp)
    target_include_directories(SmatchetPlugin_LuaConsole PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Plugins/LuaConsole")
    target_link_libraries(SmatchetPlugin_LuaConsole PUBLIC SmatchetCoreInterface ImGuiLib)
    if(SMATCHET_WITH_MCP)
        target_link_libraries(SmatchetPlugin_LuaConsole PUBLIC SmatchetCoreMcpShim)
    endif()
    # SmatchetUiSession.h has a ~200-byte `#if defined(SMATCHET_WITH_AI)` block of `assistant*`
    # members in the middle of `UiDrawSession`. Source/Core compiles with `SMATCHET_WITH_AI=1`
    # (via `SmatchetCoreAiShim` on the OpenGL/DX targets) — without the matching shim on the
    # plugin side every `g_ui.gridEditError` / `gridState` / `cachedSortedIndices` access from
    # LuaConsole hits the wrong offset and corrupts memory. Crash signature: SIGSEGV inside
    # `std::__cxx11::basic_string::clear()` on the Save / Reload / Run buttons.
    if(SMATCHET_WITH_AI)
        target_link_libraries(SmatchetPlugin_LuaConsole PUBLIC SmatchetCoreAiShim)
    endif()
    smatchet_apply_strict_warnings(SmatchetPlugin_LuaConsole)
    smatchet_assert_plugin_shim_links(SmatchetPlugin_LuaConsole)
endif()
