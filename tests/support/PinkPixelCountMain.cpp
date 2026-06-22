// PinkPixelCountMain.cpp — tiny CLI used by scripts/dev/test-screenshot-diff.sh
// for the pink-clear dock-gap scan (pink-clear-dock-gap-scan).
//
// Counts pixels in a captured PNG matching a sentinel RGB within a per-channel
// tolerance via smatchet::test::CountPixels. The DockGapSentinelScenario arms a
// transient magenta (255,0,255) clear-color during its warm-up frames, so any
// viewport pixel the dock layout leaves uncovered renders magenta. A zero count
// means the dockspace fully covers the viewport (no dock gap leak); any non-zero
// count is a real gap.
//
// The bash script g++-compiles this TU on the fly with `-ISource/Core/ThirdParty`
// so stb_image resolves; it is not wired into CMake so the production build stays
// untouched (same pattern as ScreenshotDiffMain.cpp).
//
// Usage:
//   pink_pixel_count <captured.png> <r> <g> <b> <tolerance> <maxAllowed>
//   pink_pixel_count --help
//
// Output (stdout):
//   pink=<N> w=<w> h=<h>
//
// Exit codes:
//   0 — pink count <= maxAllowed (no dock gap leak)
//   1 — pink count >  maxAllowed (dock gap detected) OR load failed (stderr carries the reason)

#include "GoldenImage.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s <captured.png> <r> <g> <b> <tolerance> <maxAllowed>\n"
                 "  r g b      sentinel color channels (dock-gap sentinel: 255 0 255)\n"
                 "  tolerance  per-channel match tolerance (typical: 8)\n"
                 "  maxAllowed maximum tolerated matching pixels (dock-gap gate: 0)\n"
                 "exit codes:\n"
                 "  0  pink count <= maxAllowed (no dock gap leak)\n"
                 "  1  pink count >  maxAllowed (dock gap) or load failed\n"
                 "example:\n"
                 "  %s capture.png 255 0 255 8 0\n",
                 argv0, argv0);
}

int ClampByte(long v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return static_cast<int>(v);
}

} // namespace

int main(int argc, char** argv) {
    const char* argv0 = argc > 0 ? argv[0] : "pink_pixel_count";
    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        PrintUsage(argv0);
        return 0;
    }
    if (argc != 7) {
        PrintUsage(argv0);
        return 1;
    }
    const std::string captured = argv[1];
    const int r = ClampByte(std::strtol(argv[2], nullptr, 10));
    const int g = ClampByte(std::strtol(argv[3], nullptr, 10));
    const int b = ClampByte(std::strtol(argv[4], nullptr, 10));
    const int tol = static_cast<int>(std::strtol(argv[5], nullptr, 10));
    const long maxAllowed = std::strtol(argv[6], nullptr, 10);

    smatchet::test::GoldenImage img;
    std::string err;
    if (!smatchet::test::LoadImage(captured, img, err)) {
        std::fprintf(stderr, "pink_pixel_count: load failed: %s\n", err.c_str());
        return 1;
    }
    const std::size_t hits = smatchet::test::CountPixels(img, static_cast<unsigned char>(r),
                                                         static_cast<unsigned char>(g),
                                                         static_cast<unsigned char>(b), tol);
    std::fprintf(stdout, "pink=%zu w=%d h=%d\n", hits, img.Width, img.Height);
    if (static_cast<long>(hits) > maxAllowed) {
        std::fprintf(stderr, "pink_pixel_count: %zu sentinel pixels > maxAllowed %ld — dock gap leak\n", hits,
                     maxAllowed);
        return 1;
    }
    return 0;
}
