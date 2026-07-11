# Source/UnrealPlugins — Unreal/DX12 embedding surface + packaging.
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
# Defines (WIN32 only): SmatchetPlugin_Mcp_DX12 / SmatchetPlugin_LuaConsole_DX12
# (EXCLUDE_FROM_ALL Unreal-only plugin variants), SmatchetImGuiHost_DX12, and the
# SmatchetPackageUnrealLibs_DX12 custom packaging target that stages libs +
# headers into SMATCHET_UNREAL_THIRDPARTY_DIR for Unreal Build Tool consumption.

if(WIN32)
    if(SMATCHET_WITH_MCP AND SMATCHET_WITH_MCP_UNREAL)
        # EXCLUDE_FROM_ALL — Unreal-only target. See SmatchetCore_DX12 above.
        add_library(SmatchetPlugin_Mcp_DX12 STATIC EXCLUDE_FROM_ALL Source/Plugins/Mcp/McpPlugin.cpp Source/Plugins/Mcp/McpJsonRpcPure.cpp)
        target_include_directories(SmatchetPlugin_Mcp_DX12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source/Plugins/Mcp")
        target_link_libraries(SmatchetPlugin_Mcp_DX12 PUBLIC SmatchetCore_DX12 SmatchetCoreMcpShim)
        smatchet_apply_strict_warnings(SmatchetPlugin_Mcp_DX12)
    endif()
    if(SMATCHET_WITH_LUA_AUTOMATION)
        # TextEditor TU is built into SmatchetCore_DX12 directly (added to CORE_SOURCES below);
        # the plugin lib drops its own copy and consumes the symbol via SmatchetCore_DX12.
        # EXCLUDE_FROM_ALL — Unreal-only target. See SmatchetCore_DX12 above.
        add_library(SmatchetPlugin_LuaConsole_DX12 STATIC EXCLUDE_FROM_ALL Source/Plugins/LuaConsole/LuaConsolePlugin.cpp)
        target_include_directories(SmatchetPlugin_LuaConsole_DX12 PUBLIC
                "${CMAKE_CURRENT_SOURCE_DIR}/Source/Plugins/LuaConsole")
        target_link_libraries(SmatchetPlugin_LuaConsole_DX12 PUBLIC SmatchetCore_DX12 ImGuiLib_DX12)
        if(MSVC)
            target_compile_definitions(SmatchetPlugin_LuaConsole_DX12 PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        smatchet_apply_strict_warnings(SmatchetPlugin_LuaConsole_DX12)
    endif()

    # EXCLUDE_FROM_ALL — Unreal-only target. See SmatchetCore_DX12 above.
    add_library(SmatchetImGuiHost_DX12 STATIC EXCLUDE_FROM_ALL
            Source/Core/src/Ui/SmatchetImGuiHost.cpp
            Source/Core/src/Ui/SmatchetImGuiHostC_Diagnostics.cpp
    )
    target_include_directories(SmatchetImGuiHost_DX12 PUBLIC Source/Core/include)
    if(SMATCHET_WITH_MCP AND SMATCHET_WITH_MCP_UNREAL)
        target_include_directories(SmatchetImGuiHost_DX12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source/Plugins/Mcp")
    endif()
    if(SMATCHET_WITH_LUA_AUTOMATION)
        target_include_directories(SmatchetImGuiHost_DX12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Source/Plugins/LuaConsole")
    endif()
    target_link_libraries(SmatchetImGuiHost_DX12 PUBLIC SmatchetCore_DX12 ImGuiLib_DX12 d3d12 dxgi)
    if(SMATCHET_WITH_MCP AND SMATCHET_WITH_MCP_UNREAL)
        target_link_libraries(SmatchetImGuiHost_DX12 PUBLIC SmatchetPlugin_Mcp_DX12)
    endif()
    if(SMATCHET_WITH_LUA_AUTOMATION)
        target_link_libraries(SmatchetImGuiHost_DX12 PUBLIC SmatchetPlugin_LuaConsole_DX12)
    endif()
    if(MSVC)
        target_compile_definitions(SmatchetImGuiHost_DX12 PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    smatchet_apply_strict_warnings(SmatchetImGuiHost_DX12)

    # Package DX12 libs + public host header directly into Unreal plugin ThirdParty layout.
    set(_SMATCHET_UNREAL_INCLUDE_DIR "${SMATCHET_UNREAL_THIRDPARTY_DIR}/include")
    set(_SMATCHET_UNREAL_LIB_DIR "${SMATCHET_UNREAL_THIRDPARTY_DIR}/lib/Win64/$<IF:$<CONFIG:Debug>,Debug,Development>")
    set(_SMATCHET_PKG_DEP SmatchetImGuiHost_DX12 SmatchetCore_DX12 ImGuiLib_DX12 md4c)
    set(_SMATCHET_PKG_CMDS
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_SMATCHET_UNREAL_INCLUDE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_SMATCHET_UNREAL_LIB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_SMATCHET_UNREAL_LIB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include/Ui/SmatchetImGuiHost.h"
                "${_SMATCHET_UNREAL_INCLUDE_DIR}/SmatchetImGuiHost.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include/Ui/SmatchetImGuiHostC.h"
                "${_SMATCHET_UNREAL_INCLUDE_DIR}/SmatchetImGuiHostC.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include/SmatchetDefaults.h"
                "${_SMATCHET_UNREAL_INCLUDE_DIR}/SmatchetDefaults.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${imgui_SOURCE_DIR}/imgui.h"
                "${_SMATCHET_UNREAL_INCLUDE_DIR}/imgui.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${imgui_SOURCE_DIR}/imconfig.h"
                "${_SMATCHET_UNREAL_INCLUDE_DIR}/imconfig.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SmatchetImGuiHost_DX12>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:SmatchetImGuiHost_DX12>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SmatchetCore_DX12>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:SmatchetCore_DX12>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:ImGuiLib_DX12>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:ImGuiLib_DX12>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:cpr>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:cpr>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SQLiteCpp>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:SQLiteCpp>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:sqlite3>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:sqlite3>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:libcurl>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:libcurl>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:md4c>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:md4c>"
    )
    if(MSVC)
        list(APPEND _SMATCHET_PKG_CMDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:SmatchetImGuiHost_DX12>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/SmatchetImGuiHost_DX12.lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:SmatchetCore_DX12>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/SmatchetCore_DX12.lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:ImGuiLib_DX12>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/ImGuiLib_DX12.lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:cpr>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/cpr.lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:SQLiteCpp>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/SQLiteCpp.lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:sqlite3>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/sqlite3.lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:libcurl>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/libcurl.lib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:md4c>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/md4c.lib")
    endif()
    if(SMATCHET_WITH_MCP AND SMATCHET_WITH_MCP_UNREAL)
        list(APPEND _SMATCHET_PKG_CMDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:SmatchetPlugin_Mcp_DX12>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:SmatchetPlugin_Mcp_DX12>"
        )
        if(MSVC)
            list(APPEND _SMATCHET_PKG_CMDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:SmatchetPlugin_Mcp_DX12>"
                        "${_SMATCHET_UNREAL_LIB_DIR}/SmatchetPlugin_Mcp_DX12.lib")
        endif()
        list(APPEND _SMATCHET_PKG_DEP SmatchetPlugin_Mcp_DX12)
    endif()
    if(SMATCHET_WITH_LUA_AUTOMATION)
        list(APPEND _SMATCHET_PKG_CMDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:SmatchetPlugin_LuaConsole_DX12>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:SmatchetPlugin_LuaConsole_DX12>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:Smatchet_Lua_Internal>"
                    "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:Smatchet_Lua_Internal>")
        if(MSVC)
            list(APPEND _SMATCHET_PKG_CMDS
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:SmatchetPlugin_LuaConsole_DX12>"
                        "${_SMATCHET_UNREAL_LIB_DIR}/SmatchetPlugin_LuaConsole_DX12.lib"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:Smatchet_Lua_Internal>"
                        "${_SMATCHET_UNREAL_LIB_DIR}/Smatchet_Lua_Internal.lib")
        endif()
        list(APPEND _SMATCHET_PKG_DEP SmatchetPlugin_LuaConsole_DX12 Smatchet_Lua_Internal)
    endif()
    add_custom_target(SmatchetPackageUnrealLibs_DX12
        ${_SMATCHET_PKG_CMDS}
        DEPENDS ${_SMATCHET_PKG_DEP}
        COMMENT "Packaging Smatchet DX12 artifacts for Unreal plugin"
    )
endif()
