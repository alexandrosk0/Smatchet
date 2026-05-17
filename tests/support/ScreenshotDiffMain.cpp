// screenshot_diff_main.cpp — tiny CLI used by scripts/dev/test-screenshot-
// diff.sh. Computes the per-channel L∞ between two PPMs and exits 0 (within
// tolerance) or 1 (above tolerance / dim mismatch / load failure). The bash
// script g++-compiles this TU on the fly; we don't wire it into CMake so the
// production build stays untouched.
//
// Usage:
//   screenshot_diff <captured.ppm> <golden.ppm> <tolerance>
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
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <captured.ppm> <golden.ppm> <tolerance>\n",
                     argc > 0 ? argv[0] : "screenshot_diff");
        return 1;
    }
    const std::string captured = argv[1];
    const std::string golden   = argv[2];
    const int tol = static_cast<int>(std::strtol(argv[3], nullptr, 10));

    smatchet::test::PpmImage a, b;
    std::string err;
    if (!smatchet::test::LoadPpm(captured, a, err)) {
        std::fprintf(stderr, "screenshot_diff: load failed (captured): %s\n", err.c_str());
        return 1;
    }
    if (!smatchet::test::LoadPpm(golden, b, err)) {
        std::fprintf(stderr, "screenshot_diff: load failed (golden): %s\n", err.c_str());
        return 1;
    }
    const int linf = smatchet::test::DiffMaxLInf(a, b);
    std::fprintf(stdout, "linf=%d w=%d h=%d\n", linf, a.Width, a.Height);
    if (linf < 0) {
        std::fprintf(stderr, "screenshot_diff: dimension mismatch captured=%dx%d golden=%dx%d\n",
                     a.Width, a.Height, b.Width, b.Height);
        return 1;
    }
    if (linf > tol) {
        std::fprintf(stderr, "screenshot_diff: L∞ %d > tolerance %d\n", linf, tol);
        return 1;
    }
    return 0;
}
