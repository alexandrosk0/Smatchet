// ui_test_skip.h — shared environment-skip predicate for bucket-E ImGui Test
// Engine TUs.
//
// The CI bucket-E lane runs headless under Mesa llvmpipe software GL
// (GALLIUM_DRIVER=llvmpipe, LIBGL_ALWAYS_SOFTWARE=1). A small set of tests is
// render/timing-dependent in ways that fail deterministically ONLY in that
// environment (docked-tab activation, input-routing timing) while passing on
// every real-GL desktop run. Those tests skip-with-log under the software-GL
// env so per-test verdicts can gate the lane (testing-surface roadmap,
// Slice B residual a). Each skip site names this rationale; a skipped test
// still runs everywhere else, including local head-ful bucket-E runs.
#pragma once

#if defined(SMATCHET_BUILD_UI_TESTS)

#include <cstdlib>

// True when running under the headless software-GL CI lane. Keyed on the env
// the lane exports; a local run that sets LIBGL_ALWAYS_SOFTWARE=1 reproduces
// the lane's skip set exactly.
inline bool SmatchetUiTestIsHeadlessSoftwareGl() {
    const char* v = std::getenv("LIBGL_ALWAYS_SOFTWARE");
    return v != nullptr && v[0] == '1';
}

#endif // SMATCHET_BUILD_UI_TESTS
