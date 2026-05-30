// screenshot_diff_main.cpp — tiny CLI used by scripts/dev/test-screenshot-
// diff.sh. Computes the per-channel L∞ between two PNGs and exits 0 (within
// tolerance) or 1 (above tolerance / dim mismatch / load failure). The bash
// script g++-compiles this TU on the fly with `-ISource/Core/ThirdParty` so stb_image
// resolves; we don't wire it into CMake so the production build stays
// untouched.
//
// Usage:
//   screenshot_diff <captured.png> <golden.png> <tolerance>
//   screenshot_diff --help            # prints usage + exit-code reference
//
// Output (stdout):
//   linf=<N> w=<w> h=<h>
//
// Exit codes:
//   0 — linf <= tolerance (within budget)
//   1 — linf >  tolerance OR dimensions mismatch OR load failed (stderr carries the reason)

#include "GoldenImage.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <captured.png> <golden.png> <tolerance>\n"
                 "  tolerance  per-channel L_inf cap (typical: 4)\n"
                 "exit codes:\n"
                 "  0  L_inf within tolerance\n"
                 "  1  L_inf > tolerance, dimensions mismatch, or load failed\n"
                 "example:\n"
                 "  %s capture.png tests/golden/dock-gap-sentinel.png 4\n",
                 argv0, argv0);
}

} // namespace

int main(int argc, char** argv) {
    const char* argv0 = argc > 0 ? argv[0] : "screenshot_diff";
    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        PrintUsage(argv0);
        return 0;
    }
    if (argc != 4) {
        PrintUsage(argv0);
        return 1;
    }
    const std::string captured = argv[1];
    const std::string golden = argv[2];
    const int tol = static_cast<int>(std::strtol(argv[3], nullptr, 10));

    smatchet::test::GoldenImage a;
    smatchet::test::GoldenImage b;
    std::string err;
    if (!smatchet::test::LoadImage(captured, a, err)) {
        std::fprintf(stderr, "screenshot_diff: load failed (captured): %s\n", err.c_str());
        return 1;
    }
    if (!smatchet::test::LoadImage(golden, b, err)) {
        std::fprintf(stderr, "screenshot_diff: load failed (golden): %s\n", err.c_str());
        return 1;
    }
    const int linf = smatchet::test::DiffMaxLInf(a, b);
    std::fprintf(stdout, "linf=%d w=%d h=%d\n", linf, a.Width, a.Height);
    if (linf < 0) {
        std::fprintf(stderr, "screenshot_diff: dimension mismatch captured=%dx%d golden=%dx%d\n", a.Width, a.Height,
                     b.Width, b.Height);
        return 1;
    }
    if (linf > tol) {
        std::fprintf(stderr, "screenshot_diff: L_inf %d > tolerance %d\n", linf, tol);
        return 1;
    }
    return 0;
}
