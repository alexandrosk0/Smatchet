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

project(SmatchetApp VERSION 0.6.7 LANGUAGES C CXX)
set(SMATCHET_GITHUB_RELEASE_REPO "alexandrosk0/Smatchet" CACHE STRING
    "GitHub owner/repo used by the standalone updater")
add_compile_definitions(
    SMATCHET_APP_VERSION="${PROJECT_VERSION}"
    SMATCHET_GITHUB_RELEASE_REPO="${SMATCHET_GITHUB_RELEASE_REPO}"
)

# ==============================================================================
# Iteration-speed knobs — all auto-detected and gracefully degrade. A fresh
# clone with `cmake --preset ninja-publish-msys2 && cmake --build --preset ninja-publish-msys2`
# still works whether or not ccache / lld are installed.
# See BUILD_ITERATION_PLAN.md for the rationale and benchmarks.
# ==============================================================================

# ccache (and sccache) — drop-in compiler-output cache. Saves 3-10× on warm
# rebuilds. VS generator support exists but is finicky on Windows; gate to the
# Ninja / Makefile generators where it's robust. Set SMATCHET_NO_CCACHE=1 in
# the env to opt out.
#
# Two ccache settings are tuned automatically for this project:
#   * sloppiness — relax the timestamp checks that otherwise force misses on
#     PCH-using builds and on freshly-cloned trees (mtimes differ from build to
#     build).
#   * basedir — rewrite absolute paths to project-relative so cache hits move
#     across build directories and worktrees. Without this, every fresh build
#     dir starts from a cold cache.
if(CMAKE_GENERATOR MATCHES "Ninja|Makefile" AND NOT DEFINED ENV{SMATCHET_NO_CCACHE})
    find_program(SMATCHET_CCACHE_PROGRAM NAMES ccache sccache)
    if(SMATCHET_CCACHE_PROGRAM)
        # Wrap ccache via `cmake -E env` so the env vars propagate at build time
        # (set(ENV{...}) only affects the configure process; ninja won't see it).
        # The launcher list is invoked as a chain:
        #   cmake -E env CCACHE_SLOPPINESS=... CCACHE_BASEDIR=... ccache <compiler> <args>
        # which is the canonical recipe for portable ccache+CMake on Windows.
        set(_smatchet_ccache_launcher
            "${CMAKE_COMMAND}"
            "-E" "env"
            "CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime"
            "CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}"
            "${SMATCHET_CCACHE_PROGRAM}")
        set(CMAKE_C_COMPILER_LAUNCHER   "${_smatchet_ccache_launcher}" CACHE STRING "Compile-cache launcher (C)"   FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${_smatchet_ccache_launcher}" CACHE STRING "Compile-cache launcher (C++)" FORCE)
        message(STATUS "Compile cache: ${SMATCHET_CCACHE_PROGRAM}")
        message(STATUS "  CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime,include_file_ctime")
        message(STATUS "  CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}")
    endif()
endif()

# lld — much faster than BFD ld for static-link-heavy targets, multi-threaded
# by default. Detected by looking for `ld.lld` in the same directory as the
# active compiler — i.e. a MinGW toolchain that bundles lld. We don't try
# to use a standalone LLVM `ld.lld` together with a separately-installed
# MinGW GCC: that combo has a known interaction where collect2 invokes lld
# without forwarding MinGW's MSVC-import-lib search semantics, and link
# fails with `unable to find library -lgdi32` even when the lib clearly
# exists. The fix on Windows is to install a MinGW that bundles lld:
#   * MSYS2:    pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-lld
#   * winlibs:  download a UCRT 13+ build from https://winlibs.com/ — they
#               ship `ld.lld.exe` next to `g++.exe` already.
# Then ensure that MinGW's `bin/` is the active toolchain (the CMake compiler
# detected). On a fresh clone with neither setup, BFD ld is used and the
# build still works — just slower.
# Set `SMATCHET_NO_LLD=1` in the env to opt out unconditionally.
# Skipped under MSVC (link.exe is the right tool there).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC AND NOT DEFINED ENV{SMATCHET_NO_LLD})
    get_filename_component(_smatchet_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(SMATCHET_LLD_PROGRAM
        NAMES ld.lld
        HINTS "${_smatchet_compiler_dir}"
        NO_DEFAULT_PATH
        NO_CACHE
        DOC "ld.lld bundled with the active toolchain")
    if(SMATCHET_LLD_PROGRAM)
        add_link_options(-fuse-ld=lld)
        message(STATUS "Linker: ld.lld at ${SMATCHET_LLD_PROGRAM} (multi-threaded by default)")
    else()
        message(STATUS "Linker: BFD ld (install MinGW lld next to g++ for faster, multi-threaded linking)")
    endif()
endif()

# Job pool: cap concurrent links so simultaneous static links of large targets
# don't thrash RAM (each materializes ~200 MB of static-archive symbols).
# Compiles still run at full -j. Ninja-only.
if(CMAKE_GENERATOR MATCHES "Ninja")
    set_property(GLOBAL PROPERTY JOB_POOLS smatchet_link_pool=2)
endif()

# Precompiled header (Standalone target only). The four heaviest headers
# (imgui.h, nlohmann/json.hpp, windows.h, common <string>/<vector>/...) cost
# ~30-60% of compile time across the 56 TUs that include them. Defaults ON;
# `-DSMATCHET_USE_PCH=OFF` to opt out (e.g. when bisecting compile errors).
option(SMATCHET_USE_PCH
    "Use a precompiled header on SmatchetStandalone (faster compile; off when bisecting)"
    ON)

# Link-time optimization for SmatchetStandalone. Slows configure+build
# substantially (LTO has to defer codegen to link), but produces a smaller,
# faster binary — worth it for shipping artifacts, not worth it for daily
# iteration. Default OFF; the `ninja-publish-msys2` preset turns it on.
option(SMATCHET_ENABLE_LTO
    "Enable link-time optimisation (LTO/IPO) on eligible targets"
    OFF)
option(SMATCHET_WINDOWS_FULLY_STATIC_RUNTIME
    "Link MinGW standalone runtime fully static for one-file Windows publish artifacts"
    OFF)

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

option(SMATCHET_WITH_LUA_AUTOMATION "Build with Lua/sol2 field automation and Lua console plugin" ON)
option(SMATCHET_WITH_MCP "Build with MCP plugin (Model Context Protocol server)" ON)
option(SMATCHET_WITH_AI "Build with AI assistant side panel (provider-pluggable IAiClient)" ON)
option(SMATCHET_WITH_WHISPER "Build with Whisper dictation plugin (push-to-talk; Phase A shell)" ON)
# Agentic flow (triage + handoff). T1 introduces the gate; full feature surface lands across
# T1-T9 (triage) + H0-H10 (handoff) per docs/design/agentic-flow-implementation.md. OFF excludes
# every agentic-tagged TU from the build (source-list-conditional in section 3 below) and erases
# the SMATCHET_WITH_AGENTIC compile definition, so any call site referencing the gated symbols
# must guard its include via `#if defined(SMATCHET_WITH_AGENTIC)`.
option(SMATCHET_WITH_AGENTIC "Build with agentic flow (triage + handoff coding harness)" ON)
# Sub-option — flip ON to fetch whisper.cpp v1.7.4 and link it into
# SmatchetPlugin_Whisper. Default OFF gates the binary-size + MinGW UCRT build
# risk away from the Phase C release; the WhisperLocal API surface still
# compiles, but Transcribe() returns "local backend not built". See
# docs/design/whisper-dictation.md § Deviations for the multi-PR rationale.
option(SMATCHET_WHISPER_LOCAL_BACKEND
    "Fetch + link whisper.cpp for on-device Whisper inference (large binary impact)" OFF)
# Unreal-packaged DX12 artifacts intentionally ship without Whisper (WASAPI / whisper.cpp /
# global-hotkey hook would pollute the Unreal header path). Mirror SMATCHET_WITH_MCP_UNREAL.
if(SMATCHET_EMBEDDED_IN_UNREAL)
    set(SMATCHET_WITH_WHISPER OFF)
    set(SMATCHET_WHISPER_LOCAL_BACKEND OFF)
    # Agentic flow ships only on the Standalone — the DX12 target stays free of GitHub-PAT
    # persistence, claude subprocess spawning, and the related cpr / subprocess surface.
    set(SMATCHET_WITH_AGENTIC OFF)
endif()
option(SMATCHET_BUILD_TESTS "Build CTest target for pure-logic Source_Core unit tests (doctest)" OFF)
option(SMATCHET_BUILD_UI_TESTS "Build ImGui Test Engine bucket-E surface (in-process; opt-in)" OFF)
option(SMATCHET_BUILD_LUA_TESTS "Build SmatchetLuaTests binary (sandbox + timeout + stubs-compile drift sentinel; requires SMATCHET_WITH_LUA_AUTOMATION=ON)" OFF)
option(SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME
    "Standalone: create GLFW window hidden; show after first SwapBuffers (perceived startup)"
    ON)
# Unreal-packaged DX12 artifacts intentionally ship without MCP server support.
set(SMATCHET_WITH_MCP_UNREAL OFF)


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
# Must be set before include(FetchContent): otherwise clones default to
# ${CMAKE_BINARY_DIR}/_deps
# and wiping build/ forces a full re-clone on the next configure.
if(NOT DEFINED FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.fetchcontent-src" CACHE PATH
        "Shared FetchContent source trees (survives build/ clean; avoids repeat git clone)")
endif()

# Re-configure should not hit the network to "update" already-populated deps (first clone still uses git).
if(NOT DEFINED FETCHCONTENT_UPDATES_DISCONNECTED)
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
        "Skip FetchContent update step when content is already populated (faster IDE re-configure)")
endif()

include(FetchContent)
include(cmake/SmatchetThirdParty.cmake)
include(cmake/Sanitizers.cmake)

option(SMATCHET_FETCHCONTENT_VERBOSE "Log each third-party dependency during configure" OFF)
function(smatchet_fetch_status _msg)
    if(SMATCHET_FETCHCONTENT_VERBOSE)
        message(STATUS "${_msg}")
    endif()
endfunction()

if(NOT SMATCHET_FETCHCONTENT_VERBOSE)
    message(STATUS "FetchContent: sources under ${FETCHCONTENT_BASE_DIR} (SMATCHET_FETCHCONTENT_VERBOSE=ON for per-dependency log)")
endif()

smatchet_fetch_status("Fetching nlohmann/json...")
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

smatchet_fetch_status("Fetching cpr (C++ Requests)...")
smatchet_prepare_cpr()
FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git GIT_TAG 1.9.2)
FetchContent_MakeAvailable(cpr)
smatchet_patch_cpr_after_fetch("${cpr_SOURCE_DIR}")

smatchet_fetch_status("Fetching SQLiteCpp...")
smatchet_prepare_sqlitecpp()
FetchContent_Declare(sqlitecpp GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git GIT_TAG 3.3.1)
FetchContent_GetProperties(sqlitecpp)
if(NOT sqlitecpp_POPULATED)
    FetchContent_Populate(sqlitecpp)
endif()
smatchet_patch_sqlitecpp_after_populate("${sqlitecpp_SOURCE_DIR}")
add_subdirectory("${sqlitecpp_SOURCE_DIR}" "${sqlitecpp_BINARY_DIR}" EXCLUDE_FROM_ALL)

# SQLite's amalgamation (sqlite3.c) trips GCC's -Wstringop-overread on
# sqlite3ColumnSetColl → sqlite3Strlen30 → strlen(z) — GCC's value-range analysis decides z
# could be at address zero. This is a known SQLite/GCC interaction and a false positive
# (the call site has a null guard upstream). Vendored source — suppress on the sqlite3
# target only rather than patching the upstream amalgamation.
if(TARGET sqlite3 AND (CMAKE_C_COMPILER_ID STREQUAL "GNU"))
    target_compile_options(sqlite3 PRIVATE -Wno-stringop-overread)
endif()

smatchet_fetch_status("Fetching cpp-httplib...")
smatchet_prepare_httplib()
FetchContent_Declare(httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git GIT_TAG v0.14.1)
FetchContent_MakeAvailable(httplib)


# md4c is the Markdown parser backing the rich-text editing pipeline (md ↔ ADF/HTML for Jira/Plane
# description fields). We compile only `md4c.c` ourselves rather than using upstream's CMake project,
# which also builds md4c-html and the md2html CLI we don't need. See RICH_TEXT_EDITING_V2_PLAN.md.
smatchet_fetch_status("Fetching md4c (CommonMark/GFM parser)...")
FetchContent_Declare(md4c
    GIT_REPOSITORY https://github.com/mity/md4c.git
    GIT_TAG release-0.5.2
)
FetchContent_GetProperties(md4c)
if(NOT md4c_POPULATED)
    FetchContent_Populate(md4c)
endif()
if(NOT TARGET md4c)
    add_library(md4c STATIC "${md4c_SOURCE_DIR}/src/md4c.c")
    target_include_directories(md4c PUBLIC "${md4c_SOURCE_DIR}/src")
    set_target_properties(md4c PROPERTIES C_STANDARD 99)
    if(MSVC)
        target_compile_definitions(md4c PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()


smatchet_fetch_status("Fetching ghc::filesystem (C++14 Polyfill)...")
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

smatchet_fetch_status("Fetching GLFW...")
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.3.8)
FetchContent_MakeAvailable(glfw)

# ==============================================================================
# LUA & SOL2 (C++14 COMPATIBLE) — optional (SMATCHET_WITH_LUA_AUTOMATION)
# ==============================================================================
if(SMATCHET_WITH_LUA_AUTOMATION)
    set(LUA_SRC_DIR "${FETCHCONTENT_BASE_DIR}/lua-src")
    if(NOT EXISTS "${LUA_SRC_DIR}")
        smatchet_fetch_status("Downloading Lua 5.3.6 source...")
        file(MAKE_DIRECTORY "${FETCHCONTENT_BASE_DIR}")
        set(_lua_tar "${FETCHCONTENT_BASE_DIR}/lua-5.3.6.tar.gz")
        file(DOWNLOAD https://www.lua.org/ftp/lua-5.3.6.tar.gz "${_lua_tar}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${_lua_tar}" WORKING_DIRECTORY "${FETCHCONTENT_BASE_DIR}")
        file(RENAME "${FETCHCONTENT_BASE_DIR}/lua-5.3.6" "${LUA_SRC_DIR}")
        unset(_lua_tar)
    endif()

    file(GLOB LUA_SOURCES "${LUA_SRC_DIR}/src/*.c")
    list(REMOVE_ITEM LUA_SOURCES "${LUA_SRC_DIR}/src/lua.c" "${LUA_SRC_DIR}/src/luac.c")

    # Build Lua 5.3 as C++ so LUAI_THROW expands to `throw` instead of `longjmp`. With
    # longjmp, a luaL_error inside a sol2 protected_function skips C++ destructors between
    # the error site and the pcall setjmp frame — the recorder + sol2 marshalling layer
    # both own std::vector / std::string temporaries that would leak (or corrupt sol2
    # internals). With C++ exceptions, the host-side try / catch around protected_function
    # catches cleanly and destructors run. See docs/design/applied/lua-recorded-cmd-list.md
    # § Lua build mode.
    set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)

    # Lua 5.3's lua.h has no `extern "C"` guards. When compiled as C++, every API function
    # gets C++ name mangling — link errors against sol2 which includes the headers via
    # `lua.hpp` (which DOES wrap in `extern "C"`). Patch luaconf.h once at configure time
    # so LUA_API / LUALIB_API / LUAMOD_API resolve to `extern "C"` under __cplusplus.
    set(_luaconf_h "${LUA_SRC_DIR}/src/luaconf.h")
    if(EXISTS "${_luaconf_h}")
        file(READ "${_luaconf_h}" _luaconf_contents)
        string(FIND "${_luaconf_contents}" "SMATCHET_LUA_EXTERN_C_PATCH" _patched_idx)
        if(_patched_idx EQUAL -1)
            string(REPLACE "#define LUA_API\t\textern"
                "/* SMATCHET_LUA_EXTERN_C_PATCH: Lua 5.3 lacks extern \"C\" guards; force them when compiled as C++ so symbol names match the sol2 caller side that includes via lua.hpp. */\n#if defined(__cplusplus)\n#define LUA_API\t\textern \"C\"\n#else\n#define LUA_API\t\textern\n#endif"
                _luaconf_patched "${_luaconf_contents}")
            file(WRITE "${_luaconf_h}" "${_luaconf_patched}")
            message(STATUS "Patched luaconf.h with extern \"C\" guards for Lua-as-C++ build.")
        endif()
    endif()

    add_library(Smatchet_Lua_Internal STATIC ${LUA_SOURCES})

    # Belt-and-suspenders: fail the configure step if a future contributor strips LANGUAGE
    # CXX off a Lua source. Header-side __cplusplus checks can't see this target's TUs.
    get_target_property(_lua_sources Smatchet_Lua_Internal SOURCES)
    foreach(_src IN LISTS _lua_sources)
        get_source_file_property(_lang "${_src}" LANGUAGE)
        if(NOT _lang STREQUAL "CXX")
            message(FATAL_ERROR
                "Smatchet_Lua_Internal source ${_src} has LANGUAGE=${_lang}, expected CXX. "
                "luaL_error must use C++ exceptions for crash-safe unwinding through the "
                "recorder (see docs/design/applied/lua-recorded-cmd-list.md § Lua build mode).")
        endif()
    endforeach()

    target_include_directories(Smatchet_Lua_Internal PUBLIC "$<BUILD_INTERFACE:${LUA_SRC_DIR}/src>")

    set(LUA_INCLUDE_DIR "${LUA_SRC_DIR}/src" CACHE PATH "" FORCE)
    set(LUA_LIBRARIES Smatchet_Lua_Internal CACHE STRING "" FORCE)
    set(SOL2_BUILD_LUA OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(sol2 GIT_REPOSITORY https://github.com/ThePhD/sol2.git GIT_TAG v2.20.6)

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

    # sol2 2.20.6 keys userdata/metatable names from demangled type text only.
    # GCC/MinGW can collapse distinct internal callable/userdata types to the same text, then Lua reuses a
    # wrong __gc metatable and corrupts the heap during InitLua. Append a per-template-type token to the
    # internal metatable keys while keeping public usertype names unchanged.
    set(_sol2_single_header "${sol2_SOURCE_DIR}/single/sol/sol.hpp")
    if(EXISTS "${_sol2_single_header}")
        file(READ "${_sol2_single_header}" _sol2_single_contents)

        string(REPLACE "#include <cctype>\n#if defined(__GNUC__) && defined(__MINGW32__) && (__GNUC__ < 6)"
                       "#include <cctype>\n#include <cstdint>\n#if defined(__GNUC__) && defined(__MINGW32__) && (__GNUC__ < 6)"
                       _sol2_single_patched
                       "${_sol2_single_contents}")

        string(REPLACE "namespace sol {\n\n\ttemplate <typename T>\n\tstruct usertype_traits {"
                       "namespace sol {\n\n\tnamespace detail {\n\t\ttemplate <typename T>\n\t\tstruct smatchet_type_id_token {\n\t\t\tstatic char id;\n\t\t};\n\n\t\ttemplate <typename T>\n\t\tchar smatchet_type_id_token<T>::id;\n\n\t\ttemplate <typename T>\n\t\tstd::string smatchet_type_identity_suffix() {\n\t\t\treturn std::string(\"@\").append(std::to_string(reinterpret_cast<std::uintptr_t>(&smatchet_type_id_token<T>::id)));\n\t\t}\n\t} // namespace detail\n\n\ttemplate <typename T>\n\tstruct usertype_traits {"
                       _sol2_single_patched
                       "${_sol2_single_patched}")

        string(REPLACE "static const std::string m = std::string(\"sol.\").append(detail::demangle<T>());"
                       "static const std::string m = std::string(\"sol.\").append(detail::smatchet_type_identity_suffix<T>());"
                       _sol2_single_patched
                       "${_sol2_single_patched}")
        string(REPLACE "static const std::string u_m = std::string(\"sol.\").append(detail::demangle<T>()).append(\".user\");"
                       "static const std::string u_m = std::string(\"sol.user\").append(detail::smatchet_type_identity_suffix<T>());"
                       _sol2_single_patched
                       "${_sol2_single_patched}")
        string(REPLACE "static const std::string u_g_m = std::string(\"sol.\").append(detail::demangle<T>()).append(\".user\\xE2\\x99\\xBB\");"
                       "static const std::string u_g_m = std::string(\"sol.usergc\").append(detail::smatchet_type_identity_suffix<T>());"
                       _sol2_single_patched
                       "${_sol2_single_patched}")
        string(REPLACE "static const std::string g_t = std::string(\"sol.\").append(detail::demangle<T>()).append(\".\\xE2\\x99\\xBB\");"
                       "static const std::string g_t = std::string(\"sol.gc\").append(detail::smatchet_type_identity_suffix<T>());"
                       _sol2_single_patched
                       "${_sol2_single_patched}")

        # Also rewrite already-patched build trees when CMake is re-run without refetching sol2.
        string(REPLACE "static const std::string m = std::string(\"sol.\").append(detail::demangle<T>()).append(detail::smatchet_type_identity_suffix<T>());"
                       "static const std::string m = std::string(\"sol.\").append(detail::smatchet_type_identity_suffix<T>());"
                       _sol2_single_patched
                       "${_sol2_single_patched}")
        string(REPLACE "static const std::string u_m = std::string(\"sol.\").append(detail::demangle<T>()).append(\".user\").append(detail::smatchet_type_identity_suffix<T>());"
                       "static const std::string u_m = std::string(\"sol.user\").append(detail::smatchet_type_identity_suffix<T>());"
                       _sol2_single_patched
                       "${_sol2_single_patched}")
        string(REPLACE "static const std::string u_g_m = std::string(\"sol.\").append(detail::demangle<T>()).append(\".user\\xE2\\x99\\xBB\").append(detail::smatchet_type_identity_suffix<T>());"
                       "static const std::string u_g_m = std::string(\"sol.usergc\").append(detail::smatchet_type_identity_suffix<T>());"
                       _sol2_single_patched
                       "${_sol2_single_patched}")
        string(REPLACE "static const std::string g_t = std::string(\"sol.\").append(detail::demangle<T>()).append(\".\\xE2\\x99\\xBB\").append(detail::smatchet_type_identity_suffix<T>());"
                       "static const std::string g_t = std::string(\"sol.gc\").append(detail::smatchet_type_identity_suffix<T>());"
                       _sol2_single_patched
                       "${_sol2_single_patched}")

        if(NOT _sol2_single_patched STREQUAL _sol2_single_contents)
            file(WRITE "${_sol2_single_header}" "${_sol2_single_patched}")
        endif()
    endif()

    add_subdirectory("${sol2_SOURCE_DIR}" "${sol2_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif()

# whisper.cpp — optional local-inference backend for the Whisper dictation
# plugin. Pinned to v1.7.4 (confirmed via `git ls-remote --tags`). CPU-only
# build: examples / tests are turned off, no GGML_CUDA / GGML_VULKAN /
# GGML_METAL backends. Sub-option SMATCHET_WHISPER_LOCAL_BACKEND defaults
# OFF — see the option declaration above for the multi-PR rationale.
if(SMATCHET_WITH_WHISPER AND SMATCHET_WHISPER_LOCAL_BACKEND)
    smatchet_fetch_status("Fetching whisper.cpp v1.7.4...")
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
    # OpenMP off — MinGW UCRT64 ships without a libgomp the static link can
    # find, so the default GGML_OPENMP=ON build fails with
    # `undefined reference to GOMP_barrier`. ggml falls back to its own
    # threadpool when GGML_OPENMP=OFF; we lose the small OpenMP scheduling
    # win in exchange for a clean MinGW UCRT64 link. Re-evaluate if/when we
    # add a libgomp shipping story (or migrate to clang's libomp).
    set(GGML_OPENMP            OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(whisper_cpp
        GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
        GIT_TAG v1.7.4)
    FetchContent_MakeAvailable(whisper_cpp)
endif()

# Compile Dear ImGui (with GLFW and OpenGL3 backends)
smatchet_fetch_status("Fetching Dear ImGui...")
FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        # Pinned to a specific commit on the docking branch (reproducible builds / supply chain).
        # v1.92.7-docking-39-g329c5a6b3
        GIT_TAG 329c5a6b3be75ebf54506d3ae94b836ffcf19fa0
)

FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()
file(GLOB IMGUI_SOURCES "${imgui_SOURCE_DIR}/*.cpp")
list(APPEND IMGUI_SOURCES "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
add_library(ImGuiLib STATIC ${IMGUI_SOURCES})
target_include_directories(ImGuiLib PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${glfw_SOURCE_DIR}/include
        # Source_Core/include hosts SmatchetImConfig.h — the single source of
        # ImGui compile-time configuration (IMGUI_USE_WCHAR32, optional test
        # engine hookup). Adding it PUBLIC so every linking target's #include
        # "imgui.h" → IMGUI_USER_CONFIG path resolves cleanly without a
        # per-target include-dir tweak.
        ${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/include
)
# Route every TU that includes imgui.h through SmatchetImConfig.h. The file
# carries IMGUI_USE_WCHAR32 (supplementary-plane Unicode) plus the
# bucket-E test-engine hooks when SMATCHET_BUILD_UI_TESTS is ON. Consolidated
# from a previous PUBLIC IMGUI_USE_WCHAR32 define — having both mechanisms
# split is the easiest way to desync ImWchar across TUs.
target_compile_definitions(ImGuiLib PUBLIC IMGUI_USER_CONFIG="SmatchetImConfig.h")
if(SMATCHET_BUILD_UI_TESTS)
    target_compile_definitions(ImGuiLib PUBLIC SMATCHET_BUILD_UI_TESTS=1)
endif()

# Dear ImGui variant for Unreal/DX12 embedding (no GLFW/OpenGL dependency).
# EXCLUDE_FROM_ALL — only used by the (also-excluded) DX12 targets, so a
# default `cmake --build` for SmatchetStandalone shouldn't compile it.
if(WIN32)
    file(GLOB IMGUI_DX12_SOURCES "${imgui_SOURCE_DIR}/*.cpp")
    list(APPEND IMGUI_DX12_SOURCES "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp")
    add_library(ImGuiLib_DX12 STATIC EXCLUDE_FROM_ALL ${IMGUI_DX12_SOURCES})
    target_include_directories(ImGuiLib_DX12 PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
            ${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/include
    )
    target_compile_definitions(ImGuiLib_DX12 PUBLIC IMGUI_USER_CONFIG="SmatchetImConfig.h")
    if(SMATCHET_BUILD_UI_TESTS)
        target_compile_definitions(ImGuiLib_DX12 PUBLIC SMATCHET_BUILD_UI_TESTS=1)
    endif()
endif()

if(SMATCHET_BUILD_UI_TESTS)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ImGuiTestEngine.cmake)
endif()

# ==============================================================================
# 3. SMATCHET CORE LIBRARY (Engine-Agnostic)
# ==============================================================================
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source_Core/src/*.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetImGuiHost.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetImGuiHostC_Diagnostics.cpp")
# ImGuiColorTextEdit — vendored third-party TU promoted from Plugins/LuaConsole/ThirdParty
# so Source_Core (AI chat panel) can consume it. The glob above only matches
# Source_Core/src/*.cpp; the ThirdParty TU needs an explicit append. Both Standalone and
# DX12 targets inherit it via CORE_SOURCES.
set(_SMATCHET_IMGUI_TEXTEDIT_CPP
        "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp")
list(APPEND CORE_SOURCES "${_SMATCHET_IMGUI_TEXTEDIT_CPP}")
# Vendored third-party — suppress strict warnings so the smatchet_apply_strict_warnings
# flags on Core / Standalone / DX12 targets don't flag upstream TextEditor.cpp.
if(MSVC)
    set_source_files_properties("${_SMATCHET_IMGUI_TEXTEDIT_CPP}" PROPERTIES COMPILE_OPTIONS /W0)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties("${_SMATCHET_IMGUI_TEXTEDIT_CPP}" PROPERTIES COMPILE_OPTIONS -w)
endif()
# GLOB snapshots at configure time; pick up new TUs if the cache predates them (no duplicate paths).
set(_SMATCHET_CPP_SYNTAX "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CppSyntaxHighlight.cpp")
if(EXISTS "${_SMATCHET_CPP_SYNTAX}")
    list(FIND CORE_SOURCES "${_SMATCHET_CPP_SYNTAX}" _smatchet_cpp_syntax_idx)
    if(_smatchet_cpp_syntax_idx EQUAL -1)
        list(APPEND CORE_SOURCES "${_SMATCHET_CPP_SYNTAX}")
    endif()
endif()
unset(_smatchet_cpp_syntax_idx)
unset(_SMATCHET_CPP_SYNTAX)

list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AppController_LuaBindings.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AppController_LuaBindingsCore.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AppController_LuaStubs.cpp")
if(SMATCHET_WITH_LUA_AUTOMATION)
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AppController_LuaBindings.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AppController_LuaBindingsCore.cpp")
else()
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AppController_LuaStubs.cpp")
endif()

# Whisper dictation router — same bindings/stubs split shape as Lua. Always exactly
# one of the two TUs is compiled; both export identical symbols (enforced by the CI
# `build-windows-no-whisper` job — see .github/workflows/build-and-test.yml).
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Whisper.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Stubs.cpp")
# Phase C — setup banner is source-list-conditional (gating layer 2). The TU
# uses ImGui + ModelDownloader + ModelCatalog, all of which are inside the
# Plugins/Whisper subtree's `if(SMATCHET_WITH_WHISPER)` gate. CMake adds the
# TU only on the ON path; the OFF build never compiles it (call site in
# SmatchetUI.cpp is wrapped `#if defined(SMATCHET_WITH_WHISPER) ... #endif`).
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetWhisperSetupBanner.cpp")
# Phase E — amplitude-meter overlay TU, source-list-conditional with the same
# shape as the setup banner. Compiled only on the ON path; the OFF build
# never visits it (call site in SmatchetUI.cpp is wrapped
# `#if defined(SMATCHET_WITH_WHISPER) ... #endif`).
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetWhisperOverlayUi.cpp")
# Phase G — whisper-dictation-roundtrip scenario. Source-list conditional with
# the same shape as the overlay + banner above: the TU references
# WhisperPlugin::SetMockTranscription which lives behind the gate, so the OFF
# build must not compile it. The matching RegisterFactory call in
# AppController.cpp is also #ifdef'd on SMATCHET_WITH_WHISPER.
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/Commands/Scenarios/WhisperDictationScenario.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/Commands/Scenarios/WhisperAiAssistantAutosendScenario.cpp")
if(SMATCHET_WITH_WHISPER)
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Whisper.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetWhisperSetupBanner.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetWhisperOverlayUi.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/Commands/Scenarios/WhisperDictationScenario.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/Commands/Scenarios/WhisperAiAssistantAutosendScenario.cpp")
else()
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/DictationInsertionRouter_Stubs.cpp")
endif()

# Agentic flow — source-list-conditional. The OFF build never compiles these TUs and never sees
# the SMATCHET_WITH_AGENTIC compile definition (see PUBLIC define below). Every consumer must
# `#if defined(SMATCHET_WITH_AGENTIC)`-gate its include of GitHubClient.h / GitHubClientHelpers.h.
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/GitHubClient.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/GitHubClientHelpers.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticInferenceClient.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticInferenceClientPure.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgentProposalStore.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticTriageController.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetAgentProposalsUi.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetAgentProposalsUiPure.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetAgentHandoffUi.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticProposalAuditPure.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgentTriageScenarioFixtures.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/Commands/Scenarios/AgentTriageScenarioStep.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/Commands/Scenarios/AgentHandoffScenarioStep.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CodingHarnessSeedBuilder.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/ClaudeCodeLocalRunner.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticHandoffController.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/HarnessRunState.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/PrCommentWatcher.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/OpenPrRegistrar.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CoderabbitCommentClassifierPure.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CoderabbitCommentClassifier.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CiFailureClassifierPure.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CiFailureClassifier.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/PrCheckRunWatcher.cpp")
if(SMATCHET_WITH_AGENTIC)
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/GitHubClient.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/GitHubClientHelpers.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticInferenceClient.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticInferenceClientPure.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgentProposalStore.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticTriageController.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetAgentProposalsUi.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetAgentProposalsUiPure.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/SmatchetAgentHandoffUi.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticProposalAuditPure.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgentTriageScenarioFixtures.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/Commands/Scenarios/AgentTriageScenarioStep.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/Commands/Scenarios/AgentHandoffScenarioStep.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CodingHarnessSeedBuilder.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/ClaudeCodeLocalRunner.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AgenticHandoffController.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/HarnessRunState.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/PrCommentWatcher.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/OpenPrRegistrar.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CoderabbitCommentClassifierPure.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CoderabbitCommentClassifier.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CiFailureClassifierPure.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/CiFailureClassifier.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/PrCheckRunWatcher.cpp")
endif()

add_library(SmatchetCoreInterface INTERFACE)
target_include_directories(SmatchetCoreInterface INTERFACE
        Source_Core/include
        Source_Core/ThirdParty/ImGuiColorTextEdit
        ${sqlitecpp_SOURCE_DIR}/include
)
if(SMATCHET_WITH_LUA_AUTOMATION)
    target_include_directories(SmatchetCoreInterface INTERFACE
            ${LUA_SRC_DIR}/src
    )
    target_include_directories(SmatchetCoreInterface SYSTEM INTERFACE
            ${sol2_SOURCE_DIR}/single
    )
    if(MSVC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.29)
        target_compile_options(SmatchetCoreInterface INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:/external:W0>")
    endif()
endif()

target_link_libraries(SmatchetCoreInterface INTERFACE
        nlohmann_json::nlohmann_json
        SQLiteCpp
        cpr::cpr
        httplib::httplib
        md4c
)

if(SMATCHET_WITH_LUA_AUTOMATION)
    target_compile_definitions(SmatchetCoreInterface INTERFACE SMATCHET_WITH_LUA_AUTOMATION=1)
endif()
if(SMATCHET_WITH_WHISPER)
    target_compile_definitions(SmatchetCoreInterface INTERFACE SMATCHET_WITH_WHISPER=1)
endif()
if(SMATCHET_WITH_AGENTIC)
    # Mirrors the Whisper define shape — PUBLIC on SmatchetCoreInterface so consumers
    # (Source_Core sources, Plugins, future agentic shims) see the gate uniformly.
    target_compile_definitions(SmatchetCoreInterface INTERFACE SMATCHET_WITH_AGENTIC=1)
endif()
# MCP define is not on SmatchetCoreInterface: Unreal DX12 libs omit MCP without MSVC D9025 (/D vs /U clash).
# Targets that compile MCP-gated code link SmatchetCoreMcpShim (PUBLIC) instead.
if(SMATCHET_WITH_MCP)
    add_library(SmatchetCoreMcpShim INTERFACE)
    target_compile_definitions(SmatchetCoreMcpShim INTERFACE SMATCHET_WITH_MCP=1)
endif()
# AI assistant shim: keeps Unreal DX12 libs free of the define unless explicitly opted in.
# Phase A is no-op at the call sites (skeleton only), but the shim is created now so Phases B-E
# can compile-gate UI / Lua hooks without re-plumbing CMake.
if(SMATCHET_WITH_AI)
    add_library(SmatchetCoreAiShim INTERFACE)
    target_compile_definitions(SmatchetCoreAiShim INTERFACE SMATCHET_WITH_AI=1)
endif()

# Configure-time guard for the plugin shim-link discipline documented in
# docs/adr/0002-plugin-shim-link-discipline.md. Every standalone plugin lib that
# includes Source_Core headers must link the matching `Smatchet*Shim` libraries
# or `UiDrawSession` member offsets diverge at runtime (SIGSEGV inside
# std::string::clear() from a plugin TU is the canonical crash signature).
function(smatchet_assert_plugin_shim_links _target)
    if(NOT TARGET ${_target})
        return()
    endif()
    get_target_property(_libs ${_target} INTERFACE_LINK_LIBRARIES)
    if(NOT _libs)
        get_target_property(_libs ${_target} LINK_LIBRARIES)
    endif()
    if(SMATCHET_WITH_AI AND NOT "SmatchetCoreAiShim" IN_LIST _libs)
        message(FATAL_ERROR
            "Plugin '${_target}' includes Source_Core headers but does not link SmatchetCoreAiShim. "
            "Add `target_link_libraries(${_target} PUBLIC SmatchetCoreAiShim)` inside an "
            "`if(SMATCHET_WITH_AI)` guard. See docs/adr/0002-plugin-shim-link-discipline.md.")
    endif()
    if(SMATCHET_WITH_MCP AND NOT "SmatchetCoreMcpShim" IN_LIST _libs)
        message(FATAL_ERROR
            "Plugin '${_target}' includes Source_Core headers but does not link SmatchetCoreMcpShim. "
            "Add `target_link_libraries(${_target} PUBLIC SmatchetCoreMcpShim)` inside an "
            "`if(SMATCHET_WITH_MCP)` guard. See docs/adr/0002-plugin-shim-link-discipline.md.")
    endif()
endfunction()

function(smatchet_configure_core_impl_target _target)
    target_link_libraries("${_target}" PUBLIC SmatchetCoreInterface)
    target_include_directories("${_target}" PRIVATE Source_Core/src
            "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty")
    if(SMATCHET_WITH_MCP)
        target_include_directories("${_target}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Mcp")
    endif()
    if(SMATCHET_WITH_WHISPER)
        # SmatchetWhisperSetupBanner.cpp (compiled into CORE_SOURCES on the ON
        # path) includes ModelDownloader.h / ModelCatalog.h / WhisperConsentGate.h
        # which live in the Plugins/Whisper/ subtree.
        target_include_directories("${_target}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Whisper")
    endif()
    if(SMATCHET_WITH_LUA_AUTOMATION)
        target_include_directories("${_target}" PRIVATE
                ${LUA_SRC_DIR}/src
        )
        target_include_directories("${_target}" SYSTEM PRIVATE
                ${sol2_SOURCE_DIR}/single
        )
        target_link_libraries("${_target}" PRIVATE Smatchet_Lua_Internal ghc_filesystem)
    else()
        target_link_libraries("${_target}" PRIVATE ghc_filesystem)
    endif()
endfunction()

function(smatchet_configure_opengl_core_impl_target _target)
    smatchet_configure_core_impl_target("${_target}")
    target_link_libraries("${_target}" PRIVATE ImGuiLib glfw)
    if(SMATCHET_WITH_MCP)
        target_link_libraries("${_target}" PUBLIC SmatchetCoreMcpShim)
    endif()
    if(SMATCHET_WITH_AI)
        # Phase B: standalone target gets the side-panel + worker controller. DX12 (Unreal)
        # builds compile the same Source_Core/ TUs with SMATCHET_WITH_AI undefined so the
        # ai-assistant codepaths drop out via #if defined gates.
        target_link_libraries("${_target}" PUBLIC SmatchetCoreAiShim)
    endif()
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
    if(MSVC)
        # ConfigManager (and other TU) still use CRT APIs MSVC marks deprecated (e.g. getenv → C4996).
        target_compile_definitions("${_target}" PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

if(WIN32)
    # EXCLUDE_FROM_ALL: the DX12 lib is consumed by the Unreal plugin only.
    # Excluding it from the default ALL target halves incremental compile time
    # for SmatchetStandalone iteration (each Source_Core/src/*.cpp would otherwise
    # be built twice — once into libSmatchetCore_DX12.a, once into the exe).
    # The Unreal-packaging build presets explicitly target
    # `SmatchetPackageUnrealLibs_DX12` so they're unaffected.
    add_library(SmatchetCore_DX12 STATIC EXCLUDE_FROM_ALL ${CORE_SOURCES})
    smatchet_configure_dx12_core_impl_target(SmatchetCore_DX12)
    smatchet_apply_strict_warnings(SmatchetCore_DX12)
endif()

# ==============================================================================
# 3b. STATIC PLUGINS (same IPlugin contract as future DLLs)
# ==============================================================================
if(SMATCHET_WITH_MCP)
    add_library(SmatchetPlugin_Mcp STATIC Plugins/Mcp/McpPlugin.cpp Plugins/Mcp/McpJsonRpcPure.cpp)
    target_include_directories(SmatchetPlugin_Mcp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Mcp")
    target_link_libraries(SmatchetPlugin_Mcp PUBLIC SmatchetCoreInterface SmatchetCoreMcpShim)
    # See AI-shim note on SmatchetPlugin_LuaConsole — UiDrawSession layout depends on
    # SMATCHET_WITH_AI. Plugins that include Source_Core headers must match the Source_Core
    # define state or risk silent member-offset drift.
    if(SMATCHET_WITH_AI)
        target_link_libraries(SmatchetPlugin_Mcp PUBLIC SmatchetCoreAiShim)
    endif()
    smatchet_apply_strict_warnings(SmatchetPlugin_Mcp)
    smatchet_assert_plugin_shim_links(SmatchetPlugin_Mcp)
endif()

if(SMATCHET_WITH_LUA_AUTOMATION)
    # TextEditor source lives under Source_Core/ThirdParty/ImGuiColorTextEdit (promoted from
    # Plugins/LuaConsole/ThirdParty so Source_Core can consume it for the AI chat panel).
    # SmatchetCore now compiles + links the TU; LuaConsole inherits it via SmatchetCoreInterface.
    # LuaConsolePlugin.cpp instantiates sol2 templates and trips the same COFF
    # relocation overflow as AppController_LuaBindings.cpp under BFD ld. Skip
    # the flag when lld is the active linker — lld uses 64-bit relocations by
    # default and rejects -mcmodel=large objects (unsupported reloc type 0xe).
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT SMATCHET_LLD_PROGRAM)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/LuaConsole/LuaConsolePlugin.cpp"
            PROPERTIES COMPILE_OPTIONS "-mcmodel=large")
    endif()
    add_library(SmatchetPlugin_LuaConsole STATIC Plugins/LuaConsole/LuaConsolePlugin.cpp)
    target_include_directories(SmatchetPlugin_LuaConsole PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/LuaConsole")
    target_link_libraries(SmatchetPlugin_LuaConsole PUBLIC SmatchetCoreInterface ImGuiLib)
    if(SMATCHET_WITH_MCP)
        target_link_libraries(SmatchetPlugin_LuaConsole PUBLIC SmatchetCoreMcpShim)
    endif()
    # SmatchetUiSession.h has a ~200-byte `#if defined(SMATCHET_WITH_AI)` block of `assistant*`
    # members in the middle of `UiDrawSession`. Source_Core compiles with `SMATCHET_WITH_AI=1`
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

# Whisper dictation plugin — entire subtree is CMake-conditional (gating layer 6).
# DX12 / Unreal builds keep SMATCHET_WITH_WHISPER=OFF and never see this file.
if(SMATCHET_WITH_WHISPER)
    add_subdirectory(Plugins/Whisper)
endif()

if(WIN32)
    if(SMATCHET_WITH_MCP AND SMATCHET_WITH_MCP_UNREAL)
        # EXCLUDE_FROM_ALL — Unreal-only target. See SmatchetCore_DX12 above.
        add_library(SmatchetPlugin_Mcp_DX12 STATIC EXCLUDE_FROM_ALL Plugins/Mcp/McpPlugin.cpp Plugins/Mcp/McpJsonRpcPure.cpp)
        target_include_directories(SmatchetPlugin_Mcp_DX12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Mcp")
        target_link_libraries(SmatchetPlugin_Mcp_DX12 PUBLIC SmatchetCore_DX12 SmatchetCoreMcpShim)
        smatchet_apply_strict_warnings(SmatchetPlugin_Mcp_DX12)
    endif()
    if(SMATCHET_WITH_LUA_AUTOMATION)
        # TextEditor TU is built into SmatchetCore_DX12 directly (added to CORE_SOURCES below);
        # the plugin lib drops its own copy and consumes the symbol via SmatchetCore_DX12.
        # EXCLUDE_FROM_ALL — Unreal-only target. See SmatchetCore_DX12 above.
        add_library(SmatchetPlugin_LuaConsole_DX12 STATIC EXCLUDE_FROM_ALL Plugins/LuaConsole/LuaConsolePlugin.cpp)
        target_include_directories(SmatchetPlugin_LuaConsole_DX12 PUBLIC
                "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/LuaConsole")
        target_link_libraries(SmatchetPlugin_LuaConsole_DX12 PUBLIC SmatchetCore_DX12 ImGuiLib_DX12)
        if(MSVC)
            target_compile_definitions(SmatchetPlugin_LuaConsole_DX12 PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        smatchet_apply_strict_warnings(SmatchetPlugin_LuaConsole_DX12)
    endif()

    # EXCLUDE_FROM_ALL — Unreal-only target. See SmatchetCore_DX12 above.
    add_library(SmatchetImGuiHost_DX12 STATIC EXCLUDE_FROM_ALL
            Source_Core/src/SmatchetImGuiHost.cpp
            Source_Core/src/SmatchetImGuiHostC_Diagnostics.cpp
    )
    target_include_directories(SmatchetImGuiHost_DX12 PUBLIC Source_Core/include)
    if(SMATCHET_WITH_MCP AND SMATCHET_WITH_MCP_UNREAL)
        target_include_directories(SmatchetImGuiHost_DX12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/Mcp")
    endif()
    if(SMATCHET_WITH_LUA_AUTOMATION)
        target_include_directories(SmatchetImGuiHost_DX12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Plugins/LuaConsole")
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
                "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/include/SmatchetImGuiHost.h"
                "${_SMATCHET_UNREAL_INCLUDE_DIR}/SmatchetImGuiHost.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/include/SmatchetImGuiHostC.h"
                "${_SMATCHET_UNREAL_INCLUDE_DIR}/SmatchetImGuiHostC.h"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/include/SmatchetDefaults.h"
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
set_target_properties(SmatchetStandalone PROPERTIES OUTPUT_NAME "Smatchet")
if(WIN32)
    set_target_properties(SmatchetStandalone PROPERTIES WIN32_EXECUTABLE TRUE)
    # WIN32_EXECUTABLE -> /SUBSYSTEM:WINDOWS, whose default entry is WinMain. Standalone uses int main().
    if(MSVC)
        target_link_options(SmatchetStandalone PRIVATE "/ENTRY:mainCRTStartup")
    endif()
endif()
smatchet_configure_opengl_core_impl_target(SmatchetStandalone)
smatchet_apply_strict_warnings(SmatchetStandalone)

if(SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME)
    target_compile_definitions(SmatchetStandalone PRIVATE SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME=1)
endif()

if(SMATCHET_WITH_LUA_AUTOMATION)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Scripts")
        set(SMATCHET_LUA_SCRIPTS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Scripts")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
        set(SMATCHET_LUA_SCRIPTS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/scripts")
    else()
        set(SMATCHET_LUA_SCRIPTS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Scripts")
    endif()

    # Single-config generators (Ninja, Make): create a junction (Windows) or
    # symlink (POSIX) at configure-time so edits to Scripts/ propagate live
    # without re-running the build. Multi-config generators (Visual Studio,
    # Xcode) keep the per-build copy because the runtime dir is per-config and
    # not knowable at configure time. Falls back to copy if linking fails
    # (FAT filesystem, lacking privileges, etc.).
    set(_SMATCHET_SCRIPTS_DST "${CMAKE_BINARY_DIR}/Scripts")
    set(_smatchet_scripts_linked FALSE)
    if(NOT CMAKE_CONFIGURATION_TYPES AND EXISTS "${SMATCHET_LUA_SCRIPTS_SRC}")
        if(NOT EXISTS "${_SMATCHET_SCRIPTS_DST}")
            if(WIN32)
                # mklink /J — directory junction; does NOT require admin rights.
                # mklink is a cmd built-in (not a separate exe), and it expects
                # native Windows backslashes; CMake paths are forward-slash by
                # default so we convert before invoking.
                file(TO_NATIVE_PATH "${_SMATCHET_SCRIPTS_DST}" _smatchet_dst_native)
                file(TO_NATIVE_PATH "${SMATCHET_LUA_SCRIPTS_SRC}" _smatchet_src_native)
                execute_process(
                    COMMAND cmd /c mklink /J "${_smatchet_dst_native}" "${_smatchet_src_native}"
                    RESULT_VARIABLE _smatchet_mklink_rc
                    OUTPUT_QUIET ERROR_QUIET)
                if(_smatchet_mklink_rc EQUAL 0)
                    set(_smatchet_scripts_linked TRUE)
                endif()
            else()
                execute_process(
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                            "${SMATCHET_LUA_SCRIPTS_SRC}" "${_SMATCHET_SCRIPTS_DST}"
                    RESULT_VARIABLE _smatchet_mklink_rc
                    OUTPUT_QUIET ERROR_QUIET)
                if(_smatchet_mklink_rc EQUAL 0)
                    set(_smatchet_scripts_linked TRUE)
                endif()
            endif()
            if(_smatchet_scripts_linked)
                message(STATUS "Scripts: linked ${_SMATCHET_SCRIPTS_DST} -> ${SMATCHET_LUA_SCRIPTS_SRC}")
            else()
                message(STATUS "Scripts: link failed (rc=${_smatchet_mklink_rc}); falling back to per-build copy")
            endif()
        else()
            # Already there from a prior configure (junction or copy). Trust it;
            # users can rm -rf the build dir to re-link.
            set(_smatchet_scripts_linked TRUE)
        endif()
    endif()
    if(NOT _smatchet_scripts_linked)
        add_custom_command(TARGET SmatchetStandalone POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${SMATCHET_LUA_SCRIPTS_SRC}"
                        "$<TARGET_FILE_DIR:SmatchetStandalone>/Scripts"
                COMMENT "Copy Scripts (Lua + bundled art) next to SmatchetStandalone"
                VERBATIM)
    endif()
endif()

# Fix MinGW/GCC "relocation truncated to fit" when linking sol2/CachedTicket.
# -mcmodel=large makes GCC emit 64-bit absolute (R_X86_64_32S) relocations so
# BFD ld can reach symbols past the 2 GB boundary that short-range relocations
# cannot. lld already uses 64-bit relocations for all COFF/PE targets by
# default and does NOT support R_X86_64_32S (type 0xe) — using -mcmodel=large
# with lld causes "unsupported relocation type 0xe" link errors. So we apply
# the flag ONLY when BFD ld is the active linker (SMATCHET_LLD_PROGRAM unset).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(SMATCHET_WITH_LUA_AUTOMATION AND NOT SMATCHET_LLD_PROGRAM)
        # COMPILE_OPTIONS adds the flag; SKIP_PRECOMPILE_HEADERS opts the TU
        # out of the Standalone PCH because PCHs are model-locked — a PCH
        # built with -mcmodel=small cannot be consumed by a -mcmodel=large TU
        # (and vice-versa). Without the skip, GCC emits a noisy
        # "[-Winvalid-pch]" warning and silently falls back to non-PCH for
        # this TU. The skip makes the fallback explicit and quiets the build.
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AppController_LuaBindings.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/src/AppController_LuaBindingsCore.cpp"
            PROPERTIES
                COMPILE_OPTIONS "-mcmodel=large"
                SKIP_PRECOMPILE_HEADERS ON)
        target_link_options(SmatchetStandalone PRIVATE -mcmodel=large)
    endif()
    # COFF debug-section relocation overflow (more than 64K relocations in one
    # section) can hit MinGW Debug builds when sol2 expands a lot of templates
    # in a single TU. Two ways to lift the limit without losing debug info:
    #   * `-Wa,-mbig-obj` — switch the assembler to the big-obj format (linker-
    #     side limit, no source change). Cheap and Just Works on any GCC ≥ 5.
    #   * `-gsplit-dwarf` — split debug info into a sidecar `.dwo` file.
    # Use `-Wa,-mbig-obj` as the default; falls through to `-g0` only on the
    # off chance the assembler doesn't support it. RelWithDebInfo and Debug
    # both keep their `-g`, so the iteration presets remain debuggable.
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-Wa,-mbig-obj" SMATCHET_HAS_MBIG_OBJ)
    if(SMATCHET_HAS_MBIG_OBJ)
        target_compile_options(SmatchetStandalone PRIVATE -Wa,-mbig-obj)
    else()
        # Last-ditch: drop debug info to avoid the overflow. Only Debug is
        # affected; RelWithDebInfo still uses its build-type defaults because
        # it lives in the build-type flags, not target_compile_options.
        target_compile_options(SmatchetStandalone PRIVATE "$<$<CONFIG:Debug>:-g0>")
    endif()
endif()

if(SMATCHET_WITH_MCP)
    target_link_libraries(SmatchetStandalone PRIVATE SmatchetPlugin_Mcp)
endif()
if(SMATCHET_WITH_LUA_AUTOMATION)
    target_link_libraries(SmatchetStandalone PRIVATE SmatchetPlugin_LuaConsole)
endif()
if(SMATCHET_WITH_WHISPER)
    target_link_libraries(SmatchetStandalone PRIVATE SmatchetPlugin_Whisper)
endif()

if(WIN32)
    target_link_libraries(SmatchetStandalone PRIVATE opengl32 ws2_32 crypt32)
    # MinGW release exe otherwise depends on libgcc/libstdc++ DLLs; wrong DLL on PATH → 0xC0000139
    # (entry point not found) before main — no logs, "release doesn't run".
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(SMATCHET_WINDOWS_FULLY_STATIC_RUNTIME)
            target_link_options(SmatchetStandalone PRIVATE -static -static-libgcc -static-libstdc++)
        else()
            target_link_options(SmatchetStandalone PRIVATE -static-libgcc -static-libstdc++)
            # Still links a handful of MinGW/MSYS2 runtime DLLs; copy them next to
            # the exe so command-prompt launches and debugger runs do not depend on
            # PATH pointing at the same toolchain build.
            get_filename_component(_SMATCHET_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
            set(_SMATCHET_STANDALONE_RUNTIME_DLLS
                "${_SMATCHET_MINGW_BIN}/libwinpthread-1.dll")
            foreach(_smatchet_runtime_dll IN LISTS _SMATCHET_STANDALONE_RUNTIME_DLLS)
                if(EXISTS "${_smatchet_runtime_dll}")
                    get_filename_component(_smatchet_runtime_dll_name "${_smatchet_runtime_dll}" NAME)
                    # Configure-time copy covers the common "configure, then run"
                    # flow even before the target is rebuilt.
                    file(COPY "${_smatchet_runtime_dll}" DESTINATION "${CMAKE_BINARY_DIR}/")
                    # Post-build copy keeps the runtime DLL set fresh whenever the
                    # standalone target is rebuilt.
                    add_custom_command(
                        TARGET SmatchetStandalone
                        POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                "${_smatchet_runtime_dll}"
                                "$<TARGET_FILE_DIR:SmatchetStandalone>/"
                        COMMENT "Copy ${_smatchet_runtime_dll_name} next to SmatchetStandalone"
                        VERBATIM)
                endif()
            endforeach()
        endif()
    endif()
elseif(APPLE)
    find_library(COCOA_LIBRARY Cocoa)
    find_library(OPENGL_LIBRARY OpenGL)
    target_link_libraries(SmatchetStandalone PRIVATE ${COCOA_LIBRARY} ${OPENGL_LIBRARY})
endif()

# LTO/IPO for distribution builds.
# Only the publish preset sets SMATCHET_ENABLE_LTO=ON; iteration presets leave
# it OFF because LTO forces a full relink of every TU on every build.
#
# Compatibility matrix:
#   GCC  + BFD ld  → full LTO via liblto_plugin. Supported. Slow link, best code.
#   GCC  + lld     → NOT supported. GCC emits GIMPLE IR; lld reads LLVM IR.
#                    We gate explicitly on NOT SMATCHET_LLD_PROGRAM so the
#                    message is clear rather than a cryptic link error.
#   Clang + lld    → ThinLTO (-flto=thin). Fast parallel link, near-full quality.
#                    Not yet wired (no clang preset); add when needed.
if(SMATCHET_ENABLE_LTO)
    if(SMATCHET_LLD_PROGRAM AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(STATUS "LTO: disabled — GCC + lld are incompatible (GCC emits GIMPLE IR, lld reads LLVM IR). Build with BFD ld or switch to Clang + lld.")
    else()
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _smatchet_ipo_ok OUTPUT _smatchet_ipo_err)
        if(_smatchet_ipo_ok)
            if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
                # Use parallel LTRANS jobs so `lto-wrapper` doesn't serialize
                # the final link across translation units.
                add_compile_options(-flto=auto)
                add_link_options(-flto=auto)
            endif()
            message(STATUS "LTO: enabled (IPO supported by ${CMAKE_CXX_COMPILER_ID} + active linker)")
            set(SMATCHET_IPO_ENABLED TRUE)
        else()
            message(STATUS "LTO: disabled (not supported: ${_smatchet_ipo_err})")
        endif()
    endif()
endif()

# Precompiled header — see SMATCHET_USE_PCH option above. Applied only to the
# Standalone target so the DX12 lib (different ImGui backend, different defs)
# stays unaffected. Header lives at Source_Core/include/SmatchetPch.h and
# pulls in the four heaviest things in the compile graph (imgui.h, json,
# windows.h, common STL containers).
if(SMATCHET_USE_PCH)
    target_precompile_headers(SmatchetStandalone PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Source_Core/include/SmatchetPch.h>")
endif()

if(SMATCHET_IPO_ENABLED)
    set_target_properties(SmatchetStandalone PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    if(TARGET SmatchetCore_DX12)
        set_target_properties(SmatchetCore_DX12 PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    if(TARGET SmatchetImGuiHost_DX12)
        set_target_properties(SmatchetImGuiHost_DX12 PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# Strip the symbol table from Release builds. No debug info is emitted under
# Release anyway (CMAKE_<LANG>_FLAGS_RELEASE = "-O3 -DNDEBUG"), so this just
# removes the linker's symbol table — a few MB of disk space saved with no
# information loss for end users. Skipped under MSVC (link.exe doesn't accept
# `-Wl,-s`; the equivalent there is `/DEBUG:NONE` which is already the default
# for Release).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(SmatchetStandalone PRIVATE
        "$<$<CONFIG:Release>:-Wl,-s>")
endif()

# Cap concurrent links so simultaneous static-link of large targets doesn't
# thrash RAM on machines with limited memory. Compiles still run at full -j.
if(CMAKE_GENERATOR MATCHES "Ninja")
    set_property(TARGET SmatchetStandalone PROPERTY JOB_POOL_LINK smatchet_link_pool)
    if(TARGET SmatchetCore_DX12)
        set_property(TARGET SmatchetCore_DX12 PROPERTY JOB_POOL_LINK smatchet_link_pool)
    endif()
    if(TARGET SmatchetImGuiHost_DX12)
        set_property(TARGET SmatchetImGuiHost_DX12 PROPERTY JOB_POOL_LINK smatchet_link_pool)
    endif()
endif()

# Sanitizer hookup. Applies SMATCHET_SANITIZER (asan / tsan / msan) flags to
# the main app targets when a sanitizer preset is active. PRIVATE keeps the
# flags off vendored FetchContent deps. No-op when SMATCHET_SANITIZER is
# unset / "off". See cmake/Sanitizers.cmake + AGENTS.md § Debug techniques.
smatchet_verify_mingw_gcc_sanitizer_runtimes()
smatchet_apply_sanitizers(SmatchetStandalone)
if(TARGET SmatchetCore_DX12)
    smatchet_apply_sanitizers(SmatchetCore_DX12)
endif()

# ImGui Test Engine (bucket E) — linked into SmatchetStandalone (live test
# runner) and the DX12 stub target (compile tripwire, no runtime invocation).
# Production builds (gate OFF) skip this block entirely.
if(SMATCHET_BUILD_UI_TESTS)
    target_compile_definitions(SmatchetStandalone PRIVATE SMATCHET_BUILD_UI_TESTS=1)
    target_link_libraries(SmatchetStandalone PRIVATE SmatchetImGuiTestEngine)
    if(TARGET SmatchetCore_DX12 AND TARGET SmatchetImGuiTestEngine_DX12)
        target_compile_definitions(SmatchetCore_DX12 PRIVATE SMATCHET_BUILD_UI_TESTS=1)
        target_link_libraries(SmatchetCore_DX12 PRIVATE SmatchetImGuiTestEngine_DX12)
    endif()
    enable_testing()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/CMakeLists.txt")
        add_subdirectory(tests/ui)
    endif()
endif()

# Pure-logic doctest rig for Source_Core helpers. Opt-in via SMATCHET_BUILD_TESTS.
# Standalone + DX12 default builds never link test code. See tests/CMakeLists.txt
# and agents/test-rig.md.
if(SMATCHET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Lua bindings rig — separate binary because Smatchet_Lua_Internal + sol2 are
# heavy templates we don't want linked into every pure-logic test run. See
# tests/Lua/CMakeLists.txt and docs/design/test-suite-expansion-completion.md
# § Phase 6.
if(SMATCHET_BUILD_LUA_TESTS)
    if(NOT SMATCHET_WITH_LUA_AUTOMATION)
        message(FATAL_ERROR "SMATCHET_BUILD_LUA_TESTS=ON requires SMATCHET_WITH_LUA_AUTOMATION=ON.")
    endif()
    enable_testing()
    add_subdirectory(tests/Lua)
endif()
