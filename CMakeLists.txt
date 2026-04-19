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
# Bundled libcurl CMake probes ioctlsocket(FIONBIO) with int*; GCC 14 rejects int* vs u_long* →
# HAVE_IOCTLSOCKET_FIONBIO unset → nonblock.c #errors. Winsock API uses u_long; safe to force on MinGW.
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(HAVE_IOCTLSOCKET_FIONBIO 1 CACHE INTERNAL "curl: ioctlsocket FIONBIO (MinGW GCC14+ probe fix)" FORCE)
endif()
FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git GIT_TAG 1.9.2)
FetchContent_MakeAvailable(cpr)
# MSVC C4244: Body(File) used int for tellg() (streamoff). Patch vendored header after fetch.
set(_cpr_body_h "${cpr_SOURCE_DIR}/include/cpr/body.h")
if(EXISTS "${_cpr_body_h}")
    file(READ "${_cpr_body_h}" _cpr_body_h_contents)
    set(_cpr_body_h_patched "${_cpr_body_h_contents}")
    string(REPLACE "int length = is.tellg();"
                   "const std::streamoff length = is.tellg();"
                   _cpr_body_h_patched
                   "${_cpr_body_h_patched}")
    string(REPLACE "buffer.resize(length);"
                   "buffer.resize(static_cast<size_t>(length));"
                   _cpr_body_h_patched
                   "${_cpr_body_h_patched}")
    string(REPLACE "is.read(&buffer[0], length);"
                   "is.read(buffer.data(), static_cast<std::streamsize>(length));"
                   _cpr_body_h_patched
                   "${_cpr_body_h_patched}")
    if(NOT _cpr_body_h_patched STREQUAL _cpr_body_h_contents)
        file(WRITE "${_cpr_body_h}" "${_cpr_body_h_patched}")
        message(STATUS "Patched cpr include/cpr/body.h (streamoff / resize / read).")
    endif()
endif()

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

# MinGW/GCC: fixed-width integer typedefs no longer leak from libstdc++ transitive includes; SQLiteCpp 3.3.1
# uses int32_t/int64_t/... without <cstdint> in Statement.h (Column.h, Database.h include it indirectly).
set(_sqlitecpp_statement_h "${sqlitecpp_SOURCE_DIR}/include/SQLiteCpp/Statement.h")
if(EXISTS "${_sqlitecpp_statement_h}")
    file(READ "${_sqlitecpp_statement_h}" _sqlitecpp_statement_h_contents)
    if(NOT _sqlitecpp_statement_h_contents MATCHES "#include <cstdint>")
        string(REPLACE "#include <memory>"
                       "#include <memory>\n#include <cstdint>"
                       _sqlitecpp_statement_h_patched
                       "${_sqlitecpp_statement_h_contents}")
        if(NOT _sqlitecpp_statement_h_patched STREQUAL _sqlitecpp_statement_h_contents)
            file(WRITE "${_sqlitecpp_statement_h}" "${_sqlitecpp_statement_h_patched}")
            message(STATUS "Patched SQLiteCpp include/SQLiteCpp/Statement.h (<cstdint> for MinGW/GCC).")
        endif()
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
        # Pinned to a specific commit on the docking branch (reproducible builds / supply chain).
        # v1.92.7-docking-39-g329c5a6b3
        GIT_TAG 329c5a6b3be75ebf54506d3ae94b836ffcf19fa0
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
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetImGuiHostC_Diagnostics.cpp")
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

# FIX: Explicitly define the sol2 and lua paths because FetchContent variables
# can sometimes be finicky with older sol2 tags.
set(SOL2_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/sol2-src")
set(LUA_INCLUDE_DIR "${CMAKE_BINARY_DIR}/_deps/lua-src/src")

add_library(SmatchetCoreInterface INTERFACE)
target_include_directories(SmatchetCoreInterface INTERFACE
        Source_Core/include
        ${sqlitecpp_SOURCE_DIR}/include
        ${LUA_SRC_DIR}/src
        ${sol2_SOURCE_DIR}/single
)

target_link_libraries(SmatchetCoreInterface INTERFACE
        nlohmann_json::nlohmann_json
        SQLiteCpp
        cpr::cpr
        httplib::httplib
)

function(smatchet_configure_core_impl_target _target)
    target_link_libraries("${_target}" PUBLIC SmatchetCoreInterface)
    target_include_directories("${_target}" PRIVATE
            Source_Core/src
            ${sol2_SOURCE_DIR}/single
            ${LUA_SRC_DIR}/src
    )
    target_link_libraries("${_target}" PRIVATE
            Smatchet_Lua_Internal  # This provides the actual Lua symbols
            ghc_filesystem
    )
endfunction()

function(smatchet_configure_opengl_core_impl_target _target)
    smatchet_configure_core_impl_target("${_target}")
    target_link_libraries("${_target}" PRIVATE ImGuiLib glfw)
    if(WIN32)
        # WIC (windowscodecs/ole32) is used to decode attachment bitmaps into RGBA32 for ImGui's
        # backend-agnostic texture API (ImTextureData) when previewing image attachments.
        target_link_libraries("${_target}" PRIVATE shell32 windowscodecs ole32)
    endif()
endfunction()

function(smatchet_configure_dx12_core_impl_target _target)
    smatchet_configure_core_impl_target("${_target}")
    target_link_libraries("${_target}" PRIVATE
            ImGuiLib_DX12
            d3d12
            dxgi
            shell32
            # WIC for attachment image decoding; textures are uploaded via ImTextureData so DX12 and
            # OpenGL both render the same preview thumbnails.
            windowscodecs
            ole32
    )
    # SmatchetCore_DX12 is packaged into the Unreal plugin; enable embed-only UI affordances.
    target_compile_definitions("${_target}" PRIVATE SMATCHET_EMBEDDED_IN_UNREAL=1)
endfunction()

if(WIN32)
    add_library(SmatchetCore_DX12 STATIC ${CORE_SOURCES})
    smatchet_configure_dx12_core_impl_target(SmatchetCore_DX12)
    smatchet_apply_strict_warnings(SmatchetCore_DX12)
endif()

# ==============================================================================
# 3b. STATIC PLUGINS (same IPlugin contract as future DLLs)
# ==============================================================================
add_library(SmatchetPlugin_Mcp STATIC Plugins/Mcp/McpPlugin.cpp)
target_include_directories(SmatchetPlugin_Mcp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Mcp")
target_link_libraries(SmatchetPlugin_Mcp PUBLIC SmatchetCoreInterface)

add_library(SmatchetPlugin_LuaConsole STATIC Plugins/LuaConsole/LuaConsolePlugin.cpp)
target_include_directories(SmatchetPlugin_LuaConsole PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/LuaConsole")
target_link_libraries(SmatchetPlugin_LuaConsole PUBLIC SmatchetCoreInterface ImGuiLib)
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
    set(_SMATCHET_ICO_DEST "${CMAKE_CURRENT_SOURCE_DIR}/Target_Standalone/smatchet.ico")
    if(EXISTS "${_SMATCHET_ICO_DEST}")
        # Generate a `.rc` file with an embedded absolute icon path.
        file(TO_CMAKE_PATH "${_SMATCHET_ICO_DEST}" _SMATCHET_ICO_PATH_CMAKE)
        set(SMATCHET_ICO_PATH "${_SMATCHET_ICO_PATH_CMAKE}")

        set(_SMATCHET_RC_IN "${CMAKE_CURRENT_SOURCE_DIR}/Target_Standalone/smatchet.rc.in")
        set(_SMATCHET_RC_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/smatchet.rc")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
        configure_file("${_SMATCHET_RC_IN}" "${_SMATCHET_RC_OUT}" @ONLY)
        list(APPEND STANDALONE_SOURCES "${_SMATCHET_RC_OUT}")
    else()
        message(WARNING "Smatchet icon not found at ${_SMATCHET_ICO_DEST}; building without embedded icon.")
    endif()
endif()

add_executable(SmatchetStandalone ${STANDALONE_SOURCES} ${CORE_SOURCES})
smatchet_configure_opengl_core_impl_target(SmatchetStandalone)
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
        SmatchetPlugin_Mcp
        SmatchetPlugin_LuaConsole
)

if(WIN32)
    target_link_libraries(SmatchetStandalone PRIVATE opengl32 ws2_32 crypt32)
    # MinGW release exe otherwise depends on libgcc/libstdc++ DLLs; wrong DLL on PATH → 0xC0000139
    # (entry point not found) before main — no logs, "release doesn't run".
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(SmatchetStandalone PRIVATE -static-libgcc -static-libstdc++)
        # Still links libwinpthread-1.dll; copy toolchain DLL next to exe so PATH doesn't load a wrong build.
        get_filename_component(_SMATCHET_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(_SMATCHET_WINPTHREAD_DLL "${_SMATCHET_MINGW_BIN}/libwinpthread-1.dll")
        if(EXISTS "${_SMATCHET_WINPTHREAD_DLL}")
            add_custom_command(
                TARGET SmatchetStandalone
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_SMATCHET_WINPTHREAD_DLL}"
                        "$<TARGET_FILE_DIR:SmatchetStandalone>/"
                COMMENT "Copy MinGW libwinpthread-1.dll next to SmatchetStandalone"
                VERBATIM)
        endif()
    endif()
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(OPENGL_LIBRARY OpenGL)
    target_link_libraries(SmatchetStandalone PRIVATE ${COCOA_LIBRARY} ${OPENGL_LIBRARY})
endif()