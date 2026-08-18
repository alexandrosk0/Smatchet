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

# ------------------------------------------------------------------------------
# clang-cl + Ninja: turn OFF cmcldeps-based RC dependency scanning (#2029)
#
# On Windows/Ninja, CMake's Windows-MSVC.cmake sets CMAKE_NINJA_CMCLDEPS_RC=1,
# which wraps every "Building RC object" step in cmcldeps.exe. cmcldeps runs a
# C PREPROCESSOR over the .rc to harvest /showIncludes dependencies, and CMake
# hands it CMAKE_C_COMPILER for that job — then appends the argument tail meant
# for the *resource* compiler, `/fo <out>` included. `/fo` is an rc.exe spelling:
# cl.exe parses options case-insensitively and quietly accepts it as /Fo, which
# is why ninja-iter-msvc builds, but clang-cl does not and treats `/fo` as a
# missing input file:
#     clang-cl: error: no such file or directory: '/fo'
# That killed every ninja-*-clang preset at curl's libcurl.rc, so the Clang half
# of the "must compile on MSVC + Clang" rule could only be exercised in CI.
#
# CMake already disables cmcldeps for the llvm-rc toolchain (Windows-Clang.cmake
# does exactly `set(CMAKE_NINJA_CMCLDEPS_RC 0)`), but that branch is keyed on the
# RC compiler being llvm-rc; we use the Windows SDK rc.exe, so it never fires.
# Take the same supported knob explicitly for clang-cl. Both Windows-MSVC.cmake
# and Windows-Clang.cmake guard their assignment on NOT DEFINED / their own
# branch, so a value set here (before project(), and cached) wins.
#
# Cost: .rc files lose header-dependency tracking under the clang presets — a
# touched header included by a .rc will not by itself retrigger the resource
# compile. That is the same trade CMake itself takes for llvm-rc, and it is
# scoped to the affected toolchain: nothing about the MSVC presets changes, and
# the resource compiler is still the same rc.exe producing the same .res.
if(CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_NINJA_CMCLDEPS_RC
   AND (CMAKE_C_COMPILER MATCHES "clang-cl" OR CMAKE_CXX_COMPILER MATCHES "clang-cl"))
    set(CMAKE_NINJA_CMCLDEPS_RC 0 CACHE STRING
        "Disable cmcldeps RC dep-scanning under clang-cl (#2029: cmcldeps feeds rc.exe's /fo to the preprocessor)")
    message(STATUS "Smatchet: clang-cl + Ninja detected — CMAKE_NINJA_CMCLDEPS_RC=0 (see CMakeLists.txt, Issue #2029)")
endif()

project(SmatchetApp VERSION 0.7.0 LANGUAGES C CXX)
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
# and the build wrapper (scripts/dev/with-msvc-env.sh) passes it
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
            "(on Windows, run it from Git Bash)\n"
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
include(cmake/SmatchetDepManifest.cmake)
include(cmake/SmatchetFontAssets.cmake)

# ---------------------------------------------------------------------------
# Help > About build-info codegen.
#
# Emits build/generated/include/SmatchetBuildInfo.h — git SHA/branch/dirty,
# toolchain, and the third-party credits manifest. Included by EXACTLY ONE TU
# (Source/Core/src/Diagnostics/AboutInfo.cpp); never packaged into the Unreal
# plugin (a copy there would go stale against the packaged .lib — the exact
# drift the About dialog exists to make impossible).
#
# Runs at BUILD time, not just configure time: a plain configure_file would
# freeze the SHA at configure time and then lie after the next commit. The
# generator script writes through `copy_if_different`, so an unchanged SHA never
# moves the header's mtime and the always-run target costs no recompiles.
# ---------------------------------------------------------------------------
find_package(Git QUIET)

set(SMATCHET_BUILDINFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
set(SMATCHET_BUILDINFO_HEADER "${SMATCHET_BUILDINFO_DIR}/SmatchetBuildInfo.h")
file(MAKE_DIRECTORY "${SMATCHET_BUILDINFO_DIR}")

smatchet_collect_dep_manifest(SMATCHET_DEP_MANIFEST)
smatchet_check_dep_manifest("${SMATCHET_DEP_MANIFEST}" "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
smatchet_build_dep_literals("${SMATCHET_DEP_MANIFEST}" SMATCHET_DEPS_LITERALS)

# The static (configure-time-known) half is handed to the -P script through a
# generated .cmake file rather than -D flags: SMATCHET_DEPS_LITERALS is
# multi-line and full of quotes and backslashes, which a command line mangles.
set(_smatchet_buildinfo_static "${CMAKE_CURRENT_BINARY_DIR}/generated/SmatchetBuildInfoStatic.cmake")
file(WRITE "${_smatchet_buildinfo_static}"
"# AUTO-GENERATED at configure time by CMakeLists.txt. Do not edit.\n"
"set(SMATCHET_CXX_COMPILER_ID \"${CMAKE_CXX_COMPILER_ID}\")\n"
"set(SMATCHET_CXX_COMPILER_VERSION \"${CMAKE_CXX_COMPILER_VERSION}\")\n"
"set(SMATCHET_DEPS_LITERALS [==[${SMATCHET_DEPS_LITERALS}]==])\n")

set(_smatchet_buildinfo_script "${CMAKE_CURRENT_SOURCE_DIR}/cmake/SmatchetGenerateBuildInfo.cmake")
set(_smatchet_buildinfo_args
        "-DSMATCHET_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/SmatchetBuildInfo.h.in"
        "-DSMATCHET_OUT=${SMATCHET_BUILDINFO_HEADER}"
        "-DSMATCHET_STATIC_VARS=${_smatchet_buildinfo_static}"
        "-DSMATCHET_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}")

# Configure-time seed: the header must exist before the first compile for
# clangd/IDE indexing, and for EXCLUDE_FROM_ALL targets built without `ALL`
# having run (SmatchetCore_DX12, SmatchetCore_PosixCheck).
execute_process(COMMAND "${CMAKE_COMMAND}" ${_smatchet_buildinfo_args} -P "${_smatchet_buildinfo_script}")

# Build-time refresh: always runs, but copy_if_different means no rebuild storm.
add_custom_target(SmatchetBuildInfoGen ALL
        COMMAND "${CMAKE_COMMAND}" ${_smatchet_buildinfo_args} -P "${_smatchet_buildinfo_script}"
        BYPRODUCTS "${SMATCHET_BUILDINFO_HEADER}"
        COMMENT "Refreshing SmatchetBuildInfo.h (git provenance)"
        VERBATIM)

# ---------------------------------------------------------------------------
# Help > About app-icon codegen.
#
# Emits build/generated/include/SmatchetIconBytes.h — the bytes of the Win32
# resource icon, so the About dialog can draw the real app icon with no runtime
# file lookup (there is no meaningful icon path inside the Unreal plugin) and no
# second copy of the artwork to drift. Included by EXACTLY ONE TU
# (Source/Core/src/Ui/SmatchetAboutIcon.cpp); never packaged into the plugin.
#
# Configure time only, unlike SmatchetBuildInfo.h: the icon changes on a human
# edit rather than on every commit, and CMAKE_CONFIGURE_DEPENDS re-runs configure
# when it does. A missing icon is a WARNING, never a FATAL_ERROR
# (cmake-local-gate-ci-scope) — the About dialog falls back to a glyph.
# ---------------------------------------------------------------------------
set(_smatchet_icon_src "${CMAKE_CURRENT_SOURCE_DIR}/Source/Standalone/smatchet.ico")
set(SMATCHET_ICON_HEADER "${SMATCHET_BUILDINFO_DIR}/SmatchetIconBytes.h")
if(EXISTS "${_smatchet_icon_src}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_smatchet_icon_src}")
    file(READ "${_smatchet_icon_src}" _smatchet_icon_hex HEX)
    string(LENGTH "${_smatchet_icon_hex}" _smatchet_icon_nibbles)
    math(EXPR SMATCHET_ICON_BYTE_COUNT "${_smatchet_icon_nibbles} / 2")
    # Hex blob -> C initializer, then a line break every 16 bytes so the generated
    # header stays readable (and diffable) instead of one multi-KB line.
    string(REGEX REPLACE "(..)" "0x\\1," SMATCHET_ICON_BYTES "${_smatchet_icon_hex}")
    string(REGEX REPLACE "((0x..,){16})" "\\1\n    " SMATCHET_ICON_BYTES "${SMATCHET_ICON_BYTES}")
    set(SMATCHET_ICON_AVAILABLE 1)
else()
    message(WARNING "Smatchet icon not found at ${_smatchet_icon_src}; About dialog falls back to a glyph")
    set(SMATCHET_ICON_AVAILABLE 0)
    set(SMATCHET_ICON_BYTE_COUNT 0)
    set(SMATCHET_ICON_BYTES "")
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SmatchetIconBytes.h.in" "${SMATCHET_ICON_HEADER}" @ONLY)

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
# which also builds md4c-html and the md2html CLI we don't need. See docs/plans/rich-text-editing-v2-remaining.md.
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
# COMPONENT INCLUDES — per-component CMakeLists.txt files, pulled in via
# include() (deliberately NOT add_subdirectory): every component file executes
# in THIS directory scope, so target/variable/source-property semantics, object
# paths, and artifact locations are byte-identical to the former monolithic
# root file (pure mechanical split; see each component file's header).
# Shared state crossing component boundaries (defined in Source/Core, consumed
# by Mobile/Standalone/UnrealPlugins): CORE_SOURCES, SmatchetCoreInterface, the
# SmatchetCore{Mcp,Ai}Shim libs, and the smatchet_configure_*_core_impl_target()
# helpers. Order matters — Core first, Standalone last.
# ==============================================================================

# Engine-agnostic core: CORE_SOURCES, SmatchetCoreInterface + shims, the
# core-impl configure helpers, SmatchetCore_DX12, and the POSIX compile gate.
include(Source/Core/CMakeLists.txt)

# Android SHARED lib over CORE_SOURCES (no-op on desktop configures).
include(Source/Mobile/CMakeLists.txt)

# ==============================================================================
# STATIC PLUGINS (same IPlugin contract as future DLLs)
# ==============================================================================
include(Source/Plugins/Mcp/CMakeLists.txt)
include(Source/Plugins/LuaConsole/CMakeLists.txt)

# Whisper dictation plugin — entire subtree is CMake-conditional (gating layer 6).
# DX12 / Unreal builds keep SMATCHET_WITH_WHISPER=OFF and never see this file.
# (True add_subdirectory — Source/Plugins/Whisper/CMakeLists.txt predates the
# component split and owns its own directory scope.)
if(SMATCHET_WITH_WHISPER)
    add_subdirectory(Source/Plugins/Whisper)
endif()

# Unreal/DX12 embedding surface: DX12 plugin variants, SmatchetImGuiHost_DX12,
# and the SmatchetPackageUnrealLibs_DX12 packaging target (WIN32 only).
include(Source/UnrealPlugins/CMakeLists.txt)

# Desktop GLFW/OpenGL executable + its full configuration tail (NOT ANDROID).
include(Source/Standalone/CMakeLists.txt)


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

# ---------------------------------------------------------------------------
# SmatchetBuildInfoGen ordering edges. Deliberately at the very end of the file,
# after every target (incl. the tests subdirectories) has been defined.
#
# The `ALL` on the custom target already covers a plain `cmake --build .`. These
# explicit edges cover the targets that `ALL` does NOT reach — SmatchetCore_DX12
# and SmatchetCore_PosixCheck are EXCLUDE_FROM_ALL, so `--target SmatchetCore_DX12`
# would otherwise race the generator.
#
# The edges attach to the concrete targets, NOT to SmatchetCoreInterface:
# add_dependencies() on an INTERFACE library does not propagate to consumers.
# ---------------------------------------------------------------------------
foreach(_smatchet_bi_consumer
        SmatchetStandalone
        SmatchetCore_DX12
        SmatchetMobile
        SmatchetCore_PosixCheck
        SmatchetTests)
    if(TARGET ${_smatchet_bi_consumer})
        add_dependencies(${_smatchet_bi_consumer} SmatchetBuildInfoGen)
    endif()
endforeach()
