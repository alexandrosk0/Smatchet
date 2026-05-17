// GoldenImage.h — header-only PNG reader + per-channel L∞ diff helper for
// Phase 7 bucket-C screenshot-diff verification.
//
// Migrated from PPM-P6 to PNG (via stb_image) to cut tests/golden/* repo bloat
// (raw P6 is ~6 bytes/pixel, PNG is ~1-2 bytes/pixel post-compression for typical
// UI captures).
//
// API:
//   smatchet::test::GoldenImage img;
//   std::string err;
//   if (!smatchet::test::LoadImage("a.png", img, err)) { /* missing/corrupt */ }
//   int linf = smatchet::test::DiffMaxLInf(a, b);   // -1 on dim mismatch
//   // Back-compat aliases:
//   smatchet::test::LoadPpm("a.png", img, err);   // calls LoadImage; name kept
//   smatchet::test::PpmImage = GoldenImage;        // typedef
//
// Pure C++14, depends on stb_image (vendored at ThirdParty/stb/stb_image.h).
// stb_image's PNG decoder rejects oversized dims via STBI_MAX_DIMENSIONS (1<<24
// by default — we cap further at 16384x16384 for defense-in-depth against
// crafted PNGs).
//
// Why header-only: the bash driver g++-compiles a tiny one-TU CLI helper that
// includes this header; we don't want a CMake side-effect for a single
// throwaway binary. The doctest rig (SmatchetTests) does not link this helper
// today.

#ifndef SMATCHET_TESTS_SUPPORT_GOLDEN_IMAGE_H
#define SMATCHET_TESTS_SUPPORT_GOLDEN_IMAGE_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// stb_image needs ONE TU per linkable to define the implementation. The CLI
// helper (tests/support/ScreenshotDiffMain.cpp) is single-TU so we define
// here. If this header is ever pulled into a multi-TU target, lift the
// implementation out via a sibling .cpp.
//
// Use an angle-bracket include so cppcheck (run by the lint hook without a
// -I flag) treats stb_image.h as a system header and skips scanning into
// third-party code. The bash driver (scripts/dev/test-screenshot-diff.sh)
// must pass `-IThirdParty` so g++ resolves it at compile time.
#ifndef SMATCHET_GOLDEN_IMAGE_NO_STB_IMPL
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#endif
#include <stb/stb_image.h>

namespace smatchet {
namespace test {

struct GoldenImage {
    int Width = 0;
    int Height = 0;
    std::vector<unsigned char> Rgb; // size = Width * Height * 3, row-major, top-left.
};

// Defense-in-depth cap on accepted image dimensions. 16384x16384 is far beyond
// any realistic UI capture; reject larger to bound memory + decode cost.
constexpr int kMaxGoldenImageDim = 16384;

inline bool LoadImage(const std::string& path, GoldenImage& out, std::string& err) {
    out = GoldenImage();
    int w = 0, h = 0, channelsInFile = 0;
    // Request 3 channels (drop alpha if present). stb_image returns RGB row-major
    // top-left, matching our prior PPM-P6 byte layout.
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channelsInFile, 3);
    if (!data) {
        const char* reason = stbi_failure_reason();
        err = std::string("stb_image load failed: ") + (reason ? reason : "unknown") + " (" + path + ")";
        return false;
    }
    if (w <= 0 || h <= 0 || w > kMaxGoldenImageDim || h > kMaxGoldenImageDim) {
        stbi_image_free(data);
        err = "rejected dimensions: " + std::to_string(w) + "x" + std::to_string(h) + " (" + path + ")";
        return false;
    }
    out.Width = w;
    out.Height = h;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 3u;
    out.Rgb.assign(data, data + n);
    stbi_image_free(data);
    return true;
}

// Back-compat alias for the prior PPM-only name.
inline bool LoadPpm(const std::string& path, GoldenImage& out, std::string& err) { return LoadImage(path, out, err); }

// Back-compat type alias.
using PpmImage = GoldenImage;

/// Per-channel L∞ between two same-dimensioned images. Returns the maximum
/// absolute |a-b| across every R/G/B byte, or -1 if dimensions disagree
/// (which is itself a fail — the caller should treat it as the worst).
inline int DiffMaxLInf(const GoldenImage& a, const GoldenImage& b) {
    if (a.Width != b.Width || a.Height != b.Height) {
        return -1;
    }
    int worst = 0;
    const size_t n = a.Rgb.size();
    for (size_t i = 0; i < n; ++i) {
        const int diff = std::abs(static_cast<int>(a.Rgb[i]) - static_cast<int>(b.Rgb[i]));
        if (diff > worst)
            worst = diff;
    }
    return worst;
}

/// Count pixels matching a sentinel RGB within a per-channel tolerance.
/// Used by the dock-gap-sentinel diff to catch leaked magenta clear-color
/// pixels (any > 0 fails the run when pink-clear mode is enabled).
inline std::size_t CountPixels(const GoldenImage& img, unsigned char r, unsigned char g, unsigned char b, int tol = 0) {
    std::size_t hits = 0;
    const size_t n = img.Rgb.size();
    for (size_t i = 0; i + 2 < n; i += 3) {
        if (std::abs(static_cast<int>(img.Rgb[i + 0]) - static_cast<int>(r)) <= tol &&
            std::abs(static_cast<int>(img.Rgb[i + 1]) - static_cast<int>(g)) <= tol &&
            std::abs(static_cast<int>(img.Rgb[i + 2]) - static_cast<int>(b)) <= tol) {
            ++hits;
        }
    }
    return hits;
}

} // namespace test
} // namespace smatchet

#endif // SMATCHET_TESTS_SUPPORT_GOLDEN_IMAGE_H
