# Source/Core — engine-agnostic core (CORE_SOURCES + interface/impl wiring).
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
# Defines: CORE_SOURCES (shared source list consumed by SmatchetStandalone,
# SmatchetCore_DX12, SmatchetMobile, SmatchetCore_PosixCheck), the SmatchetTextEdit
# per-world OBJECT libs, SmatchetCoreInterface + the SmatchetCore{Mcp,Ai}Shim
# INTERFACE libs, the smatchet_configure_*_core_impl_target() helper functions,
# smatchet_assert_plugin_shim_links(), the SmatchetCore_DX12 STATIC lib (WIN32),
# and the SmatchetCore_PosixCheck compile gate + SmatchetLinearLiveSmoke (Linux).

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
# god-file-splits: sol2-dependent companion registrar TUs carved out of AppController_LuaBindings.cpp
# (Ui/Ai/Tickets). Same ON-only gating as the parent — their AppController:: Lua methods are stubbed
# in AppController_LuaStubs.cpp for the OFF build, so they must not enter CORE_SOURCES when Lua is off.
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Ui.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Ai.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Tickets.cpp")
# AppController_LuaBindings_Draw.cpp is sol2-dependent (LuaDrawList + replay glue).
# Its AppController:: methods are stubbed in AppController_LuaStubs.cpp for the OFF build.
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Draw.cpp")
list(REMOVE_ITEM CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaStubs.cpp")
if(SMATCHET_WITH_LUA_AUTOMATION)
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindingsCore.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Ui.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Ai.cpp")
    list(APPEND CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AppController_LuaBindings_Tickets.cpp")
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
    "${CMAKE_CURRENT_SOURCE_DIR}/Source/Core/src/AiRequestBuilder.cpp"
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
        # Generated SmatchetBuildInfo.h (Help > About). One INTERFACE line here
        # means every consumer — Standalone, DX12, Mobile, PosixCheck, tests —
        # inherits the path without a per-target edit.
        ${SMATCHET_BUILDINFO_DIR}
)
# Source-tree fallback path for asset lookups (Phase 4 of
# ai-chat-claude-desktop-parity). SmatchetApplyImGuiFont resolves the FA TTF
# in exe-dir first (POST_BUILD-copied at install) then falls back here for
# dev-tree runs / tests / out-of-build invocations. Define is INTERFACE on
# SmatchetCoreInterface so Standalone + DX12 both pick it up.
target_compile_definitions(SmatchetCoreInterface INTERFACE
        "SMATCHET_ASSETS_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\"")
# Build configuration for the Help > About dialog. MUST be a $<CONFIG> generator
# expression and NOT a value baked into SmatchetBuildInfo.h: CMAKE_BUILD_TYPE is
# EMPTY under the Visual Studio multi-config generator (the primary Windows dev
# path), so a header-baked value would render a blank build type there.
target_compile_definitions(SmatchetCoreInterface INTERFACE
        "SMATCHET_BUILD_CONFIG=\"$<CONFIG>\"")
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
    # Instrument the core archive when a sanitizer is requested (no-op otherwise, so the
    # plain posix-core-check portability gate is unchanged). The nightly command-registry
    # monkey (.github/workflows/monkey-nightly.yml) configures this preset with
    # -DSMATCHET_SANITIZER=asan so ASan+UBSan actually cover the dispatch/validation code
    # it fuzzes. NOTE: any executable that links this archive under a sanitizer must ALSO
    # call smatchet_apply_sanitizers() on itself (to link the runtime) — see the test exes.
    smatchet_apply_sanitizers(SmatchetCore_PosixCheck)
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
