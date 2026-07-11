# Source/Standalone — desktop GLFW/OpenGL executable (SmatchetStandalone).
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
# Defines (NOT ANDROID): the SmatchetStandalone executable over
# STANDALONE_SOURCES + CORE_SOURCES, plus its full configuration tail — icon/.rc,
# Scripts junction/copy, Font Awesome TTF copy, MinGW -mcmodel/big-obj fixes,
# LTO/IPO (sets SMATCHET_IPO_ENABLED; also applied to the DX12 targets here),
# PCH, Release strip, link job pool, sanitizer hookup, ImGui Test Engine
# (bucket E) wiring + tests/ui, and the SMATCHET_AGENT_DEBUG define.

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
            # god-file-splits: the sol2-heavy registrar TUs carved out of AppController_LuaBindings.cpp
            # expand the same template surface, so give them the identical -mcmodel=large + no-PCH
            # treatment under BFD ld to avoid the R_X86_64_32S relocation-overflow at link.
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Ui.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Ai.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Tickets.cpp"
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
