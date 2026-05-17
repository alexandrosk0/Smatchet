// GoldenImage.h — header-only PPM (P6) reader + per-channel L∞ diff helper
// for Phase 7 bucket-C screenshot-diff verification.
//
// PPM-P6 format (the writer Target_Standalone/main.cpp uses for
// debug.window.screenshot output):
//   "P6\n<w> <h>\n255\n" + raw RGB bytes, row-major, top-left origin.
//
// API:
//   smatchet::test::PpmImage img;
//   std::string err;
//   if (!smatchet::test::LoadPpm("a.ppm", img, err)) { /* missing/corrupt */ }
//   int linf = smatchet::test::DiffMaxLInf(a, b);   // -1 on dim mismatch
//
// Pure C++14, no third-party deps. Used by tests/support/
// ScreenshotDiffMain.cpp (compiled by scripts/dev/test-screenshot-diff.sh).
//
// Why header-only: the bash driver g++-compiles a tiny one-TU CLI helper that
// includes this header; we don't want a CMake side-effect for a single
// throwaway binary. The doctest rig (SmatchetTests) does not link this helper
// today.

#ifndef SMATCHET_TESTS_SUPPORT_GOLDEN_IMAGE_H
#define SMATCHET_TESTS_SUPPORT_GOLDEN_IMAGE_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace smatchet {
namespace test {

struct PpmImage {
    int                        Width  = 0;
    int                        Height = 0;
    std::vector<unsigned char> Rgb;       // size = Width * Height * 3, row-major, top-left.
};

inline bool LoadPpm(const std::string& path, PpmImage& out, std::string& err) {
    out = PpmImage();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open: " + path;
        return false;
    }
    // Header parser walks chars one at a time so it handles whitespace and
    // comments (# ...\n) the way most PPM writers emit. Target_Standalone's
    // writer uses no comments; we tolerate them for forward-compat with
    // anything else that writes goldens (ImageMagick, GIMP).
    auto readToken = [&](std::string& tok) -> bool {
        tok.clear();
        int c = 0;
        // Skip whitespace + comment lines.
        for (;;) {
            c = std::fgetc(f);
            if (c == EOF) return false;
            if (c == '#') {
                while (c != '\n' && c != EOF) c = std::fgetc(f);
                continue;
            }
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        }
        // Accumulate non-whitespace.
        while (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != EOF) {
            tok.push_back(static_cast<char>(c));
            c = std::fgetc(f);
        }
        return !tok.empty();
    };

    std::string magic;
    std::string ws, hs, maxs;
    if (!readToken(magic) || magic != "P6") {
        std::fclose(f);
        err = "not a P6 PPM: " + path;
        return false;
    }
    if (!readToken(ws) || !readToken(hs) || !readToken(maxs)) {
        std::fclose(f);
        err = "truncated header: " + path;
        return false;
    }
    const long w    = std::strtol(ws.c_str(), nullptr, 10);
    const long h    = std::strtol(hs.c_str(), nullptr, 10);
    const long maxv = std::strtol(maxs.c_str(), nullptr, 10);
    if (w <= 0 || h <= 0 || maxv != 255) {
        std::fclose(f);
        err = "unsupported PPM dims/maxval: " + path;
        return false;
    }
    // P6 spec: exactly one whitespace char between maxval and binary payload.
    // readToken consumed the trailing whitespace already.
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 3u;
    out.Width  = static_cast<int>(w);
    out.Height = static_cast<int>(h);
    out.Rgb.resize(n);
    const size_t got = std::fread(out.Rgb.data(), 1, n, f);
    std::fclose(f);
    if (got != n) {
        err = "short read of pixel data: " + path;
        return false;
    }
    return true;
}

/// Per-channel L∞ between two same-dimensioned PPMs. Returns the maximum
/// absolute |a-b| across every R/G/B byte, or -1 if dimensions disagree
/// (which is itself a fail — the caller should treat it as the worst).
inline int DiffMaxLInf(const PpmImage& a, const PpmImage& b) {
    if (a.Width != b.Width || a.Height != b.Height) {
        return -1;
    }
    int worst = 0;
    const size_t n = a.Rgb.size();
    for (size_t i = 0; i < n; ++i) {
        const int diff = std::abs(static_cast<int>(a.Rgb[i]) - static_cast<int>(b.Rgb[i]));
        if (diff > worst) worst = diff;
    }
    return worst;
}

/// Count pixels matching a sentinel RGB within a per-channel tolerance.
/// Used by the dock-gap-sentinel diff to catch leaked magenta clear-color
/// pixels (any > 0 fails the run when pink-clear mode is enabled).
inline std::size_t CountPixels(const PpmImage& img, unsigned char r, unsigned char g, unsigned char b,
                               int tol = 0) {
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
