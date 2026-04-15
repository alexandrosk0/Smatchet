cmake_minimum_required(VERSION 3.24)

# Tell modern CMake to tolerate older third-party library policies
set(CMAKE_POLICY_VERSION_MINIMUM "3.5" CACHE STRING "Minimum CMake policy version" FORCE)

# FIX: Silence the FetchContent_Populate deprecation warning for header-only libs
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# Use Ninja from PATH when using Ninja generator (avoids stale WinGet path from another user)
if(CMAKE_GENERATOR MATCHES "Ninja")
    find_program(_NINJA_FROM_PATH ninja PATHS ENV PATH)
    if(_NINJA_FROM_PATH)
        set(CMAKE_MAKE_PROGRAM "${_NINJA_FROM_PATH}" CACHE FILEPATH "Ninja executable" FORCE)
    endif()
    unset(_NINJA_FROM_PATH CACHE)
endif()

project(SmatchetApp VERSION 1.0 LANGUAGES C CXX)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static libraries" FORCE)
set(CPR_FORCE_USE_SYSTEM_CURL OFF CACHE BOOL "Use internal cURL" FORCE)

# Enable Windows resource compilation (windres) so `.rc` files are processed.
if(WIN32)
    enable_language(RC)
endif()

# SET TO C++14 FOR MAXIMUM ENGINE COMPATIBILITY
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SMATCHET_UNREAL_THIRDPARTY_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet"
    CACHE PATH "Output directory for Unreal ThirdParty Smatchet artifacts")

# Stronger warnings on first-party targets only (FetchContent deps keep their own flags).
option(SMATCHET_ENABLE_STRICT_WARNINGS
    "Apply /W4 or -Wall -Wextra -Wunused-* to Smatchet libraries and standalone (not ImGui/GLFW)"
    ON)
option(SMATCHET_WARNINGS_AS_ERRORS
    "Treat the above warnings as errors (useful for CI)"
    OFF)

function(smatchet_apply_strict_warnings _target)
    if(NOT SMATCHET_ENABLE_STRICT_WARNINGS)
        return()
    endif()
    if(NOT TARGET "${_target}")
        return()
    endif()
    if(MSVC)
        target_compile_options("${_target}" PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # Omit -Wunused-parameter: callback-heavy UI code (ImGui, etc.) is very noisy.
        target_compile_options(
            "${_target}"
            PRIVATE
            -Wall
            -Wextra
            -Wunused-function
            -Wunused-variable
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wunused-but-set-variable>
        )
    endif()
    if(SMATCHET_WARNINGS_AS_ERRORS)
        if(MSVC)
            target_compile_options("${_target}" PRIVATE /WX)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options("${_target}" PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ==============================================================================
# 1. FETCH ALL DEPENDENCIES (Zero manual downloads required)
# ==============================================================================
include(FetchContent)

message(STATUS "Fetching nlohmann/json...")
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

message(STATUS "Fetching cpr (C++ Requests)...")
FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git GIT_TAG 1.9.2)
FetchContent_MakeAvailable(cpr)

message(STATUS "Fetching SQLiteCpp...")
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "Disable linting" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "Disable cppcheck" FORCE)
FetchContent_Declare(sqlitecpp GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git GIT_TAG 3.3.1)
# Fetch + patch to avoid CMake deprecation warnings about < 3.10 compatibility.
FetchContent_GetProperties(sqlitecpp)
if(NOT sqlitecpp_POPULATED)
    FetchContent_Populate(sqlitecpp)
endif()

set(_sqlitecpp_cmakelists "${sqlitecpp_SOURCE_DIR}/CMakeLists.txt")
if(EXISTS "${_sqlitecpp_cmakelists}")
    file(READ "${_sqlitecpp_cmakelists}" _sqlitecpp_cmakelists_contents)
    string(REPLACE "cmake_minimum_required(VERSION 3.5)"
                   "cmake_minimum_required(VERSION 3.10)"
                   _sqlitecpp_patched_contents
                   "${_sqlitecpp_cmakelists_contents}")
    if(NOT _sqlitecpp_patched_contents STREQUAL _sqlitecpp_cmakelists_contents)
        file(WRITE "${_sqlitecpp_cmakelists}" "${_sqlitecpp_patched_contents}")
    endif()
endif()

add_subdirectory("${sqlitecpp_SOURCE_DIR}" "${sqlitecpp_BINARY_DIR}" EXCLUDE_FROM_ALL)

message(STATUS "Fetching cpp-httplib...")
FetchContent_Declare(httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git GIT_TAG v0.14.1)
FetchContent_MakeAvailable(httplib)


message(STATUS "Fetching ghc::filesystem (C++14 Polyfill)...")
FetchContent_Declare(ghc_filesystem
    GIT_REPOSITORY https://github.com/gulrak/filesystem.git
    GIT_TAG v1.5.14
)

# FIX: CMake <3.10 compatibility removal warning
# The upstream ghc_filesystem CMakeLists starts with:
#   cmake_minimum_required(VERSION 3.7.2)
# Newer CMake versions emit a deprecation warning for this.
FetchContent_GetProperties(ghc_filesystem)
if(NOT ghc_filesystem_POPULATED)
    FetchContent_Populate(ghc_filesystem)
endif()

set(_ghc_filesystem_cmakelists "${ghc_filesystem_SOURCE_DIR}/CMakeLists.txt")
if(EXISTS "${_ghc_filesystem_cmakelists}")
    file(READ "${_ghc_filesystem_cmakelists}" _ghc_filesystem_cmakelists_contents)
    string(REPLACE "cmake_minimum_required(VERSION 3.7.2)"
                   "cmake_minimum_required(VERSION 3.10)"
                   _ghc_filesystem_cmakelists_contents_patched
                   "${_ghc_filesystem_cmakelists_contents}")
    if(NOT _ghc_filesystem_cmakelists_contents_patched STREQUAL _ghc_filesystem_cmakelists_contents)
        file(WRITE "${_ghc_filesystem_cmakelists}" "${_ghc_filesystem_cmakelists_contents_patched}")
    endif()
endif()

add_subdirectory("${ghc_filesystem_SOURCE_DIR}" "${ghc_filesystem_BINARY_DIR}" EXCLUDE_FROM_ALL)

message(STATUS "Fetching GLFW...")
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.3.8)
FetchContent_MakeAvailable(glfw)

# ==============================================================================
# LUA & SOL2 (C++14 COMPATIBLE)
# ==============================================================================
set(LUA_SRC_DIR "${CMAKE_BINARY_DIR}/_deps/lua-src")
if(NOT EXISTS "${LUA_SRC_DIR}")
    message(STATUS "Downloading Lua 5.3.6 source...")
    file(DOWNLOAD https://www.lua.org/ftp/lua-5.3.6.tar.gz ${CMAKE_BINARY_DIR}/lua-5.3.6.tar.gz)
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf ${CMAKE_BINARY_DIR}/lua-5.3.6.tar.gz WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/_deps)
    file(RENAME ${CMAKE_BINARY_DIR}/_deps/lua-5.3.6 ${LUA_SRC_DIR})
endif()

# 1. Build Lua under a UNIQUE name to avoid clashing with sol2's internal 'LuaLib'
file(GLOB LUA_SOURCES "${LUA_SRC_DIR}/src/*.c")
list(REMOVE_ITEM LUA_SOURCES "${LUA_SRC_DIR}/src/lua.c" "${LUA_SRC_DIR}/src/luac.c")

add_library(Smatchet_Lua_Internal STATIC ${LUA_SOURCES})
target_include_directories(Smatchet_Lua_Internal PUBLIC "$<BUILD_INTERFACE:${LUA_SRC_DIR}/src>")

# 2. Tell sol2 where the headers are and that Lua is ALREADY FOUND
set(LUA_INCLUDE_DIR "${LUA_SRC_DIR}/src" CACHE PATH "" FORCE)
set(LUA_LIBRARIES Smatchet_Lua_Internal CACHE STRING "" FORCE)
set(SOL2_BUILD_LUA OFF CACHE BOOL "" FORCE) # Tell sol2 NOT to try building its own Lua

FetchContent_Declare(sol2 GIT_REPOSITORY https://github.com/ThePhD/sol2.git GIT_TAG v2.20.6)

# FIX: Silence the FetchContent deprecation warning emitted by sol2's
# fetched CMakeLists.txt (it uses cmake_minimum_required(VERSION 3.5.0)).
# Newer CMake versions deprecate compatibility with CMake < 3.10.
FetchContent_GetProperties(sol2)
if(NOT sol2_POPULATED)
    FetchContent_Populate(sol2)
endif()

set(_sol2_cmakelists "${sol2_SOURCE_DIR}/CMakeLists.txt")
if(EXISTS "${_sol2_cmakelists}")
    file(READ "${_sol2_cmakelists}" _sol2_cmakelists_contents)

    string(REPLACE "cmake_minimum_required(VERSION 3.5.0)"
                   "cmake_minimum_required(VERSION 3.10.0)"
                   _sol2_patched_contents
                   "${_sol2_cmakelists_contents}")
    string(REPLACE "cmake_minimum_required(VERSION 3.5)"
                   "cmake_minimum_required(VERSION 3.10.0)"
                   _sol2_patched_contents
                   "${_sol2_patched_contents}")

    if(NOT _sol2_patched_contents STREQUAL _sol2_cmakelists_contents)
        file(WRITE "${_sol2_cmakelists}" "${_sol2_patched_contents}")
    endif()
endif()

add_subdirectory("${sol2_SOURCE_DIR}" "${sol2_BINARY_DIR}" EXCLUDE_FROM_ALL)



# Compile Dear ImGui (with GLFW and OpenGL3 backends)
message(STATUS "Fetching Dear ImGui...")
FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG docking
)

FetchContent_Populate(imgui)
file(GLOB IMGUI_SOURCES "${imgui_SOURCE_DIR}/*.cpp")
list(APPEND IMGUI_SOURCES "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
add_library(ImGuiLib STATIC ${IMGUI_SOURCES})
target_include_directories(ImGuiLib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${glfw_SOURCE_DIR}/include
)

# Dear ImGui variant for Unreal/DX12 embedding (no GLFW/OpenGL dependency).
if(WIN32)
    file(GLOB IMGUI_DX12_SOURCES "${imgui_SOURCE_DIR}/*.cpp")
    list(APPEND IMGUI_DX12_SOURCES "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp")
    add_library(ImGuiLib_DX12 STATIC ${IMGUI_DX12_SOURCES})
    target_include_directories(ImGuiLib_DX12 PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )
endif()

# ==============================================================================
# 3. SMATCHET CORE LIBRARY (Engine-Agnostic)
# ==============================================================================
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source_Core/src/*.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetImGuiHost.cpp")
# GLOB snapshots at configure time; pick up new TUs if the cache predates them (no duplicate paths).
set(_SMATCHET_BLAME_SYNTAX "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/BlameSyntaxHighlight.cpp")
if(EXISTS "${_SMATCHET_BLAME_SYNTAX}")
    list(FIND CORE_SOURCES "${_SMATCHET_BLAME_SYNTAX}" _smatchet_blame_syntax_idx)
    if(_smatchet_blame_syntax_idx EQUAL -1)
        list(APPEND CORE_SOURCES "${_SMATCHET_BLAME_SYNTAX}")
    endif()
endif()
unset(_smatchet_blame_syntax_idx)
unset(_SMATCHET_BLAME_SYNTAX)
add_library(SmatchetCore STATIC ${CORE_SOURCES})

# FIX: Explicitly define the sol2 and lua paths because FetchContent variables
# can sometimes be finicky with older sol2 tags.
set(SOL2_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/sol2-src")
set(LUA_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/lua-src/src")

target_include_directories(SmatchetCore PUBLIC
        Source_Core/include
        ${sqlitecpp_SOURCE_DIR}/include
        ${LUA_SRC_DIR}/src
        ${sol2_SOURCE_DIR}/single
)
target_include_directories(SmatchetCore PRIVATE
        Source_Core/src
        ${sol2_SOURCE_DIR}/single
        ${LUA_SRC_DIR}/src
)

target_link_libraries(SmatchetCore PUBLIC
        nlohmann_json::nlohmann_json
        SQLiteCpp
        cpr::cpr
        httplib::httplib
)

target_link_libraries(SmatchetCore PRIVATE
        Smatchet_Lua_Internal  # This provides the actual Lua symbols
        ghc_filesystem
        ImGuiLib
        glfw
)
target_compile_definitions(SmatchetCore PRIVATE SMATCHET_ENABLE_OPENGL_ATTACHMENT_THUMBNAILS=1)
if(WIN32)
    target_link_libraries(SmatchetCore PRIVATE shell32 windowscodecs ole32)
endif()
smatchet_apply_strict_warnings(SmatchetCore)

if(WIN32)
    add_library(SmatchetCore_DX12 STATIC ${CORE_SOURCES})
    target_include_directories(SmatchetCore_DX12 PUBLIC
            Source_Core/include
            ${sqlitecpp_SOURCE_DIR}/include
            ${LUA_SRC_DIR}/src
            ${sol2_SOURCE_DIR}/single
    )
    target_include_directories(SmatchetCore_DX12 PRIVATE
            Source_Core/src
            ${sol2_SOURCE_DIR}/single
            ${LUA_SRC_DIR}/src
    )
    target_link_libraries(SmatchetCore_DX12 PUBLIC
            nlohmann_json::nlohmann_json
            SQLiteCpp
            cpr::cpr
            httplib::httplib
    )
    target_link_libraries(SmatchetCore_DX12 PRIVATE
            Smatchet_Lua_Internal
            ghc_filesystem
            ImGuiLib_DX12
            d3d12
            dxgi
            shell32
    )
    smatchet_apply_strict_warnings(SmatchetCore_DX12)
endif()

# ==============================================================================
# 3b. STATIC PLUGINS (same IPlugin contract as future DLLs)
# ==============================================================================
add_library(SmatchetPlugin_Mcp STATIC Plugins/Mcp/McpPlugin.cpp)
target_include_directories(SmatchetPlugin_Mcp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Mcp")
target_link_libraries(SmatchetPlugin_Mcp PUBLIC SmatchetCore)

add_library(SmatchetPlugin_LuaConsole STATIC Plugins/LuaConsole/LuaConsolePlugin.cpp)
target_include_directories(SmatchetPlugin_LuaConsole PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/LuaConsole")
target_link_libraries(SmatchetPlugin_LuaConsole PUBLIC SmatchetCore ImGuiLib)
smatchet_apply_strict_warnings(SmatchetPlugin_Mcp)
smatchet_apply_strict_warnings(SmatchetPlugin_LuaConsole)

if(WIN32)
    add_library(SmatchetPlugin_Mcp_DX12 STATIC Plugins/Mcp/McpPlugin.cpp)
    target_include_directories(SmatchetPlugin_Mcp_DX12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Mcp")
    target_link_libraries(SmatchetPlugin_Mcp_DX12 PUBLIC SmatchetCore_DX12)

    add_library(SmatchetPlugin_LuaConsole_DX12 STATIC Plugins/LuaConsole/LuaConsolePlugin.cpp)
    target_include_directories(SmatchetPlugin_LuaConsole_DX12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/LuaConsole")
    target_link_libraries(SmatchetPlugin_LuaConsole_DX12 PUBLIC SmatchetCore_DX12 ImGuiLib_DX12)
    smatchet_apply_strict_warnings(SmatchetPlugin_Mcp_DX12)
    smatchet_apply_strict_warnings(SmatchetPlugin_LuaConsole_DX12)

    add_library(SmatchetImGuiHost_DX12 STATIC
            Source_Core/src/SmatchetImGuiHost.cpp
            Source_Core/src/SmatchetImGuiHostC_Diagnostics.cpp
    )
    target_include_directories(SmatchetImGuiHost_DX12 PUBLIC
            Source_Core/include
            "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Mcp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/LuaConsole"
    )
    target_link_libraries(SmatchetImGuiHost_DX12 PUBLIC
            SmatchetCore_DX12
            SmatchetPlugin_Mcp_DX12
            SmatchetPlugin_LuaConsole_DX12
            ImGuiLib_DX12
            d3d12
            dxgi
    )
    smatchet_apply_strict_warnings(SmatchetImGuiHost_DX12)

    # Package DX12 libs + public host header directly into Unreal plugin ThirdParty layout.
    set(_SMATCHET_UNREAL_INCLUDE_DIR "${SMATCHET_UNREAL_THIRDPARTY_DIR}/include")
    set(_SMATCHET_UNREAL_LIB_DIR "${SMATCHET_UNREAL_THIRDPARTY_DIR}/lib/Win64/$<IF:$<CONFIG:Debug>,Debug,Development>")
    add_custom_target(SmatchetPackageUnrealLibs_DX12
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_SMATCHET_UNREAL_INCLUDE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_SMATCHET_UNREAL_LIB_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/include/SmatchetImGuiHost.h"
                "${_SMATCHET_UNREAL_INCLUDE_DIR}/SmatchetImGuiHost.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/include/SmatchetImGuiHostC.h"
                "${_SMATCHET_UNREAL_INCLUDE_DIR}/SmatchetImGuiHostC.h"
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
                "$<TARGET_FILE:SmatchetPlugin_Mcp_DX12>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:SmatchetPlugin_Mcp_DX12>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SmatchetPlugin_LuaConsole_DX12>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:SmatchetPlugin_LuaConsole_DX12>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SmatchetCore_DX12>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:SmatchetCore_DX12>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:ImGuiLib_DX12>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:ImGuiLib_DX12>"
        # Static dependencies needed by the core when linked from Unreal.
        # (Static library link-time deps are not automatically pulled by UBT.)
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
                "$<TARGET_FILE:Smatchet_Lua_Internal>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:Smatchet_Lua_Internal>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:libcurl>"
                "${_SMATCHET_UNREAL_LIB_DIR}/$<TARGET_FILE_NAME:libcurl>"
        DEPENDS
            SmatchetImGuiHost_DX12
            SmatchetPlugin_Mcp_DX12
            SmatchetPlugin_LuaConsole_DX12
            SmatchetCore_DX12
            ImGuiLib_DX12
        COMMENT "Packaging Smatchet DX12 artifacts for Unreal plugin"
    )
endif()

# ==============================================================================
# 4. SMATCHET STANDALONE EXECUTABLE
# ==============================================================================
file(GLOB STANDALONE_SOURCES "Target_Standalone/*.cpp")

if(WIN32)
    # Copy the icon into the repo (only if it doesn't already exist).
    # This keeps the build working even if the icon only exists locally.
    set(_SMATCHET_ICO_SRC "C:/Users/alexk/Downloads/smatchet.ico")
    set(_SMATCHET_ICO_DEST "${CMAKE_CURRENT_SOURCE_DIR}/Target_Standalone/smatchet.ico")

    if(NOT EXISTS "${_SMATCHET_ICO_DEST}")
        if(EXISTS "${_SMATCHET_ICO_SRC}")
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Target_Standalone")
            file(COPY "${_SMATCHET_ICO_SRC}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/Target_Standalone")
            message(STATUS "Copied smatchet.ico into repo at: ${_SMATCHET_ICO_DEST}")
        else()
            message(FATAL_ERROR "smatchet.ico not found. Expected either at '${_SMATCHET_ICO_DEST}' or '${_SMATCHET_ICO_SRC}'.")
        endif()
    endif()

    # Generate a `.rc` file with an embedded absolute icon path.
    file(TO_CMAKE_PATH "${_SMATCHET_ICO_DEST}" _SMATCHET_ICO_PATH_CMAKE)
    set(SMATCHET_ICO_PATH "${_SMATCHET_ICO_PATH_CMAKE}")

    set(_SMATCHET_RC_IN "${CMAKE_CURRENT_SOURCE_DIR}/Target_Standalone/smatchet.rc.in")
    set(_SMATCHET_RC_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/smatchet.rc")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

    configure_file("${_SMATCHET_RC_IN}" "${_SMATCHET_RC_OUT}" @ONLY)

    list(APPEND STANDALONE_SOURCES "${_SMATCHET_RC_OUT}")
endif()

add_executable(SmatchetStandalone ${STANDALONE_SOURCES})
smatchet_apply_strict_warnings(SmatchetStandalone)

set(SMATCHET_LUA_SCRIPTS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Scripts")
add_custom_command(TARGET SmatchetStandalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${SMATCHET_LUA_SCRIPTS_SRC}" "$<TARGET_FILE_DIR:SmatchetStandalone>/Scripts"
        COMMENT "Copy Scripts/*.lua next to SmatchetStandalone"
        VERBATIM)

# Fix MinGW/GCC "relocation truncated to fit" when linking sol2/CachedTicket in Debug
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(SmatchetStandalone PRIVATE -mcmodel=large)
    target_link_options(SmatchetStandalone PRIVATE -mcmodel=large)
    # MinGW ld can overflow COFF debug relocations in this translation unit.
    # Force minimal debug emission for standalone target to avoid linker overflow.
    target_compile_options(SmatchetStandalone PRIVATE -g0)
endif()

target_link_libraries(SmatchetStandalone PRIVATE
        SmatchetCore
        SmatchetPlugin_Mcp
        SmatchetPlugin_LuaConsole
        ImGuiLib
        glfw
)

if(WIN32)
    target_link_libraries(SmatchetStandalone PRIVATE opengl32 ws2_32 crypt32)
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(OPENGL_LIBRARY OpenGL)
    target_link_libraries(SmatchetStandalone PRIVATE ${COCOA_LIBRARY} ${OPENGL_LIBRARY})
endif()