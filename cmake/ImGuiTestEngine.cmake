# ImGuiTestEngine.cmake — wire ocornut/imgui_test_engine into the Smatchet build.
#
# Included from the root CMakeLists.txt only when SMATCHET_BUILD_UI_TESTS is ON.
# Upstream has no CMakeLists.txt (see ocornut/imgui_test_engine#86), so we
# hand-roll a static library against the 7 source files we actually consume.
# Capture / PNG / GIF / ffmpeg are intentionally skipped — disabled via
# IMGUI_TEST_ENGINE_ENABLE_CAPTURE=0 in Source/Core/include/SmatchetImConfig.h.

include(FetchContent)

smatchet_fetch_status("Fetching imgui_test_engine...")
FetchContent_Declare(imgui_test_engine
    GIT_REPOSITORY https://github.com/ocornut/imgui_test_engine.git
    # Pinned to HEAD on `main` at the time the bucket-E plan was probed for
    # C++14 compat. See docs/design/imgui-test-engine-bucket-e-execution.md
    # § Phase 0 for the probe transcript.
    GIT_TAG 8568767ad4c53d6ce02d65f01a09d30fb630bd80
)
FetchContent_GetProperties(imgui_test_engine)
if(NOT imgui_test_engine_POPULATED)
    FetchContent_Populate(imgui_test_engine)
endif()

set(_SMATCHET_TE_SRC_DIR "${imgui_test_engine_SOURCE_DIR}/imgui_test_engine")
set(_SMATCHET_TE_SOURCES
    "${_SMATCHET_TE_SRC_DIR}/imgui_te_engine.cpp"
    "${_SMATCHET_TE_SRC_DIR}/imgui_te_context.cpp"
    "${_SMATCHET_TE_SRC_DIR}/imgui_te_coroutine.cpp"
    "${_SMATCHET_TE_SRC_DIR}/imgui_te_perftool.cpp"
    "${_SMATCHET_TE_SRC_DIR}/imgui_te_ui.cpp"
    "${_SMATCHET_TE_SRC_DIR}/imgui_te_utils.cpp"
    "${_SMATCHET_TE_SRC_DIR}/imgui_te_exporters.cpp"
    # imgui_capture_tool.cpp must compile even when we never actually capture —
    # imgui_te_engine.cpp calls ImGuiCaptureContext::* unconditionally (no
    # #if guards around the call sites). Keeping CAPTURE=1 makes the symbol
    # definitions available; the dormant runtime path is harmless when we
    # leave ImGuiTestEngineIO::ScreenCaptureFunc unset.
    "${_SMATCHET_TE_SRC_DIR}/imgui_capture_tool.cpp"
)

# OpenGL/GLFW variant — linked into SmatchetStandalone.
add_library(SmatchetImGuiTestEngine STATIC ${_SMATCHET_TE_SOURCES})
target_include_directories(SmatchetImGuiTestEngine PUBLIC
    "${imgui_test_engine_SOURCE_DIR}"
    "${_SMATCHET_TE_SRC_DIR}"
)
target_link_libraries(SmatchetImGuiTestEngine PUBLIC ImGuiLib)
# Test-engine TUs need to see SMATCHET_BUILD_UI_TESTS so SmatchetImConfig.h
# enables IMGUI_ENABLE_TEST_ENGINE. PUBLIC propagates to anything linking
# against this lib (UiTestScenario, tests/ui/*).
target_compile_definitions(SmatchetImGuiTestEngine PUBLIC SMATCHET_BUILD_UI_TESTS=1)
# Engine sources reference IMGUI_ENABLE_TEST_ENGINE directly in a couple of
# spots — defining it on the engine target itself avoids a header-order dance.
target_compile_definitions(SmatchetImGuiTestEngine PRIVATE IMGUI_ENABLE_TEST_ENGINE=1)
# Engine is third-party; silence its warnings under our strict-warning regime.
if(MSVC)
    target_compile_options(SmatchetImGuiTestEngine PRIVATE /W0)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(SmatchetImGuiTestEngine PRIVATE -w)
endif()

# DX12 stub variant — EXCLUDE_FROM_ALL, dead-code-linked into SmatchetCore_DX12
# only as a compile tripwire to catch include-cascade regressions (someone
# accidentally pulling GLFW/OpenGL into Source/Core via the engine glue).
if(WIN32)
    add_library(SmatchetImGuiTestEngine_DX12 STATIC EXCLUDE_FROM_ALL ${_SMATCHET_TE_SOURCES})
    target_include_directories(SmatchetImGuiTestEngine_DX12 PUBLIC
        "${imgui_test_engine_SOURCE_DIR}"
        "${_SMATCHET_TE_SRC_DIR}"
    )
    target_link_libraries(SmatchetImGuiTestEngine_DX12 PUBLIC ImGuiLib_DX12)
    target_compile_definitions(SmatchetImGuiTestEngine_DX12 PUBLIC SMATCHET_BUILD_UI_TESTS=1)
    target_compile_definitions(SmatchetImGuiTestEngine_DX12 PRIVATE IMGUI_ENABLE_TEST_ENGINE=1)
    if(MSVC)
        target_compile_options(SmatchetImGuiTestEngine_DX12 PRIVATE /W0)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(SmatchetImGuiTestEngine_DX12 PRIVATE -w)
    endif()
endif()

unset(_SMATCHET_TE_SRC_DIR)
unset(_SMATCHET_TE_SOURCES)
