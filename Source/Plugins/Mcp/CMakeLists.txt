# Source/Plugins/Mcp — MCP static plugin (SmatchetPlugin_Mcp).
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
# Defines (SMATCHET_WITH_MCP only): SmatchetPlugin_Mcp. The Unreal/DX12 sibling
# (SmatchetPlugin_Mcp_DX12) lives in Source/UnrealPlugins/CMakeLists.txt.

if(SMATCHET_WITH_MCP)
    add_library(SmatchetPlugin_Mcp STATIC Source/Plugins/Mcp/McpPlugin.cpp Source/Plugins/Mcp/McpJsonRpcPure.cpp)
    target_include_directories(SmatchetPlugin_Mcp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source/Plugins/Mcp")
    target_link_libraries(SmatchetPlugin_Mcp PUBLIC SmatchetCoreInterface SmatchetCoreMcpShim)
    # See AI-shim note on SmatchetPlugin_LuaConsole — UiDrawSession layout depends on
    # SMATCHET_WITH_AI. Plugins that include Source/Core headers must match the Source/Core
    # define state or risk silent member-offset drift.
    if(SMATCHET_WITH_AI)
        target_link_libraries(SmatchetPlugin_Mcp PUBLIC SmatchetCoreAiShim)
    endif()
    smatchet_apply_strict_warnings(SmatchetPlugin_Mcp)
    smatchet_assert_plugin_shim_links(SmatchetPlugin_Mcp)
endif()
