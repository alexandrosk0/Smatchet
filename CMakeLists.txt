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
# MSVC base compile flags — bake them so a fresh CMake-4 configure can't drop them
#
# Background (docs/agent-rules/build.md § Fresh-worktree configure pitfalls #1/#2):
# CMake's MSVC defaults seed CMAKE_CXX_FLAGS with "/DWIN32 /D_WINDOWS /EHsc" and
# CMAKE_C_FLAGS with "/DWIN32 /D_WINDOWS". On a FRESH configure under CMake 4.x
# those defaults were observed to be dropped, so the whole tree built with
# exceptions OFF — doctest TUs failed `C2338: Exceptions are disabled!` and
# product TUs failed `C4530` → (under /WX) `C2220`. Long-lived build dirs only
# survived because their caches predated the CMake upgrade.
#
# Fix: ensure /EHsc + the WIN32 macros are present in the flag vars regardless of
# what the generator's defaults did. We APPEND-IF-MISSING (never clobber): if
# CMake already seeded them, this is a no-op; if a fresh CMake-4 configure dropped
# them — or a poisoned cache (#2) left CMAKE_CXX_FLAGS empty — we restore them.
# This is the single authoritative home; the per-pitfall `-DCMAKE_CXX_FLAGS=...`
# workaround in build.md is no longer required for these presets.
#
# CACHE FORCE so the value persists into CMakeCache.txt and a later re-configure
# (or a downstream consumer reading the cache) sees the corrected flags — this is
# also what neutralises the poisoned-empty-flags cache class: a build dir whose
# cached CMAKE_CXX_FLAGS is empty (a silently-failed first configure, #2) gets
# /EHsc re-injected on the next configure instead of perpetually building with
# exceptions off.
if(MSVC)
    # Detect + warn on the poisoned-empty-flags cache before we repair it, so the
    # symptom (a "configured fine" dir that builds with exceptions off) is named.
    if(DEFINED CMAKE_CXX_FLAGS AND CMAKE_CXX_FLAGS STREQUAL "" AND
       NOT DEFINED ENV{CI})
        message(STATUS
            "Smatchet: cached CMAKE_CXX_FLAGS is empty (a silently-failed first "
            "configure can poison it; build.md pitfall #2) — re-injecting the MSVC "
            "base flags (/EHsc /DWIN32 /D_WINDOWS).")
    endif()
    foreach(_smatchet_msvc_flag "/DWIN32" "/D_WINDOWS" "/EHsc")
        if(NOT CMAKE_CXX_FLAGS MATCHES "(^| )${_smatchet_msvc_flag}( |$)")
            string(STRIP "${CMAKE_CXX_FLAGS} ${_smatchet_msvc_flag}" CMAKE_CXX_FLAGS)
        endif()
    endforeach()
    # C has no /EHsc (C++-exceptions only); just the WIN32 platform macros.
    foreach(_smatchet_msvc_cflag "/DWIN32" "/D_WINDOWS")
        if(NOT CMAKE_C_FLAGS MATCHES "(^| )${_smatchet_msvc_cflag}( |$)")
            string(STRIP "${CMAKE_C_FLAGS} ${_smatchet_msvc_cflag}" CMAKE_C_FLAGS)
        endif()
    endforeach()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" CACHE STRING "Flags used by the CXX compiler during all build types." FORCE)
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}"   CACHE STRING "Flags used by the C compiler during all build types."   FORCE)
    unset(_smatchet_msvc_flag)
    unset(_smatchet_msvc_cflag)
endif()

# ==============================================================================
# MSVC toolset-consistency guard (configure-time, early — runs right after the
# compiler is known so it fires before any heavy configure work or compile).
#
# The repo pins the MSVC toolset (build.msvc_toolset_pin in project.config.json)
# and both build wrappers (scripts/dev/with-msvc-env.sh / with-msvc.ps1) pass it
# to vcvars64 as -vcvars_ver=<pin>, so a clean configure THROUGH a wrapper picks
# a cl.exe whose minor version matches the STL headers it sees → consistent.
#
# The failure this guards: if a build dir is configured WITHOUT the wrapper (or
# with `-latest`, or a leaked env where a newer side-by-side toolset's cl.exe is
# on PATH), CMake bakes that newer cl into CMakeCache.txt's CMAKE_CXX_COMPILER.
# Thereafter, even building THROUGH the wrapper (which sets INCLUDE to the pinned
# toolset's headers) compiles the CACHED newer cl against the OLDER STL headers
# → thousands of cryptic errors deep in <chrono>/<xtimec.h> (e.g. "'__int64'
# should be preceded by ';'", "'_LIKELY' undeclared", STL1001), on files
# unrelated to whatever is being changed. This converts that 5000-error cascade
# into one obvious line, at configure time, before any compile.
#
# Mapping: MSVC compiler-version 19.NN ↔ VC toolset 14.NN (compiler-minor ==
# toolset-minor). We compare the MINOR component — that's what actually diverges
# and breaks (.38 STL headers vs a .50 cl). MSVC-only; never fires for
# Clang/GCC/AppleClang (Linux/clang presets, Unreal, clang-cl, etc.).
#
# Fail-open on "can't determine the pin" (missing/unreadable key → skip the
# guard, don't break config-less or non-MSVC builds); fail-closed on "pin known
# AND minor mismatch". Override: -DSMATCHET_ALLOW_TOOLSET_MISMATCH=ON downgrades
# the FATAL to a WARNING for someone who deliberately wants a different toolset.
# ==============================================================================
option(SMATCHET_ALLOW_TOOLSET_MISMATCH
    "Downgrade the MSVC toolset-consistency guard from FATAL_ERROR to WARNING (deliberate off-pin toolset)"
    OFF)

# LOCAL-ONLY: the pin (e.g. 14.38) is a LOCAL-dev convention matching the cached
# build dirs the wrappers configure. CI runners use their own (consistent,
# possibly newer) toolset and configure FRESH every run, so they can never hit
# the stale-cache cl-vs-headers mismatch this guard targets — and enforcing the
# local pin on CI would falsely FATAL every runner whose toolset != the pin.
# `CI` is set by GitHub Actions (and virtually every CI system), so skip there.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND NOT DEFINED ENV{CI})
    # Read the pinned toolset minor straight out of project.config.json (single
    # source of truth — same scalar the bash/PowerShell wrappers grep). Matches
    # the wrapper's regex style: "msvc_toolset_pin": "<major>.<minor>".
    set(_smatchet_pin_minor "")
    set(_smatchet_pin_full "")
    set(_smatchet_cfg "${CMAKE_CURRENT_SOURCE_DIR}/project.config.json")
    if(EXISTS "${_smatchet_cfg}")
        file(READ "${_smatchet_cfg}" _smatchet_cfg_contents)
        # Capture the pinned "<major>.<minor>" value. CMake regex has no \d /
        # non-greedy; [0-9]+ on each component is sufficient and robust.
        if(_smatchet_cfg_contents MATCHES
                "\"msvc_toolset_pin\"[ \t]*:[ \t]*\"([0-9]+)\\.([0-9]+)\"")
            set(_smatchet_pin_full "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
            set(_smatchet_pin_minor "${CMAKE_MATCH_2}")
        endif()
    endif()

    # Detected compiler minor: CMAKE_CXX_COMPILER_VERSION is like "19.38.33135".
    # Take the SECOND dotted component (the minor) — that maps 1:1 to the toolset
    # minor (19.38 cl ↔ 14.38 toolset). Robust to a 2- or 3-component version.
    set(_smatchet_cc_minor "")
    if(CMAKE_CXX_COMPILER_VERSION MATCHES "^[0-9]+\\.([0-9]+)")
        set(_smatchet_cc_minor "${CMAKE_MATCH_1}")
    endif()

    # Fail-open: only enforce when BOTH the pin minor and the detected minor are
    # known. A missing pin (config-less consumer / key removed) or an
    # unparseable compiler version skips the guard rather than breaking configure.
    if(_smatchet_pin_minor STREQUAL "" OR _smatchet_cc_minor STREQUAL "")
        # Skip silently — can't determine consistency, so don't block.
    elseif(NOT _smatchet_cc_minor STREQUAL _smatchet_pin_minor)
        set(_smatchet_toolset_msg
            "MSVC toolset mismatch in this build directory.\n"
            "  Detected compiler: ${CMAKE_CXX_COMPILER_VERSION} (VC toolset 14.${_smatchet_cc_minor})\n"
            "  Expected pin:      ${_smatchet_pin_full} (build.msvc_toolset_pin in project.config.json)\n"
            "  Compiler path:     ${CMAKE_CXX_COMPILER}\n"
            "\n"
            "This build dir was configured with the wrong MSVC toolset. Building it now\n"
            "(even through a wrapper that pins INCLUDE to 14.${_smatchet_pin_minor}) compiles the cached\n"
            "14.${_smatchet_cc_minor} cl.exe against 14.${_smatchet_pin_minor} STL headers -> thousands of cryptic errors\n"
            "deep in <chrono>/<xtimec.h> (STL1001, \"'__int64' should be preceded by ';'\").\n"
            "\n"
            "Fix: wipe this build dir and reconfigure THROUGH the wrapper, which pins\n"
            "-vcvars_ver from project.config.json:\n"
            "  rm -rf \"${CMAKE_BINARY_DIR}\"\n"
            "  scripts/dev/with-msvc-env.sh cmake --preset <preset>\n"
            "(PowerShell: scripts/dev/with-msvc.ps1 cmake --preset <preset>)\n"
            "\n"
            "Or, if you deliberately want toolset 14.${_smatchet_cc_minor}: set build.msvc_toolset_pin to\n"
            "match, or pass -DSMATCHET_ALLOW_TOOLSET_MISMATCH=ON to downgrade this to a warning.")
        if(SMATCHET_ALLOW_TOOLSET_MISMATCH)
            message(WARNING ${_smatchet_toolset_msg})
        else()
            message(FATAL_ERROR ${_smatchet_toolset_msg})
        endif()
    else()
        message(STATUS "MSVC toolset 14.${_smatchet_cc_minor} matches pin ${_smatchet_pin_full} (project.config.json)")
    endif()

    unset(_smatchet_pin_minor)
    unset(_smatchet_pin_full)
    unset(_smatchet_cc_minor)
    unset(_smatchet_cfg)
    unset(_smatchet_cfg_contents)
    unset(_smatchet_toolset_msg)
endif()

# ==============================================================================
# Iteration-speed knobs — all auto-detected and gracefully degrade. A fresh
# clone with `cmake --preset ninja-publish-msvc && cmake --build --preset ninja-publish-msvc`
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

# CI-only: embed debug info in objects (/Z7) instead of a separate PDB (/Zi).
# sccache CANNOT cache /Zi compilations (the PDB write is a side effect it can't
# reproduce from cache), so on CI — where sccache is the whole point — we force
# /Z7. Gated on $ENV{CI} (set automatically by GitHub Actions) and read at
# CONFIGURE time: a generator expression cannot read env at build time, so an
# `if()` is required. Deliberately NOT gated on compile-cache-present — harmless
# if sccache is absent (just larger .obj). Local dev keeps the default /Zi
# separate PDB. We sidestep CMAKE_MSVC_DEBUG_INFORMATION_FORMAT entirely so the
# repo's `cmake_minimum_required(VERSION 3.24)` (policy CMP0141 is 3.25+, default
# OLD here — the variable would be silently ignored) needs no bump.
#
# REPLACE /Zi in the per-config flag vars rather than add_compile_options(/Z7):
# appending /Z7 alongside CMake's built-in /Zi makes cl emit D9025 ("overriding
# '/Zi' with '/Z7'"), which scripts/dev/test-build-warnings.sh would fail on.
# Single-config Ninja only consumes the active config's vars, but we rewrite both
# RelWithDebInfo (iter/test/ui-test presets) and Debug (asan preset) for safety.
# See docs/plans/ci-build-time-reduction.md § Q1.
if(DEFINED ENV{CI} AND MSVC)
    foreach(_z7_cfg RELWITHDEBINFO DEBUG)
        foreach(_z7_lang C CXX)
            if(CMAKE_${_z7_lang}_FLAGS_${_z7_cfg} MATCHES "/Zi")
                string(REPLACE "/Zi" "/Z7"
                       CMAKE_${_z7_lang}_FLAGS_${_z7_cfg}
                       "${CMAKE_${_z7_lang}_FLAGS_${_z7_cfg}}")
            else()
                string(APPEND CMAKE_${_z7_lang}_FLAGS_${_z7_cfg} " /Z7")
            endif()
        endforeach()
    endforeach()
    message(STATUS "CI + MSVC: forcing /Z7 (embedded debug info) for sccache compatibility")
endif()

# lld — much faster than BFD ld for static-link-heavy targets, multi-threaded
# by default. Detected by looking for `ld.lld` in the same directory as the
# active compiler — i.e. a MinGW toolchain that bundles lld. We don't try
# to use a standalone LLVM `ld.lld` together with a separately-installed
# MinGW GCC: that combo has a known interaction where collect2 invokes lld
# without forwarding MinGW's MSVC-import-lib search semantics, and link
# fails with `unable to find library -lgdi32` even when the lib clearly
# exists. The fix on Windows is to install a MinGW that bundles lld:
#   * MSYS2 UCRT64 (legacy): pacman -S mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-lld
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
# iteration. Default OFF; publish presets turn it on.
option(SMATCHET_ENABLE_LTO
    "Enable link-time optimisation (LTO/IPO) on eligible targets"
    OFF)
option(SMATCHET_WINDOWS_FULLY_STATIC_RUNTIME
    "Link standalone runtime fully static for one-file Windows publish artifacts (MinGW: -static; MSVC/clang-cl: /MT)"
    OFF)

if(SMATCHET_WINDOWS_FULLY_STATIC_RUNTIME AND WIN32 AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING
        "Static CRT for MSVC/clang-cl publish builds" FORCE)
endif()

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
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet"
    CACHE PATH "Output directory for Unreal ThirdParty Smatchet artifacts")

# Stronger warnings on first-party targets only (FetchContent deps keep their own flags).
option(SMATCHET_ENABLE_STRICT_WARNINGS
    "Apply /W4 or -Wall -Wextra -Wunused-* to Smatchet libraries and standalone (not ImGui/GLFW)"
    ON)
# Default OFF for external/raw-cmake/Unreal consumers; force-ON via the
# _smatchet-msvc-base / _smatchet-clang-base presets so every first-party dev +
# CI build (and the light job) treats first-party warnings as errors.
option(SMATCHET_WARNINGS_AS_ERRORS
    "Treat the above warnings as errors (force-ON via base presets; default OFF for external/Unreal consumers)"
    OFF)

option(SMATCHET_WITH_LUA_AUTOMATION "Build with Lua/sol2 field automation and Lua console plugin" ON)
option(SMATCHET_WITH_MCP "Build with MCP plugin (Model Context Protocol server)" ON)
option(SMATCHET_WITH_AI "Build with AI assistant side panel (provider-pluggable IAiClient)" ON)
option(SMATCHET_WITH_WHISPER "Build with Whisper dictation plugin (push-to-talk; Phase A shell)" ON)
# Sub-option — fetch whisper.cpp v1.7.4 and link it into SmatchetPlugin_Whisper.
# Default ON: the locked dictation UX is "local default, cloud fallback" and the
# measured static-link delta is only ~1.37 MB (Smatchet.exe 16.78 MB OFF -> 18.15 MB
# ON; whisper.cpp + ggml link fully static, no extra DLLs), far under the 50 MB
# threshold from the plan's open question #1 — so default ON makes the shipped
# build match the locked UX with negligible footprint. Flip OFF to skip the
# fetch/link (WhisperLocal API still compiles; Transcribe() returns "local backend
# not built" and the mode router falls back to cloud). See
# docs/design/whisper-dictation.md § Deviations for the multi-PR rationale.
option(SMATCHET_WHISPER_LOCAL_BACKEND
    "Fetch + link whisper.cpp for on-device Whisper inference (~1.37 MB static delta)" ON)
# Unreal-packaged DX12 artifacts intentionally ship without Whisper (WASAPI / whisper.cpp /
# global-hotkey hook would pollute the Unreal header path). The DX12/Unreal exclusion is keyed
# on the SMATCHET_EMBEDDED_IN_UNREAL *compile-define* (target_compile_definitions on the DX12
# targets below), NOT a CMake variable — the variable is never set()/option()'d, so the old
# `if(SMATCHET_EMBEDDED_IN_UNREAL)` branch here was dead code. Whisper is gated per-target.
option(SMATCHET_BUILD_TESTS "Build CTest target for pure-logic Source/Core unit tests (doctest)" OFF)
option(SMATCHET_BUILD_UI_TESTS "Build ImGui Test Engine bucket-E surface (in-process; opt-in)" OFF)
# Gate the entire GUI/app surface — GLFW, Dear ImGui (+ImGuiLib), Lua/sol2,
# whisper.cpp, the SmatchetStandalone exe, and every DX12/Unreal target — behind
# one option. Default ON: every existing preset (Windows iter/debug/test/publish,
# Unreal, the asan/ui-test presets) is byte-for-byte unaffected. Flipped OFF only
# by the headless Linux ThreadSanitizer preset (ninja-tsan-linux), where building
# GLFW would drag in X11/OpenGL -dev packages and the Lua tarball download for zero
# TSan value — the curated SmatchetTsanTests target (tests/CMakeLists.txt) links
# only the portable, ImGui-free threading subset. Pure-additive (hardening #10).
option(SMATCHET_BUILD_APP "Build the GUI application + GLFW/ImGui/Lua/whisper/Standalone/DX12 targets (OFF for the headless TSan-subset Linux preset)" ON)
option(SMATCHET_BUILD_LUA_TESTS "Build SmatchetLuaTests binary (sandbox + timeout + stubs-compile drift sentinel; requires SMATCHET_WITH_LUA_AUTOMATION=ON)" OFF)
# Slice 0b of the Phase-0 mobile triple-target spike (docs/plans/mobile-app-jql-mvp.md).
# Compile-only portability gate: builds CORE_SOURCES with host Linux clang through the
# non-_WIN32 (#else) side of every platform #ifdef BEFORE the NDK toolchain adds ABI /
# sysroot variables. Realized as a STATIC EXCLUDE_FROM_ALL lib (SmatchetCore_PosixCheck)
# — a static archive only compiles, never links, so it exercises the #else branches with
# zero GLFW/GL/X11 link surface (TLS/cpr resolve against the host's system OpenSSL). Built
# only by the advisory Linux-clang CI job; default OFF keeps every existing preset
# byte-for-byte unaffected.
option(SMATCHET_BUILD_POSIX_CORE_CHECK "Compile CORE_SOURCES under host Linux clang (non-_WIN32 #else portability gate; compile-only static lib, no link)" OFF)
# Slice 7 of docs/design/autonomous-debugging-no-creds.md — production-resident
# NDJSON helper at tests/_debug/SmatchetAgentDebug.h. OFF in iter/publish
# (macro expands to ((void)0); zero overhead); ON in debug/asan/ui-test
# presets so debug-detective can read structured boundary-crossing logs.
option(SMATCHET_AGENT_DEBUG "Enable the production-resident SmatchetAgentDebug NDJSON helper (slice 7 of autonomous-debugging-no-creds)" OFF)
if(SMATCHET_BUILD_TESTS)
    option(SMATCHET_BUILD_AGENT_DEBUG_TESTS "Build the SmatchetAgentDebug NDJSON helper doctest" ON)
else()
    option(SMATCHET_BUILD_AGENT_DEBUG_TESTS "Build the SmatchetAgentDebug NDJSON helper doctest" OFF)
endif()
# Issue #1133 — compile-gated synthetic fault injector for the #1122 dynamic-texture
# crash class. Default OFF; the SmatchetTextureFaultInjector entry points are inline
# no-ops and its .cpp is an empty TU unless this is defined (so the injector adds ZERO
# ship-build surface — never set on ninja-iter-msvc / ninja-publish-msvc / Unreal).
# Flipped ON only by ninja-test-msvc / ninja-ui-test-msvc (+ their clang siblings) so the
# headless `mobile-texture-guard` scenario can drive the guard's recovery path. Wired onto
# SmatchetCoreInterface so the shared CORE_SOURCES injector TU sees it in the standalone
# build (DX12/Unreal never set it; the TU compiles empty there).
option(SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION
    "Compile the synthetic #1122 texture-fault injector (test/ui-test only; OFF in ship builds)" OFF)
option(SMATCHET_START_HIDDEN_UNTIL_FIRST_FRAME
    "Standalone: create GLFW window hidden; show after first SwapBuffers (perceived startup)"
    ON)
set(SMATCHET_STANDALONE_OUTPUT_NAME "Smatchet" CACHE STRING
    "Base filename for the standalone executable without extension")
set(SMATCHET_STANDALONE_FILE_DESCRIPTION "Smatchet Standalone" CACHE STRING
    "Windows VERSIONINFO FileDescription for the standalone executable")
set(SMATCHET_STANDALONE_PRODUCT_NAME "Smatchet" CACHE STRING
    "Windows VERSIONINFO ProductName for the standalone executable")
set(SMATCHET_STANDALONE_ORIGINAL_FILENAME "${SMATCHET_STANDALONE_OUTPUT_NAME}.exe")
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
        # /wd5321: this codebase is UTF-8-first (/utf-8 is forced globally) and
        # deliberately embeds raw UTF-8 (em-dash, bullets, …) in narrow string
        # literals in the localization tables (SmatchetLocalization.cpp). C5321
        # ("nonstandard extension: encoding '\xNN' as a multi-byte utf-8
        # character") was added in MSVC 14.40+ and its "use \u for cross-platform
        # compatibility" advice is moot under /utf-8 — forcing \u escapes on every
        # translated string would gut readability. Suppress only this one benign
        # extension warning; every real warning class stays /WX-fatal.
        target_compile_options("${_target}" PRIVATE /W4 /wd5321)
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
# CI is the exception: its .fetchcontent-src is restored from an actions/cache prefix fallback,
# which after a dependency pin bump can predate the new ref — the disconnected update step then
# FATALs the configure ("Requested git ref ... is not present locally"). In CI the update step
# may contact the remote so a stale restored cache self-heals; local/IDE configures keep the
# fast disconnected path.
if(NOT DEFINED FETCHCONTENT_UPDATES_DISCONNECTED)
    if(DEFINED ENV{CI})
        set(FETCHCONTENT_UPDATES_DISCONNECTED OFF CACHE BOOL
            "Allow FetchContent update step to fetch in CI (stale restored caches self-heal)")
    else()
        set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL
            "Skip FetchContent update step when content is already populated (faster IDE re-configure)")
    endif()
endif()

include(FetchContent)
include(cmake/SmatchetThirdParty.cmake)
include(cmake/Sanitizers.cmake)

# /RTC1-vs-/fsanitize=address fix (infra: asan-doctest-rig-not-instrumented).
# Must run BEFORE any add_library/add_executable so the cleaned Debug flags reach
# every target (incl. SmatchetTests). No-op unless real MSVC cl + SMATCHET_SANITIZER=asan.
smatchet_strip_msvc_rtc_for_asan()

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
# GIT_SUBMODULES "" — skip json's submodule fetch (test-data only; not needed for
# the header-only lib). The submodule-update sub-step runs `git submodule` via a
# cmd.exe spawn whose PATH (under a vcvars64-sourced shell) drops Git's
# libexec/git-core, so the helper `git-sh-setup` can't be found and the clone
# fails with "Failed to update submodules in: .../json-src". Declaring no
# submodules sidesteps it entirely. See docs/guides/offline-builds.md
# § Troubleshooting + agents/core/build-doctor.md § Common causes.
# Pinned to the exact commit the v3.11.3 lightweight tag resolves to (reproducible
# builds / supply chain) — same content the floating tag points to today, zero
# behaviour change. Lightweight tag (points straight at the commit); resolved via:
# git ls-remote https://github.com/nlohmann/json.git refs/tags/v3.11.3
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03  # json v3.11.3
    GIT_SUBMODULES "")
FetchContent_MakeAvailable(json)

# cpr (libcurl HTTP) is app-only — the headless TSan subset links no HTTP client.
# Gated behind SMATCHET_BUILD_APP so the Linux runner never FetchContent-builds
# cpr's bundled curl (heavy, and the codeload download is 504-flaky). Pure-additive:
# ON for every existing preset, so the Windows test rig still gets cpr.
if(SMATCHET_BUILD_APP)
smatchet_fetch_status("Fetching cpr (C++ Requests)...")
smatchet_prepare_cpr()
# Pinned to the exact commit the 1.9.2 tag resolves to (reproducible builds /
# supply chain) — same content the floating tag points to today, zero behaviour
# change. 1.9.2 is a lightweight tag (points straight at the commit); resolved
# via: git ls-remote https://github.com/libcpr/cpr.git 1.9.2
# GIT_SUBMODULES "" — same fresh-worktree/CI-cold-cache failure mode as json
# (above): FetchContent runs `git submodule update --init` unconditionally after
# the clone, and that bare `git submodule` wrapper resolves `git-sh-setup` via a
# vcvars64-sourced PATH that drops Git's libexec/git-core, dying with
# "git-sh-setup: file not found" → "Failed to update submodules in: .../cpr-src"
# → "Build step for cpr failed: 1". cpr 1.9.2 carries no submodules we link (it
# FetchContents its own deps; submodules are test-data/CI only), so declaring no
# submodules sidesteps the doomed wrapper call entirely. See
# docs/guides/offline-builds.md § Troubleshooting + agents/core/build-doctor.md.
FetchContent_Declare(cpr GIT_REPOSITORY https://github.com/libcpr/cpr.git GIT_TAG f88fd7737de3e640c61703eb57a0fa0ce00c60cd  # cpr 1.9.2
    GIT_SUBMODULES "")
FetchContent_GetProperties(cpr)
if(NOT cpr_POPULATED)
    FetchContent_Populate(cpr)
endif()
smatchet_patch_cpr_after_fetch("${cpr_SOURCE_DIR}")
# cpr 1.9.2 appends -Wall -Wextra -Wpedantic -Werror to CMAKE_CXX_FLAGS
# (line 76 of its CMakeLists.txt). clang-cl fires dozens of Clang-specific
# warnings under -Wpedantic that GCC silently ignores. Patch the vendored
# CMakeLists.txt to strip -Werror before add_subdirectory processes it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    file(READ "${cpr_SOURCE_DIR}/CMakeLists.txt" _cpr_cml)
    # Load-bearing for Clang: clang-cl fires dozens of warnings under -Wpedantic
    # that turn fatal under cpr's -Werror. A silent no-op after a cpr bump would
    # red-wall the Clang build; FATAL at configure names the vanished target.
    smatchet_patch_or_die(_cpr_cml "-Wpedantic -Werror" "-Wpedantic" "cpr CMakeLists.txt -Werror strip (Clang)")
    file(WRITE "${cpr_SOURCE_DIR}/CMakeLists.txt" "${_cpr_cml}")
endif()
add_subdirectory("${cpr_SOURCE_DIR}" "${cpr_BINARY_DIR}" EXCLUDE_FROM_ALL)
endif() # SMATCHET_BUILD_APP — cpr/curl (app-only HTTP)

smatchet_fetch_status("Fetching SQLiteCpp...")
smatchet_prepare_sqlitecpp()
# Pinned to the exact commit the 3.3.1 annotated tag resolves to (reproducible
# builds / supply chain) — same content the floating tag points to today, zero
# behaviour change. 3.3.1 is an annotated tag; resolved to its underlying commit
# via: git ls-remote https://github.com/SRombauts/SQLiteCpp.git 'refs/tags/3.3.1^{}'
# GIT_SUBMODULES "" — SQLiteCpp vendors googletest as a `.gitmodules` submodule
# (test-only; we build with tests off). Two reasons to skip it: it spares a
# googletest clone we never link, AND it avoids the same fresh-worktree/CI
# cold-cache failure as json/cpr above — FetchContent's unconditional
# `git submodule update --init` dies on `git-sh-setup: file not found` under the
# vcvars64-stripped PATH ("Failed to update submodules in: .../sqlitecpp-src").
FetchContent_Declare(sqlitecpp GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git GIT_TAG fe6086c12679d8744c02fa03e83e75feca2413d8  # SQLiteCpp 3.3.1
    GIT_SUBMODULES "")
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
# Pinned to the exact commit the v0.49.0 lightweight tag resolves to (reproducible
# builds / supply chain). Bumped from v0.14.1 to clear the OSV/CVE findings on the
# older pin (CVE-2025-46728/-52887/-53628/-53629/-66570/-66577 and
# CVE-2026-21428/-22776/-28434/-28435/-29076/-31870/-32627/-33745/-34441); the
# latest of these is fixed upstream in v0.40.0, so v0.49.0 clears all of them.
# Lightweight tag; resolved via:
# git ls-remote https://github.com/yhirose/cpp-httplib.git refs/tags/v0.49.0
# GIT_SUBMODULES "" — cpp-httplib has no submodules we link, but FetchContent's
# generated gitclone script runs `git submodule update --recursive --init`
# UNCONDITIONALLY (it does not pre-check `.gitmodules`). On a fresh worktree/CI
# cold cache that bare `git submodule` wrapper resolves `git-sh-setup` via a
# vcvars64-sourced PATH that drops Git's libexec/git-core and dies with
# "git-sh-setup: file not found" → "Failed to update submodules". Empirically
# confirmed: httplib fails the fresh fetch with NO `.gitmodules` present. Skip
# the step. Same fix as json/cpr/sqlitecpp/sol2 above.
FetchContent_Declare(httplib GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git GIT_TAG 2132205e1a69c9fce8096f085b1b8d72efc759fa  # cpp-httplib v0.49.0
    GIT_SUBMODULES "")
FetchContent_MakeAvailable(httplib)


# md4c is the Markdown parser backing the rich-text editing pipeline (md ↔ ADF/HTML for Jira/Plane
# description fields). We compile only `md4c.c` ourselves rather than using upstream's CMake project,
# which also builds md4c-html and the md2html CLI we don't need. See RICH_TEXT_EDITING_V2_PLAN.md.
smatchet_fetch_status("Fetching md4c (CommonMark/GFM parser)...")
FetchContent_Declare(md4c
    GIT_REPOSITORY https://github.com/mity/md4c.git
    # Pinned to the exact commit the release-0.5.2 lightweight tag resolves to
    # (reproducible builds / supply chain) — same content, zero behaviour change.
    # Lightweight tag; resolved via:
    # git ls-remote https://github.com/mity/md4c.git refs/tags/release-0.5.2
    GIT_TAG 729e6b8b320caa96328968ab27d7db2235e4fb47  # md4c release-0.5.2
    # GIT_SUBMODULES "" — md4c has no submodules we link, but FetchContent runs
    # `git submodule update --recursive --init` unconditionally; on a fresh
    # worktree/CI cold cache that wrapper dies on "git-sh-setup: file not found"
    # under the vcvars64-stripped PATH. Skip it. Same fix as json/cpr/httplib.
    GIT_SUBMODULES ""
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
    # Pinned to the exact commit the v1.5.14 lightweight tag resolves to
    # (reproducible builds / supply chain) — same content, zero behaviour change.
    # Lightweight tag; resolved via:
    # git ls-remote https://github.com/gulrak/filesystem.git refs/tags/v1.5.14
    GIT_TAG 8a2edd6d92ed820521d42c94d179462bf06b5ed3  # ghc::filesystem v1.5.14
    # GIT_SUBMODULES "" — ghc::filesystem has no submodules we link, but
    # FetchContent runs `git submodule update --recursive --init` unconditionally;
    # on a fresh worktree/CI cold cache that wrapper dies on "git-sh-setup: file
    # not found" under the vcvars64-stripped PATH. Skip it. Same fix as
    # json/cpr/httplib.
    GIT_SUBMODULES ""
)

# FIX: CMake <3.10 compatibility removal warning
# The upstream ghc_filesystem CMakeLists starts with:
#   cmake_minimum_required(VERSION 3.7.2)
# Newer CMake versions emit a deprecation warning for this.
#
# Cosmetic / best-effort: only suppresses that deprecation warning. NOT routed
# through smatchet_patch_or_die — if a future ghc bump raises its own
# cmake_minimum_required past 3.10 this target legitimately disappears, and
# FATAL-ing on a vanished cosmetic target would be wrong. Left a plain
# string(REPLACE).
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

# ============================================================================
# GUI / APPLICATION SURFACE (SMATCHET_BUILD_APP) — everything from GLFW down to
# the SmatchetStandalone exe and the DX12/Unreal targets. The portable Core
# FetchContent deps above (json, cpr, SQLiteCpp, cpp-httplib, md4c,
# ghc::filesystem) are declared BEFORE this guard so the headless TSan-subset
# Linux build (SMATCHET_BUILD_APP=OFF) still links the threading subset without
# fetching/building GLFW, Dear ImGui, Lua, sol2, whisper.cpp, or any app target.
# Default ON — no behavioural change for any existing preset. See the option
# declaration above + agents/build-doctor.md (hardening #10 / Slice-5 TSan).
# ============================================================================
if(SMATCHET_BUILD_APP)

# GLFW is the desktop windowing/GL-context backend. Mobile (Android/iOS) drives
# the GL context through the platform host (EGL on Android), so skip the GLFW
# fetch+build entirely there — building GLFW against the NDK sysroot has no X11/
# Win32/Cocoa surface and would fail configure. ImGuiLib_Mobile (below) pairs the
# android + opengl3 backends in place of imgui_impl_glfw.
if(NOT ANDROID AND NOT IOS)
    smatchet_fetch_status("Fetching GLFW...")
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # Pinned to the exact commit the 3.3.8 annotated tag resolves to (reproducible
    # builds / supply chain) — same content the floating tag points to today, zero
    # behaviour change. 3.3.8 is an annotated tag; resolved to its underlying commit
    # (NOT the tag-object SHA, which would re-fetch) via:
    # git ls-remote https://github.com/glfw/glfw.git 'refs/tags/3.3.8^{}'
    # GIT_SUBMODULES "" — glfw has no submodules we link, but FetchContent runs
    # `git submodule update --recursive --init` unconditionally; on a fresh
    # worktree/CI cold cache that wrapper dies on "git-sh-setup: file not found"
    # under the vcvars64-stripped PATH. Skip it. Same fix as json/cpr/httplib.
    FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 7482de6071d21db77a7236155da44c172a7f6c9e  # glfw 3.3.8
        GIT_SUBMODULES "")
    FetchContent_MakeAvailable(glfw)
endif()

# ==============================================================================
# LUA & SOL2 (C++14 COMPATIBLE) — optional (SMATCHET_WITH_LUA_AUTOMATION)
# ==============================================================================
if(SMATCHET_WITH_LUA_AUTOMATION)
    set(LUA_SRC_DIR "${FETCHCONTENT_BASE_DIR}/lua-src")
    if(NOT EXISTS "${LUA_SRC_DIR}")
        smatchet_fetch_status("Downloading Lua 5.3.6 source...")
        file(MAKE_DIRECTORY "${FETCHCONTENT_BASE_DIR}")
        set(_lua_tar "${FETCHCONTENT_BASE_DIR}/lua-5.3.6.tar.gz")
        # Pin the upstream tarball hash — lua.org publishes no checksum companion,
        # so this is a TOFU pin (sha256 of the canonical lua-5.3.6.tar.gz) that
        # blocks a silently-tampered mirror / MITM from compiling unverified C
        # source into both targets. Every other dependency is an immutable git ref.
        #
        # Lua is the ONE dependency fetched over plain HTTP (lua.org publishes no
        # git ref), so a lua.org outage red-walls the whole build (observed: curl
        # error 28 "Timeout was reached" took develop red for hours). Fetch from a
        # github-hosted mirror we control first (byte-identical canonical tarball,
        # see origin/bug-report-assets:deps/), fall back to lua.org. Each URL is
        # retried, and the bytes are hash-verified AFTER download (not via
        # file(DOWNLOAD EXPECTED_HASH), which hard-errors and would block fallback)
        # so a tampered/wrong mirror is discarded and the next source is tried.
        set(_lua_sha256 "fc5fd69bb8736323f026672b1b7235da613d7177e72558893a0bdcd320466d60")
        set(_lua_urls
            "https://raw.githubusercontent.com/alexandrosk0/Smatchet/bug-report-assets/deps/lua-5.3.6.tar.gz"
            "https://www.lua.org/ftp/lua-5.3.6.tar.gz")
        set(_lua_ok FALSE)
        foreach(_url IN LISTS _lua_urls)
            foreach(_attempt RANGE 1 3)
                file(DOWNLOAD "${_url}" "${_lua_tar}"
                    TIMEOUT 120 INACTIVITY_TIMEOUT 30 STATUS _dl_status)
                list(GET _dl_status 0 _dl_code)
                if(_dl_code EQUAL 0)
                    file(SHA256 "${_lua_tar}" _lua_got)
                    if(_lua_got STREQUAL _lua_sha256)
                        set(_lua_ok TRUE)
                        break()
                    endif()
                    message(WARNING "Lua tarball from ${_url} hash mismatch "
                        "(got ${_lua_got}); discarding and trying next source.")
                else()
                    list(GET _dl_status 1 _dl_msg)
                    message(STATUS "Lua download failed (${_url}, attempt ${_attempt}): ${_dl_msg}")
                endif()
                file(REMOVE "${_lua_tar}")
            endforeach()
            if(_lua_ok)
                break()
            endif()
        endforeach()
        if(NOT _lua_ok)
            message(FATAL_ERROR "Failed to download Lua 5.3.6 from all mirrors "
                "(${_lua_urls}). Last status: ${_dl_status}")
        endif()
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${_lua_tar}" WORKING_DIRECTORY "${FETCHCONTENT_BASE_DIR}")
        file(RENAME "${FETCHCONTENT_BASE_DIR}/lua-5.3.6" "${LUA_SRC_DIR}")
        unset(_lua_tar)
        unset(_lua_urls)
        unset(_lua_sha256)
        unset(_lua_ok)
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
        # The outer SMATCHET_LUA_EXTERN_C_PATCH find is the idempotency sentinel
        # (NOT redundant with the helper): the patch's <find> "#define LUA_API\t
        # \textern" stays a substring of the patched "#define LUA_API\t\textern
        # \"C\"" form, so re-running the bare helper would re-wrap. Once the
        # sentinel token is present we skip. On a fresh fetch (new Lua version)
        # where the token is absent, the helper runs and FATALs if the LUA_API
        # define text itself vanished — this patch is load-bearing (Lua-as-C++
        # link failure if dropped).
        string(FIND "${_luaconf_contents}" "SMATCHET_LUA_EXTERN_C_PATCH" _patched_idx)
        if(_patched_idx EQUAL -1)
            set(_luaconf_patched "${_luaconf_contents}")
            smatchet_patch_or_die(_luaconf_patched
                "#define LUA_API\t\textern"
                "/* SMATCHET_LUA_EXTERN_C_PATCH: Lua 5.3 lacks extern \"C\" guards; force them when compiled as C++ so symbol names match the sol2 caller side that includes via lua.hpp. */\n#if defined(__cplusplus)\n#define LUA_API\t\textern \"C\"\n#else\n#define LUA_API\t\textern\n#endif"
                "luaconf.h extern \"C\" guards (Lua-as-C++)")
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

    # Pinned to the exact commit the v2.20.6 annotated tag resolves to
    # (reproducible builds / supply chain) — same content the floating tag points
    # to today, zero behaviour change. v2.20.6 is an annotated tag; resolved to
    # its underlying commit via:
    #   git ls-remote https://github.com/ThePhD/sol2.git 'refs/tags/v2.20.6^{}'
    # GIT_SUBMODULES "" — sol2 vendors Catch as a `.gitmodules` submodule
    # (test-only; never linked here). Skips the Catch clone AND avoids the same
    # fresh-worktree/CI cold-cache failure as json/cpr/sqlitecpp above:
    # FetchContent's unconditional `git submodule update --init` dies on
    # `git-sh-setup: file not found` under the vcvars64-stripped PATH.
    FetchContent_Declare(sol2 GIT_REPOSITORY https://github.com/ThePhD/sol2.git GIT_TAG 902ceaf8a1d53ea1b70cf9f3f0302da87f5a7f8c  # sol2 v2.20.6
        GIT_SUBMODULES "")

    FetchContent_GetProperties(sol2)
    if(NOT sol2_POPULATED)
        FetchContent_Populate(sol2)
    endif()

    set(_sol2_cmakelists "${sol2_SOURCE_DIR}/CMakeLists.txt")
    if(EXISTS "${_sol2_cmakelists}")
        file(READ "${_sol2_cmakelists}" _sol2_cmakelists_contents)

        # Cosmetic / best-effort: only suppresses a CMake deprecation warning.
        # NOT routed through smatchet_patch_or_die — these are two alternative
        # spellings (3.5.0 / 3.5); on any given sol2 version at most one matches,
        # so the helper's vanish-FATAL would fire on the non-matching spelling
        # even on a healthy fresh tree. Left plain string(REPLACE).
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
        set(_sol2_single_patched "${_sol2_single_contents}")

        # HEAP-CORRUPTION-CRITICAL (Pillar 3): every replace below is routed
        # through smatchet_patch_or_die so a sol2 bump that changes the upstream
        # text FATALs at configure instead of silently no-op'ing and resurrecting
        # the GCC/MinGW __gc-metatable-collision heap corruption. Two groups:
        #
        #   PRIMARY (cstdint include + detail:: struct injection + the four
        #   metatable-key rewrites) — must run FIRST.
        #
        #   RE-RUN CLEANUP (the four ".append(...suffix...)" double-suffix fixers)
        #   — only match an OLD already-double-appended tree. On a FRESH tree their
        #   <find> (the double-appended form) is absent, but their <replace> (the
        #   single-suffix form the PRIMARY group just produced) is PRESENT, so
        #   smatchet_patch_or_die passes via the already-present branch and
        #   no-ops cleanly. This is exactly why the PRIMARY group must run first.
        smatchet_patch_or_die(_sol2_single_patched
            "#include <cctype>\n#if defined(__GNUC__) && defined(__MINGW32__) && (__GNUC__ < 6)"
            "#include <cctype>\n#include <cstdint>\n#if defined(__GNUC__) && defined(__MINGW32__) && (__GNUC__ < 6)"
            "sol2 sol.hpp <cstdint> include")

        smatchet_patch_or_die(_sol2_single_patched
            "namespace sol {\n\n\ttemplate <typename T>\n\tstruct usertype_traits {"
            "namespace sol {\n\n\tnamespace detail {\n\t\ttemplate <typename T>\n\t\tstruct smatchet_type_id_token {\n\t\t\tstatic char id;\n\t\t};\n\n\t\ttemplate <typename T>\n\t\tchar smatchet_type_id_token<T>::id;\n\n\t\ttemplate <typename T>\n\t\tstd::string smatchet_type_identity_suffix() {\n\t\t\treturn std::string(\"@\").append(std::to_string(reinterpret_cast<std::uintptr_t>(&smatchet_type_id_token<T>::id)));\n\t\t}\n\t} // namespace detail\n\n\ttemplate <typename T>\n\tstruct usertype_traits {"
            "sol2 sol.hpp detail::smatchet_type_id_token injection")

        smatchet_patch_or_die(_sol2_single_patched
            "static const std::string m = std::string(\"sol.\").append(detail::demangle<T>());"
            "static const std::string m = std::string(\"sol.\").append(detail::smatchet_type_identity_suffix<T>());"
            "sol2 sol.hpp metatable key m")
        smatchet_patch_or_die(_sol2_single_patched
            "static const std::string u_m = std::string(\"sol.\").append(detail::demangle<T>()).append(\".user\");"
            "static const std::string u_m = std::string(\"sol.user\").append(detail::smatchet_type_identity_suffix<T>());"
            "sol2 sol.hpp metatable key u_m")
        smatchet_patch_or_die(_sol2_single_patched
            "static const std::string u_g_m = std::string(\"sol.\").append(detail::demangle<T>()).append(\".user\\xE2\\x99\\xBB\");"
            "static const std::string u_g_m = std::string(\"sol.usergc\").append(detail::smatchet_type_identity_suffix<T>());"
            "sol2 sol.hpp metatable key u_g_m")
        smatchet_patch_or_die(_sol2_single_patched
            "static const std::string g_t = std::string(\"sol.\").append(detail::demangle<T>()).append(\".\\xE2\\x99\\xBB\");"
            "static const std::string g_t = std::string(\"sol.gc\").append(detail::smatchet_type_identity_suffix<T>());"
            "sol2 sol.hpp metatable key g_t")

        # Re-run cleanup — collapse a double-appended suffix from an older patched
        # tree back to the single-suffix form. Safe on a fresh tree (passes via
        # the already-present <replace> branch — see the block comment above).
        smatchet_patch_or_die(_sol2_single_patched
            "static const std::string m = std::string(\"sol.\").append(detail::demangle<T>()).append(detail::smatchet_type_identity_suffix<T>());"
            "static const std::string m = std::string(\"sol.\").append(detail::smatchet_type_identity_suffix<T>());"
            "sol2 sol.hpp metatable key m (re-run cleanup)")
        smatchet_patch_or_die(_sol2_single_patched
            "static const std::string u_m = std::string(\"sol.\").append(detail::demangle<T>()).append(\".user\").append(detail::smatchet_type_identity_suffix<T>());"
            "static const std::string u_m = std::string(\"sol.user\").append(detail::smatchet_type_identity_suffix<T>());"
            "sol2 sol.hpp metatable key u_m (re-run cleanup)")
        smatchet_patch_or_die(_sol2_single_patched
            "static const std::string u_g_m = std::string(\"sol.\").append(detail::demangle<T>()).append(\".user\\xE2\\x99\\xBB\").append(detail::smatchet_type_identity_suffix<T>());"
            "static const std::string u_g_m = std::string(\"sol.usergc\").append(detail::smatchet_type_identity_suffix<T>());"
            "sol2 sol.hpp metatable key u_g_m (re-run cleanup)")
        smatchet_patch_or_die(_sol2_single_patched
            "static const std::string g_t = std::string(\"sol.\").append(detail::demangle<T>()).append(\".\\xE2\\x99\\xBB\").append(detail::smatchet_type_identity_suffix<T>());"
            "static const std::string g_t = std::string(\"sol.gc\").append(detail::smatchet_type_identity_suffix<T>());"
            "sol2 sol.hpp metatable key g_t (re-run cleanup)")

        if(NOT _sol2_single_patched STREQUAL _sol2_single_contents)
            file(WRITE "${_sol2_single_header}" "${_sol2_single_patched}")
        endif()

        # Part D — CI smoke (plan #1 Verification): assert the heap-corruption fix
        # is actually present in the on-disk header after the patch step. file(READ)
        # back and FATAL if the patched token is absent. This makes a silently
        # broken sol2 patch fail `cmake` configure, which CI runs everywhere — no
        # extra CI infra. Gated to this SMATCHET_WITH_LUA_AUTOMATION block, the
        # only path that fetches sol2.
        file(READ "${_sol2_single_header}" _sol2_single_verify)
        string(FIND "${_sol2_single_verify}" "detail::smatchet_type_identity_suffix<T>()" _sol2_token_pos)
        if(_sol2_token_pos EQUAL -1)
            message(FATAL_ERROR
                "sol2 metatable-token patch verification FAILED: "
                "'detail::smatchet_type_identity_suffix<T>()' is absent from "
                "${_sol2_single_header} after the patch step. The GCC/MinGW "
                "__gc-metatable-collision heap-corruption fix is NOT in effect. "
                "A sol2 bump likely changed the upstream source — re-derive the "
                "metatable-key patches in CMakeLists.txt before continuing.")
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
    # Pinned to the exact commit the v1.7.4 lightweight tag resolves to
    # (reproducible builds / supply chain) — same content, zero behaviour change.
    # Lightweight tag; resolved via:
    # git ls-remote https://github.com/ggerganov/whisper.cpp.git refs/tags/v1.7.4
    FetchContent_Declare(whisper_cpp
        GIT_REPOSITORY https://github.com/ggerganov/whisper.cpp.git
        GIT_TAG 8a9ad7844d6e2a10cddf4b92de4089d7ac2b14a9  # whisper.cpp v1.7.4
        # GIT_SUBMODULES "" — whisper.cpp bundles ggml in-tree (no submodule we
        # link); FetchContent's unconditional `git submodule update --init`
        # otherwise dies on "git-sh-setup: file not found" under the
        # vcvars64-stripped PATH on a fresh worktree/CI cold cache. Same fix as
        # json/cpr/httplib.
        GIT_SUBMODULES "")
    FetchContent_MakeAvailable(whisper_cpp)
endif()

# Compile Dear ImGui (with GLFW and OpenGL3 backends)
smatchet_fetch_status("Fetching Dear ImGui...")
FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        # Pinned to a specific commit on the docking branch (reproducible builds / supply chain).
        # v1.92.7-docking-39-g329c5a6b3
        GIT_TAG 329c5a6b3be75ebf54506d3ae94b836ffcf19fa0
        # GIT_SUBMODULES "" — Dear ImGui has no submodules we link, but
        # FetchContent runs `git submodule update --recursive --init`
        # unconditionally; on a fresh worktree/CI cold cache that wrapper dies on
        # "git-sh-setup: file not found" under the vcvars64-stripped PATH. Skip
        # it. Same fix as json/cpr/httplib.
        GIT_SUBMODULES ""
)

FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()
# Desktop world (GLFW + OpenGL3 backends). Guarded out on Android — glfw is not
# fetched there and imgui_impl_glfw.cpp #includes <GLFW/glfw3.h>, so building this
# variant on the NDK toolchain would fail. The Android world builds ImGuiLib_Mobile
# in the else() arm instead.
if(NOT ANDROID)
    file(GLOB IMGUI_SOURCES "${imgui_SOURCE_DIR}/*.cpp")
    list(APPEND IMGUI_SOURCES "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
    # Windows+MSVC standalone defaults to the native DX12 renderer (the GL path is
    # software-emulated on Windows-on-ARM64), so the DX12 ImGui backend ships in the
    # same desktop ImGuiLib the standalone links — Dx12Bootstrap.cpp resolves
    # ImGui_ImplDX12_* from here. Guarded WIN32 AND MSVC: MinGW stays GL-only, and
    # this is NOT IMGUI_DX12_SOURCES (that feeds the separate ImGuiLib_DX12 — linking
    # both into one target double-defines the impl symbols). clang-cl defines
    # _MSC_VER too, matching the SMATCHET_STANDALONE_HAS_DX12 predicate.
    if(WIN32 AND MSVC)
        list(APPEND IMGUI_SOURCES "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp")
    endif()
    add_library(ImGuiLib STATIC ${IMGUI_SOURCES})
    target_include_directories(ImGuiLib PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
            ${glfw_SOURCE_DIR}/include
            # Source/Core/include hosts SmatchetImConfig.h — the single source of
            # ImGui compile-time configuration (IMGUI_USE_WCHAR32, optional test
            # engine hookup). Adding it PUBLIC so every linking target's #include
            # "imgui.h" → IMGUI_USER_CONFIG path resolves cleanly without a
            # per-target include-dir tweak.
            ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include
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
else()
    # Android world — imgui_impl_android + imgui_impl_opengl3 (the GL3 backend
    # auto-selects the GLES3 path on __ANDROID__). No GLFW dependency: the platform
    # host owns the EGL surface. Same IMGUI_USER_CONFIG=SmatchetImConfig.h as the
    # desktop lib so ImWchar / supplementary-plane config + the test-engine hooks
    # stay ABI-identical across worlds (mirrors the per-world ImGuiLib_DX12 split).
    file(GLOB IMGUI_MOBILE_SOURCES "${imgui_SOURCE_DIR}/*.cpp")
    list(APPEND IMGUI_MOBILE_SOURCES "${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp" "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
    add_library(ImGuiLib_Mobile STATIC ${IMGUI_MOBILE_SOURCES})
    target_include_directories(ImGuiLib_Mobile PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
            ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include
    )
    target_compile_definitions(ImGuiLib_Mobile PUBLIC IMGUI_USER_CONFIG="SmatchetImConfig.h")
    if(SMATCHET_BUILD_UI_TESTS)
        target_compile_definitions(ImGuiLib_Mobile PUBLIC SMATCHET_BUILD_UI_TESTS=1)
    endif()
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
            ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include
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
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "Source/Core/src/*.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Ui/SmatchetImGuiHost.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Ui/SmatchetImGuiHostC_Diagnostics.cpp")
# ImGuiColorTextEdit — vendored third-party TU promoted from Source/Plugins/LuaConsole/ThirdParty
# so Source/Core (AI chat panel) can consume it. The glob above only matches
# Source/Core/src/*.cpp; the ThirdParty TU needs an explicit path.
#
# It is built in a dedicated per-world OBJECT library (NOT appended to CORE_SOURCES)
# rather than compiled directly into SmatchetStandalone / SmatchetCore_DX12. Reason:
# those targets carry /W4 (MSVC) via smatchet_apply_strict_warnings() at *target* scope,
# and a per-source /W0 override on the TU then produced two /W flags on one command line
# (MSVC: "D9025: overriding '/W4' with '/W0'", and the suppression leaked C4189 anyway).
# The OBJECT libraries below are never passed through smatchet_apply_strict_warnings(),
# so each compiles with a single warning flag — no collision, no D9025.
#
# Two libraries (one per world) are required because TextEditor.cpp #includes imgui.h and
# must inherit the *matching* ImGui compile-time config: ImGuiLib for Standalone,
# ImGuiLib_DX12 for the Unreal/DX12 world (see the ImGui config block above — the wchar /
# supplementary-plane config and IMGUI_USE_WCHAR32 handling differ between worlds, and a
# single shared object lib would ABI-mismatch one of them). The libraries are wired into
# the respective targets by smatchet_configure_{opengl,dx12}_core_impl_target() below.
set(_SMATCHET_IMGUI_TEXTEDIT_CPP
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/ThirdParty/ImGuiColorTextEdit/TextEditor.cpp")
if(MSVC)
    set(_SMATCHET_TEXTEDIT_WARN_FLAG /W0)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(_SMATCHET_TEXTEDIT_WARN_FLAG -w)
else()
    set(_SMATCHET_TEXTEDIT_WARN_FLAG)
endif()

# Standalone (OpenGL/GLFW) world — inherits ImGuiLib's PUBLIC include dirs + IMGUI config.
add_library(SmatchetTextEdit OBJECT "${_SMATCHET_IMGUI_TEXTEDIT_CPP}")
# Inherit the matching per-world ImGui config: ImGuiLib_Mobile on Android (no glfw
# include), ImGuiLib on desktop. The TU only needs imgui.h include propagation +
# the shared IMGUI_USER_CONFIG, so the ABI stays identical to its consuming target.
if(ANDROID)
    target_link_libraries(SmatchetTextEdit PRIVATE ImGuiLib_Mobile)
else()
    target_link_libraries(SmatchetTextEdit PRIVATE ImGuiLib)
endif()
if(_SMATCHET_TEXTEDIT_WARN_FLAG)
    target_compile_options(SmatchetTextEdit PRIVATE ${_SMATCHET_TEXTEDIT_WARN_FLAG})
endif()

# Unreal/DX12 world — inherits ImGuiLib_DX12's distinct ImGui config. EXCLUDE_FROM_ALL so a
# default Standalone build doesn't compile it (mirrors SmatchetCore_DX12 / ImGuiLib_DX12).
if(WIN32)
    add_library(SmatchetTextEdit_DX12 OBJECT EXCLUDE_FROM_ALL "${_SMATCHET_IMGUI_TEXTEDIT_CPP}")
    target_link_libraries(SmatchetTextEdit_DX12 PRIVATE ImGuiLib_DX12)
    if(_SMATCHET_TEXTEDIT_WARN_FLAG)
        target_compile_options(SmatchetTextEdit_DX12 PRIVATE ${_SMATCHET_TEXTEDIT_WARN_FLAG})
    endif()
endif()
# GLOB snapshots at configure time; pick up new TUs if the cache predates them (no duplicate paths).
set(_SMATCHET_CPP_SYNTAX "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Ui/CppSyntaxHighlight.cpp")
if(EXISTS "${_SMATCHET_CPP_SYNTAX}")
    list(FIND CORE_SOURCES "${_SMATCHET_CPP_SYNTAX}" _smatchet_cpp_syntax_idx)
    if(_smatchet_cpp_syntax_idx EQUAL -1)
        list(APPEND CORE_SOURCES "${_SMATCHET_CPP_SYNTAX}")
    endif()
endif()
unset(_smatchet_cpp_syntax_idx)
unset(_SMATCHET_CPP_SYNTAX)

list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindingsCore.cpp")
# AppController_LuaBindings_Draw.cpp is sol2-dependent (LuaDrawList + replay glue).
# Its AppController:: methods are stubbed in AppController_LuaStubs.cpp for the OFF build.
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Draw.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaStubs.cpp")
if(SMATCHET_WITH_LUA_AUTOMATION)
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindingsCore.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Draw.cpp")
else()
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaStubs.cpp")
endif()

# Whisper dictation router — same bindings/stubs split shape as Lua. Always exactly
# one of the two TUs is compiled; both export identical symbols (enforced by the CI
# `build-windows-no-whisper` job — see .github/workflows/build-and-test.yml).
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/DictationInsertionRouter_Whisper.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/DictationInsertionRouter_Stubs.cpp")
# Phase C — setup banner is source-list-conditional (gating layer 2). The TU
# uses ImGui + ModelDownloader + ModelCatalog, all of which are inside the
# Source/Plugins/Whisper subtree's `if(SMATCHET_WITH_WHISPER)` gate. CMake adds the
# TU only on the ON path; the OFF build never compiles it (call site in
# SmatchetUI.cpp is wrapped `#if defined(SMATCHET_WITH_WHISPER) ... #endif`).
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/SmatchetWhisperSetupBanner.cpp")
# Phase E — amplitude-meter overlay TU, source-list-conditional with the same
# shape as the setup banner. Compiled only on the ON path; the OFF build
# never visits it (call site in SmatchetUI.cpp is wrapped
# `#if defined(SMATCHET_WITH_WHISPER) ... #endif`).
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Ui/SmatchetWhisperOverlayUi.cpp")
# Phase G — whisper-dictation-roundtrip scenario. Source-list conditional with
# the same shape as the overlay + banner above: the TU references
# WhisperPlugin::SetMockTranscription which lives behind the gate, so the OFF
# build must not compile it. The matching RegisterFactory call in
# AppController.cpp is also #ifdef'd on SMATCHET_WITH_WHISPER.
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/WhisperDictationScenario.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/WhisperAiAssistantAutosendScenario.cpp")
if(SMATCHET_WITH_WHISPER)
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/DictationInsertionRouter_Whisper.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/SmatchetWhisperSetupBanner.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Ui/SmatchetWhisperOverlayUi.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/WhisperDictationScenario.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/WhisperAiAssistantAutosendScenario.cpp")
else()
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/DictationInsertionRouter_Stubs.cpp")
endif()

# AI assistant implementation — same bindings/stubs split shape as Whisper. When OFF,
# prune client/controller/UI TUs from CORE_SOURCES; call sites stay #if SMATCHET_WITH_AI.
# NOTE: AiChatTimestamp.cpp is deliberately NOT in this list. It is a pure std-lib
# time-formatting helper (smatchet::ai::NowUnixMs / FormatRelativeTime / FormatAbsoluteTime)
# with zero AI deps, now also consumed by the backend-agnostic comments modal
# (SmatchetCommentsModalUi.cpp). It must link in the AI-off light build, so it stays in the
# unconditional glob'd CORE_SOURCES — do not move it back under SMATCHET_WITH_AI (#1217).
# NOTE: AiErrorRedact.cpp is likewise NOT in this list (same #1217 rationale). It is a pure
# string-redaction helper (smatchet::ai::pure::RedactProviderErrorBody) with zero AI deps,
# now also consumed by the always-built tracker HTTP logging (TrackerHttpUtils
# RedactHttpBodyForLog, security audit #12). Pruning it from the AI-off light build left
# RedactProviderErrorBody undefined → LNK1120. It must stay in the unconditional CORE_SOURCES.
set(_SMATCHET_AI_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AgentsMdLoader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiAssistantController.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiClientFactory.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiContextBuilder.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiEndpointSanitize.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiModelCatalog.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiModelSignature.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiNdjsonParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiPrefsTestConnection.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiPrefsValidator.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiSseParser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AnthropicClient.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/OllamaClient.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/OllamaStreamError.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/OpenAiClient.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/SmatchetChatPersistWorker.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Commands/Builtin/BuiltinCommands_Ai.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/AiAssistantStreamingHappyPathScenario.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/AiAssistantStreamingTransportDownScenario.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Commands/Scenarios/AiChatHistoryRenderScenario.cpp"
)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Ui/SmatchetAiAssistantUi.cpp")
    list(APPEND _SMATCHET_AI_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/Ui/SmatchetAiAssistantUi.cpp")
endif()
foreach(_smatchet_ai_src IN LISTS _SMATCHET_AI_SOURCES)
    list(REMOVE_ITEM CORE_SOURCES "${_smatchet_ai_src}")
endforeach()
if(SMATCHET_WITH_AI)
    list(APPEND CORE_SOURCES ${_SMATCHET_AI_SOURCES})
endif()


add_library(SmatchetCoreInterface INTERFACE)
target_include_directories(SmatchetCoreInterface INTERFACE
        Source/Core/include
        # Subsystem subdirs from the source-core-dir-reorg precursor
        # (docs/design/source-core-dir-reorg.md): headers moved into these
        # dirs but #includes stay bare (`#include "JiraClient.h"`), so each
        # subdir must be on the include path. Keeps zero #include rewrites.
        Source/Core/include/Tracker
        Source/Core/include/Sync
        Source/Core/include/Persistence
        Source/Core/include/Config
        Source/Core/include/Ui
        # log-a-bug-github — new Core subsystems (bare #includes in their TUs).
        Source/Core/include/Diagnostics
        Source/Core/include/Privacy
        Source/Core/include/Imaging
        Source/Core/ThirdParty/ImGuiColorTextEdit
        # Vendored Font Awesome 6 icon-name header (zlib licence) from
        # github.com/juliettef/IconFontCppHeaders. Header-only — defines
        # ICON_FA_* string macros (UTF-8 PUA codepoints) for every FA6
        # icon. Phase 4 of ai-chat-claude-desktop-parity. The companion
        # TTF (fa-solid-900.ttf) ships separately at assets/fonts/ and
        # is wired through the SmatchetStandalone POST_BUILD copy below.
        Source/Core/ThirdParty/IconsFontAwesome6
        ${sqlitecpp_SOURCE_DIR}/include
)
# Source-tree fallback path for asset lookups (Phase 4 of
# ai-chat-claude-desktop-parity). SmatchetApplyImGuiFont resolves the FA TTF
# in exe-dir first (POST_BUILD-copied at install) then falls back here for
# dev-tree runs / tests / out-of-build invocations. Define is INTERFACE on
# SmatchetCoreInterface so Standalone + DX12 both pick it up.
target_compile_definitions(SmatchetCoreInterface INTERFACE
        "SMATCHET_ASSETS_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\"")
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
# Issue #1133 — propagate the texture-fault-injection macro to every TU that links
# SmatchetCoreInterface (the shared CORE_SOURCES, hence both SmatchetStandalone and
# SmatchetCore_DX12). The injector TU/header compile to no-ops without it. Hard-guard
# against the macro ever reaching a ship build: an LTO publish build must NOT carry the
# synthetic fault surface. Scoped to NOT DEFINED ENV{CI} per cmake-local-gate-ci-scope.
if(SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR SMATCHET_ENABLE_LTO)
        message(FATAL_ERROR
            "SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION must stay OFF in ship/publish builds "
            "(Release/LTO). It is a test-only synthetic fault surface (Issue #1133).")
    endif()
    target_compile_definitions(SmatchetCoreInterface INTERFACE SMATCHET_ENABLE_TEXTURE_FAULT_INJECTION=1)
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
# includes Source/Core headers must link the matching `Smatchet*Shim` libraries
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
            "Plugin '${_target}' includes Source/Core headers but does not link SmatchetCoreAiShim. "
            "Add `target_link_libraries(${_target} PUBLIC SmatchetCoreAiShim)` inside an "
            "`if(SMATCHET_WITH_AI)` guard. See docs/adr/0002-plugin-shim-link-discipline.md.")
    endif()
    if(SMATCHET_WITH_MCP AND NOT "SmatchetCoreMcpShim" IN_LIST _libs)
        message(FATAL_ERROR
            "Plugin '${_target}' includes Source/Core headers but does not link SmatchetCoreMcpShim. "
            "Add `target_link_libraries(${_target} PUBLIC SmatchetCoreMcpShim)` inside an "
            "`if(SMATCHET_WITH_MCP)` guard. See docs/adr/0002-plugin-shim-link-discipline.md.")
    endif()
endfunction()

function(smatchet_configure_core_impl_target _target)
    target_link_libraries("${_target}" PUBLIC SmatchetCoreInterface)
    target_include_directories("${_target}" PRIVATE Source/Core/src
            # Subsystem subdirs from source-core-dir-reorg — bare #includes
            # resolve against these roots so no #include rewrites are needed.
            Source/Core/src/Tracker
            Source/Core/src/Sync
            Source/Core/src/Persistence
            Source/Core/src/Config
            Source/Core/src/Ui
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/ThirdParty")
    if(SMATCHET_WITH_MCP)
        target_include_directories("${_target}" PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Plugins/Mcp")
    endif()
    if(SMATCHET_WITH_WHISPER)
        # SmatchetWhisperSetupBanner.cpp (compiled into CORE_SOURCES on the ON
        # path) includes ModelDownloader.h / ModelCatalog.h / WhisperConsentGate.h
        # which live in the Source/Plugins/Whisper/ subtree.
        target_include_directories("${_target}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source/Plugins/Whisper")
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
    # Vendored ImGuiColorTextEdit TU — built in its own OBJECT lib (no /W4) to avoid the
    # MSVC D9025 /W4-vs-/W0 collision. Linking an OBJECT library pulls in its objects.
    target_link_libraries("${_target}" PRIVATE SmatchetTextEdit)
    if(SMATCHET_WITH_MCP)
        target_link_libraries("${_target}" PUBLIC SmatchetCoreMcpShim)
    endif()
    if(SMATCHET_WITH_AI)
        # Phase B: standalone target gets the side-panel + worker controller. DX12 (Unreal)
        # builds compile the same Source/Core/ TUs with SMATCHET_WITH_AI undefined so the
        # ai-assistant codepaths drop out via #if defined gates.
        target_link_libraries("${_target}" PUBLIC SmatchetCoreAiShim)
    endif()
    if(WIN32)
        # WIC (windowscodecs/ole32) is used to decode attachment bitmaps into RGBA32 for ImGui's
        # backend-agnostic texture API (ImTextureData) when previewing image attachments.
        target_link_libraries("${_target}" PRIVATE shell32 windowscodecs ole32)
        # NOMINMAX/WIN32_LEAN_AND_MEAN live in SmatchetPch.h when PCH is on; publish presets
        # set SMATCHET_USE_PCH=OFF, so command-line defines precede every include (same as DX12).
        target_compile_definitions("${_target}" PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_definitions("${_target}" PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

# Android sibling of smatchet_configure_opengl_core_impl_target — same engine-agnostic
# Source/Core wiring, but ImGuiLib_Mobile (android + GLES3 backends, no glfw) instead
# of ImGuiLib + glfw, and none of the Windows-only WIC / shell32 / win32 codepaths.
# MCP / AI shims are intentionally absent: those features are OFF in the Phase-0 mobile
# preset, so the shim targets are not even declared. This is the Slice-1/2 fork.
function(smatchet_configure_mobile_core_impl_target _target)
    smatchet_configure_core_impl_target("${_target}")
    target_link_libraries("${_target}" PRIVATE ImGuiLib_Mobile)
    # Vendored ImGuiColorTextEdit TU — same OBJECT lib as desktop; on Android it
    # inherits ImGuiLib_Mobile's ImGui config (see the SmatchetTextEdit block above).
    target_link_libraries("${_target}" PRIVATE SmatchetTextEdit)
endfunction()

function(smatchet_configure_dx12_core_impl_target _target)
    smatchet_configure_core_impl_target("${_target}")
    # Vendored ImGuiColorTextEdit TU — DX12-world OBJECT lib (no /W4; matching ImGui config).
    target_link_libraries("${_target}" PRIVATE SmatchetTextEdit_DX12)
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
    if(WIN32)
        # NOMINMAX/WIN32_LEAN_AND_MEAN are defined in SmatchetPch.h, but the DX12 lib does
        # not reliably get the PCH ahead of every transitive <windows.h>, so the min/max
        # macros leak and break bare std::max/std::min (C2589). A command-line define
        # precedes every include in every TU and bulletproofs the whole target at once.
        # Unreal itself also defines NOMINMAX; matching that here avoids divergence. CMake
        # de-dups identical definitions, and the values match Unreal's, so no redefinition
        # warning results.
        target_compile_definitions("${_target}" PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        # ConfigManager (and other TU) still use CRT APIs MSVC marks deprecated (e.g. getenv → C4996).
        target_compile_definitions("${_target}" PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

if(WIN32)
    # EXCLUDE_FROM_ALL: the DX12 lib is consumed by the Unreal plugin only.
    # Excluding it from the default ALL target halves incremental compile time
    # for SmatchetStandalone iteration (each Source/Core/src/*.cpp would otherwise
    # be built twice — once into libSmatchetCore_DX12.a, once into the exe).
    # The Unreal-packaging build presets explicitly target
    # `SmatchetPackageUnrealLibs_DX12` so they're unaffected.
    add_library(SmatchetCore_DX12 STATIC EXCLUDE_FROM_ALL ${CORE_SOURCES})
    smatchet_configure_dx12_core_impl_target(SmatchetCore_DX12)
    smatchet_apply_strict_warnings(SmatchetCore_DX12)
endif()

# ==============================================================================
# 3a. SMATCHET MOBILE SHARED LIBRARY (Android, Phase 0)
# ==============================================================================
# Third sibling of SmatchetStandalone (GLFW/GL exe) and SmatchetCore_DX12 (Unreal
# STATIC lib): a SHARED .so over the SAME engine-agnostic CORE_SOURCES, compiled by
# the NDK toolchain through the non-_WIN32 (#else) side of the platform #ifdefs.
#
# Slice 3b deliverable is the working host shell: Source/Mobile/Android/*.cpp (android_main,
# the EGL/GLES context, the JNI IME bridge, the APK asset/density helpers) compiled into the
# .so alongside CORE_SOURCES, plus the NDK android_native_app_glue static lib that provides
# the ANativeActivity_onCreate entry point and the android_app event pump. With the host in
# place every symbol CORE_SOURCES needs is now defined, so the Slice-2 -Wl,-z,undefs escape
# is dropped in favour of -u ANativeActivity_onCreate (force-keep the glue entry point so the
# linker does not garbage-collect it out of the static archive).
if(ANDROID)
    file(GLOB MOBILE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Source/Mobile/Android/*.cpp")
    add_library(SmatchetMobile SHARED ${CORE_SOURCES} ${MOBILE_SOURCES})
    smatchet_configure_mobile_core_impl_target(SmatchetMobile)
    smatchet_apply_strict_warnings(SmatchetMobile)

    # android_native_app_glue: NDK-sources static lib that turns the NativeActivity callbacks
    # into the android_app event loop android_main() consumes. Built from the active NDK's
    # source tree; its include dir is propagated PUBLIC so android_main.cpp resolves the header.
    if(DEFINED ANDROID_NDK)
        set(_smatchet_ndk_root "${ANDROID_NDK}")
    else()
        set(_smatchet_ndk_root "${CMAKE_ANDROID_NDK}")
    endif()
    set(_smatchet_glue_dir "${_smatchet_ndk_root}/sources/android/native_app_glue")
    add_library(native_app_glue STATIC "${_smatchet_glue_dir}/android_native_app_glue.c")
    target_include_directories(native_app_glue PUBLIC "${_smatchet_glue_dir}")

    # Android system libs for the GLES3 + EGL render path and platform logging, plus the glue.
    target_link_libraries(SmatchetMobile PRIVATE native_app_glue log android EGL GLESv3)
    # Force-keep the glue's ANativeActivity_onCreate (the .so entry point dlsym'd by the
    # framework) — without -u the linker prunes it as unreferenced from the static archive.
    target_link_options(SmatchetMobile PRIVATE -u ANativeActivity_onCreate)
    # 16 KB page-size compatibility (Android 15+: Pixel/devices boot with 16 KB pages and
    # reject .so files whose LOAD segments are 4 KB-aligned). NDK r27+ defaults to 16 KB;
    # NDK 26 still emits 0x1000 alignment, so force the ELF max-page-size to 16384. AGP
    # 8.5.1+ separately zip-aligns the stored uncompressed .so to a 16 KB boundary.
    target_link_options(SmatchetMobile PRIVATE
        "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
endif()

# ==============================================================================
# 3a-check. POSIX CORE COMPILE GATE (Slice 0b — host Linux clang, compile-only)
# ==============================================================================
# Cheapest exercise of every `#if defined(_WIN32)` `#else` branch in CORE_SOURCES:
# compile the whole engine-agnostic core with host Linux clang BEFORE the NDK
# toolchain adds ABI/sysroot variables. A STATIC archive only compiles its objects
# (never links), so this carries zero GLFW/GL/X11 link surface; the imgui *headers*
# are still needed (Core Ui TUs #include "imgui.h") but Core makes no raw gl* calls.
# cpr/TLS resolve against the host's system OpenSSL here (no NDK cross-TLS yet). Reuses
# the shared core-impl configure-fn + adds the imgui include set the per-world imgui
# lib would otherwise supply. No strict-warnings: 0b is a portability gate, not a
# warning-policy gate (the Android warning surface is triaged in a later slice).
if(SMATCHET_BUILD_POSIX_CORE_CHECK)
    add_library(SmatchetCore_PosixCheck STATIC EXCLUDE_FROM_ALL ${CORE_SOURCES})
    smatchet_configure_core_impl_target(SmatchetCore_PosixCheck)
    target_include_directories(SmatchetCore_PosixCheck PRIVATE
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
            ${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include)
    target_compile_definitions(SmatchetCore_PosixCheck PRIVATE IMGUI_USER_CONFIG="SmatchetImConfig.h")

    # linear-tracker-backend.md — credential-gated LIVE end-to-end smoke for the
    # Linear backend, driven by .github/workflows/linear-live-smoke.yml (nightly +
    # manual dispatch, gated on the LINEAR_TEST_API_KEY secret). EXCLUDE_FROM_ALL:
    # never part of the default/all target, so it can NEVER affect the gating
    # build — only the live workflow builds + runs it. Links the core archive
    # (which PUBLIC-links cpr/json/SQLiteCpp via SmatchetCoreInterface); glfw rides
    # the core link interface but no window is ever opened by the smoke.
    add_executable(SmatchetLinearLiveSmoke EXCLUDE_FROM_ALL
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/live/linear_live_smoke.cpp")
    target_link_libraries(SmatchetLinearLiveSmoke PRIVATE SmatchetCore_PosixCheck)
    target_include_directories(SmatchetLinearLiveSmoke PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include/Tracker"
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include/Config")
endif()

# ==============================================================================
# 3b. STATIC PLUGINS (same IPlugin contract as future DLLs)
# ==============================================================================
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

# Whisper dictation plugin — entire subtree is CMake-conditional (gating layer 6).
# DX12 / Unreal builds keep SMATCHET_WITH_WHISPER=OFF and never see this file.
if(SMATCHET_WITH_WHISPER)
    add_subdirectory(Source/Plugins/Whisper)
endif()

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

# ==============================================================================
# 4. SMATCHET STANDALONE EXECUTABLE
# ==============================================================================
# Desktop-only: this whole section builds the GLFW/OpenGL executable and links the
# glfw target, which is not fetched on Android (see the GLFW fetch guard above). On
# Android the SmatchetMobile SHARED lib (section 3a) is the app target instead. The
# guard is `NOT ANDROID` (not `NOT ANDROID AND NOT IOS`) because iOS is a later phase;
# ANDROID is falsy on every desktop build, so this is a no-op for Windows/macOS/Linux.
if(NOT ANDROID)
file(GLOB STANDALONE_SOURCES "Source/Standalone/*.cpp")

if(WIN32)
    set(_SMATCHET_ICO_DEST "${CMAKE_CURRENT_SOURCE_DIR}/Source/Standalone/smatchet.ico")
    if(EXISTS "${_SMATCHET_ICO_DEST}")
        # Generate a `.rc` file with an embedded absolute icon path.
        file(TO_CMAKE_PATH "${_SMATCHET_ICO_DEST}" _SMATCHET_ICO_PATH_CMAKE)
        set(SMATCHET_ICO_PATH "${_SMATCHET_ICO_PATH_CMAKE}")

        set(_SMATCHET_RC_IN "${CMAKE_CURRENT_SOURCE_DIR}/Source/Standalone/smatchet.rc.in")
        set(_SMATCHET_RC_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/smatchet.rc")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
        configure_file("${_SMATCHET_RC_IN}" "${_SMATCHET_RC_OUT}" @ONLY)
        list(APPEND STANDALONE_SOURCES "${_SMATCHET_RC_OUT}")
    else()
        message(WARNING "Smatchet icon not found at ${_SMATCHET_ICO_DEST}; building without embedded icon.")
    endif()
endif()

add_executable(SmatchetStandalone ${STANDALONE_SOURCES} ${CORE_SOURCES})
set_target_properties(SmatchetStandalone PROPERTIES OUTPUT_NAME "${SMATCHET_STANDALONE_OUTPUT_NAME}")
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

# Font Awesome 6 Solid TTF — Phase 4 of ai-chat-claude-desktop-parity.
# The TTF is NOT committed to git (430 KB binary, SIL OFL 1.1 — see
# assets/fonts/README.md for drop-in instructions). Configure-time gate:
# when the file exists, copy it next to the exe so the shipped Standalone
# build finds it via the exe-dir resolver. When absent, log a STATUS
# message — the runtime gracefully falls back to text labels via the
# g_FaIconsLoaded flag in SmatchetImGuiFonts.cpp, so a missing TTF does
# NOT fail the build, just degrades the UI to text-mode icons.
set(_SMATCHET_FA_TTF_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/fa-solid-900.ttf")
if(EXISTS "${_SMATCHET_FA_TTF_SRC}")
    add_custom_command(TARGET SmatchetStandalone POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_SMATCHET_FA_TTF_SRC}"
                    "$<TARGET_FILE_DIR:SmatchetStandalone>/fa-solid-900.ttf"
            COMMENT "Copy fa-solid-900.ttf next to SmatchetStandalone"
            VERBATIM)
else()
    message(STATUS "FA Solid TTF not found at ${_SMATCHET_FA_TTF_SRC}; "
                   "AI chat hover row will fall back to text labels at runtime. "
                   "See assets/fonts/README.md for drop-in instructions.")
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
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindingsCore.cpp"
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
    # Native DX12 renderer (default on Windows+MSVC) — Dx12Bootstrap + the imgui
    # DX12 backend need the D3D12 / DXGI runtime libs and d3dcompiler (the backend
    # compiles its embedded shaders at runtime via D3DCompile). MSVC-only: MinGW
    # is GL-only here (no DX12 backend compiled into ImGuiLib).
    if(MSVC)
        target_link_libraries(SmatchetStandalone PRIVATE d3d12 dxgi d3dcompiler)
    endif()
    # MinGW release exe otherwise depends on libgcc/libstdc++ DLLs; wrong DLL on PATH → 0xC0000139
    # (entry point not found) before main — no logs, "release doesn't run".
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(SMATCHET_WINDOWS_FULLY_STATIC_RUNTIME)
            target_link_options(SmatchetStandalone PRIVATE -static -static-libgcc -static-libstdc++)
        else()
            target_link_options(SmatchetStandalone PRIVATE -static-libgcc -static-libstdc++)
            # Still links a handful of MinGW runtime DLLs; copy them next to
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
# stays unaffected. Header lives at Source/Core/include/SmatchetPch.h and
# pulls in the four heaviest things in the compile graph (imgui.h, json,
# windows.h, common STL containers).
if(SMATCHET_USE_PCH)
    target_precompile_headers(SmatchetStandalone PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include/SmatchetPch.h>")

    # SmatchetCore_DX12 (Unreal-embedded DX12 lib) was PCH-less historically, yet
    # ~64% of its ~225 TUs pull the heavy nlohmann/json.hpp. A json-ONLY PCH
    # (SmatchetPchCoreDx12.h — deliberately no imgui.h / windows.h / GLFW so the
    # DX12 backend defs and the no-glfw-in-core-headers gate stay intact)
    # amortizes that parse across the target. Scoped separately from the
    # Standalone PCH above because the two targets use different ImGui backends.
    if(TARGET SmatchetCore_DX12)
        target_precompile_headers(SmatchetCore_DX12 PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/include/SmatchetPchCoreDx12.h>")
    endif()
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

# Slice 7 of autonomous-debugging-no-creds — wire SMATCHET_AGENT_DEBUG into
# the Standalone target when ON so the Logger.cpp bridge compiles. DX12 stays
# OFF unless explicitly opted in (header lives under tests/_debug/, kept out
# of the Unreal build path). The macro itself expands to ((void)0) when this
# define is absent — verified by V7 perf gate (no regression vs pre-slice-7).
if(SMATCHET_AGENT_DEBUG)
    target_compile_definitions(SmatchetStandalone PRIVATE SMATCHET_AGENT_DEBUG=1)
    target_include_directories(SmatchetStandalone PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/_debug")
endif()

endif() # NOT ANDROID — desktop SmatchetStandalone (GLFW/GL) target + its config tail

endif() # SMATCHET_BUILD_APP — GUI/app surface (GLFW/ImGui/Lua/whisper/Standalone/DX12)

# Pure-logic doctest rig for Source/Core helpers. Opt-in via SMATCHET_BUILD_TESTS.
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
