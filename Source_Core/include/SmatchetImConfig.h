// SmatchetImConfig.h — single source of ImGui compile-time configuration for Smatchet.
//
// Wired in via `IMGUI_USER_CONFIG="SmatchetImConfig.h"` on every target that
// links ImGuiLib / ImGuiLib_DX12 / SmatchetImGuiTestEngine. When this define is
// set, `imgui.h` includes this header in place of the default `imconfig.h`,
// guaranteeing every TU sees the same ImWchar typedef + test-engine hook macros.
//
// Hard rule: anything that changes the ImGui ABI (ImWchar size, test-engine
// hooks, custom callbacks) belongs here, not as ad-hoc PRIVATE/PUBLIC compile
// definitions sprinkled across targets — that path silently desyncs.

#pragma once

// Supplementary-plane Unicode (emoji in Plane/Jira titles). Must match every TU
// that includes imgui.h. Previously defined PUBLIC on ImGuiLib; consolidated
// here so test-engine TUs (which compile through IMGUI_USER_CONFIG) see it too.
#define IMGUI_USE_WCHAR32 1

// ImGui Test Engine bucket-E hookup. Only enabled when the CMake gate
// SMATCHET_BUILD_UI_TESTS is ON. Default builds (ninja-iter-msvc, ninja-debug-msvc,
// ninja-publish-msvc) leave this OFF — production exe carries zero test surface.
//
// We replicate the relevant fragment of imgui_test_engine/imgui_te_imconfig.h
// instead of `#include`-ing it: pinning ourselves to a specific subset lets us
// disable the capture (PNG/ffmpeg) path which we do not use, and keeps the
// engine sources from leaking imstb_image_write into Source_Core compiles.
#ifdef SMATCHET_BUILD_UI_TESTS

#define IMGUI_ENABLE_TEST_ENGINE

// imgui_internal.h (transitively included by the test-engine headers) hard-
// requires IMGUI_DEFINE_MATH_OPERATORS to have been set BEFORE imgui.h. Since
// every TU reaches imgui.h through this user-config header, defining it here
// lands at exactly the right point — before imgui's math-operator block.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

// Keep the screen-capture machinery compiled — `imgui_te_engine.cpp` references
// `ImGuiCaptureContext::*` symbols unconditionally, so the capture surface must
// link. We do NOT wire a `ScreenCaptureFunc` into `ImGuiTestEngineIO`, so the
// runtime path is dormant and the dependency on ffmpeg / `imstb_image_write.h`
// is header-only (no PNG actually gets written during bucket-E runs).
// Screenshot regression remains bucket C territory (separate surface).
#ifndef IMGUI_TEST_ENGINE_ENABLE_CAPTURE
#define IMGUI_TEST_ENGINE_ENABLE_CAPTURE 1
#endif

// Use std::function for ImGuiTest::TestFunc / GuiFunc so the IM_REGISTER_TEST
// macro can take captured lambdas. The engine defaults to raw function
// pointers; raw pointers would force us to write boilerplate trampolines for
// every test seed.
#ifndef IMGUI_TEST_ENGINE_ENABLE_STD_FUNCTION
#define IMGUI_TEST_ENGINE_ENABLE_STD_FUNCTION 1
#endif

// Default-disabled engine knobs we don't currently use.
#ifndef IMGUI_TEST_ENGINE_ENABLE_IMPLOT
#define IMGUI_TEST_ENGINE_ENABLE_IMPLOT 0
#endif
// CoroutineFuncs default to std::thread. ImGuiTestEngine_Start asserts
// `engine->IO.CoroutineFuncs != nullptr`, and we don't install a custom
// implementation — let the engine wire its built-in std::thread one.
#ifndef IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL
#define IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL 1
#endif

// Game-console gate — Smatchet is desktop-only. Force OFF so the engine uses
// host APIs (system(), popen(), sigaction()) freely on Windows/Linux/macOS.
#ifndef IMGUI_TEST_ENGINE_IS_GAME_CONSOLE
#define IMGUI_TEST_ENGINE_IS_GAME_CONSOLE 0
#endif

// IM_DEBUG_BREAK — the test engine references it from imgui.h before
// imgui_internal.h is reachable. Replicate the upstream definition exactly so
// IM_TEST_ENGINE_ASSERT works at TU compile time.
#ifndef IM_DEBUG_BREAK
#if defined(_MSC_VER)
#define IM_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__)
#define IM_DEBUG_BREAK() __builtin_debugtrap()
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#define IM_DEBUG_BREAK() __asm__ volatile("int $0x03")
#else
#define IM_DEBUG_BREAK() ((void)0)
#endif
#endif

extern void ImGuiTestEngine_AssertLog(const char* expr, const char* file, const char* func, int line);
#define IM_TEST_ENGINE_ASSERT(_EXPR)                                                                                   \
    do {                                                                                                               \
        if ((void)0, !(_EXPR)) {                                                                                       \
            ImGuiTestEngine_AssertLog(#_EXPR, __FILE__, __func__, __LINE__);                                           \
            IM_DEBUG_BREAK();                                                                                          \
        }                                                                                                              \
    } while (0)

#endif // SMATCHET_BUILD_UI_TESTS
