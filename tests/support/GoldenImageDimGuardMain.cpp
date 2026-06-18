// GoldenImageDimGuardMain.cpp — fuzz/regression CLI for the GoldenImage.h
// dimension guard (audit hardening: image alloc-bomb via crafted oversize
// header). Driven by scripts/dev/test-golden-image-dim-guard.sh, which
// g++-compiles this single TU with `-ISource/Core/ThirdParty` so stb_image
// resolves. Not wired into CMake (production build stays untouched).
//
// What it proves:
//   * A crafted PNM whose HEADER declares dims above our 16384 cap (but below
//     stb's STBI_MAX_DIMENSIONS = 1<<24) is rejected from the header alone,
//     BEFORE stbi_load allocates w*h*3 bytes. The tell: LoadImage fails with
//     err "rejected dimensions" rather than "load failed" / "header read
//     failed". If the pre-check were absent, stbi_load would try to allocate
//     the giant raster first (≈30 GB for 100000x100000) and fail/OOM with a
//     "load failed" error — so the err-string distinguishes "rejected before
//     decode" from "decode attempted then failed".
//   * The cap boundary is inclusive (16384 accepted, 16385 rejected).
//   * An honest small image still loads and reports correct dims.
//
// Self-contained: writes its crafted fixtures into a scratch dir (argv[1], or
// $TMPDIR, or "."), runs every case, prints per-case PASS/FAIL, then emits the
// canonical `Passed: N  Failed: M` line the test driver greps.
//
// Exit codes:
//   0 — every case passed
//   1 — at least one case failed

#include "GoldenImage.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(const std::string& label, bool ok, const std::string& detail) {
    if (ok) {
        std::printf("  PASS  %s\n", label.c_str());
        ++g_passed;
    } else {
        std::printf("  FAIL  %s  — %s\n", label.c_str(), detail.c_str());
        ++g_failed;
    }
}

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Write raw bytes to disk; returns false on any I/O error.
bool WriteFile(const std::string& path, const std::vector<unsigned char>& bytes) {
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return static_cast<bool>(f);
}

// Build a binary PNM (P6) blob: "P6\n<w> <h>\n255\n" header + `body` payload.
// The header dims are written verbatim from `declW`/`declH` so we can craft a
// LYING header (declared dims that don't match the body size).
std::vector<unsigned char> MakeP6(long declW, long declH, const std::vector<unsigned char>& body) {
    std::string header = "P6\n" + std::to_string(declW) + " " + std::to_string(declH) + "\n255\n";
    std::vector<unsigned char> blob(header.begin(), header.end());
    blob.insert(blob.end(), body.begin(), body.end());
    return blob;
}

} // namespace

int main(int argc, char** argv) {
    std::string dir;
    if (argc > 1 && argv[1][0] != '\0') {
        dir = argv[1];
    } else if (const char* t = std::getenv("TMPDIR")) {
        dir = t;
    } else {
        dir = ".";
    }
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';

    // --- Case 1: lying oversize header (the alloc bomb) -----------------------
    // Header declares 100000x100000 (≈30 GB raster) but carries a near-empty
    // body. The header pre-check must reject from stbi_info alone, never
    // reaching the stbi_load allocation.
    {
        const std::string path = dir + "dimguard_lying_oversize.ppm";
        std::vector<unsigned char> body(16, 0); // nowhere near 100000*100000*3
        if (!WriteFile(path, MakeP6(100000, 100000, body))) {
            Check("lying-oversize-header rejected pre-decode", false, "could not write fixture " + path);
        } else {
            smatchet::test::GoldenImage img;
            std::string err;
            const bool loaded = smatchet::test::LoadImage(path, img, err);
            const bool ok = !loaded && Contains(err, "rejected dimensions") && !Contains(err, "load failed");
            Check("lying-oversize-header rejected pre-decode (no alloc)", ok,
                  "loaded=" + std::to_string(loaded) + " err=\"" + err + "\"");
            std::remove(path.c_str());
        }
    }

    // --- Case 2: just-over-cap header rejected (boundary, exclusive above) -----
    {
        const std::string path = dir + "dimguard_over_cap.ppm";
        std::vector<unsigned char> body(16, 0);
        if (!WriteFile(path, MakeP6(16385, 16385, body))) {
            Check("over-cap-header (16385) rejected", false, "could not write fixture " + path);
        } else {
            smatchet::test::GoldenImage img;
            std::string err;
            const bool loaded = smatchet::test::LoadImage(path, img, err);
            const bool ok = !loaded && Contains(err, "rejected dimensions") && !Contains(err, "load failed");
            Check("over-cap-header (16385) rejected pre-decode", ok,
                  "loaded=" + std::to_string(loaded) + " err=\"" + err + "\"");
            std::remove(path.c_str());
        }
    }

    // --- Case 3: at-cap-width honest image accepted (boundary, inclusive) ------
    // 16384x1 sits exactly on the cap. Body must be fully present (49152 bytes)
    // so the decode succeeds and the post-decode consistency guard passes.
    {
        const std::string path = dir + "dimguard_at_cap.ppm";
        std::vector<unsigned char> body(static_cast<size_t>(16384) * 1u * 3u, 0x7f);
        if (!WriteFile(path, MakeP6(16384, 1, body))) {
            Check("at-cap-header (16384x1) accepted", false, "could not write fixture " + path);
        } else {
            smatchet::test::GoldenImage img;
            std::string err;
            const bool loaded = smatchet::test::LoadImage(path, img, err);
            const bool ok = loaded && img.Width == 16384 && img.Height == 1;
            Check("at-cap-header (16384x1) accepted", ok,
                  "loaded=" + std::to_string(loaded) + " dims=" + std::to_string(img.Width) + "x" +
                      std::to_string(img.Height) + " err=\"" + err + "\"");
            std::remove(path.c_str());
        }
    }

    // --- Case 4: honest small image accepted, dims reported -------------------
    {
        const std::string path = dir + "dimguard_valid_2x2.ppm";
        std::vector<unsigned char> body = {
            255, 0,   0,   0, 255, 0, // row 0: red, green
            0,   0, 255, 255, 255, 0, // row 1: blue, yellow
        };
        if (!WriteFile(path, MakeP6(2, 2, body))) {
            Check("valid-2x2 accepted", false, "could not write fixture " + path);
        } else {
            smatchet::test::GoldenImage img;
            std::string err;
            const bool loaded = smatchet::test::LoadImage(path, img, err);
            const bool ok = loaded && img.Width == 2 && img.Height == 2 && img.Rgb.size() == 12;
            Check("valid-2x2 accepted with correct dims", ok,
                  "loaded=" + std::to_string(loaded) + " dims=" + std::to_string(img.Width) + "x" +
                      std::to_string(img.Height) + " bytes=" + std::to_string(img.Rgb.size()) + " err=\"" + err + "\"");
            std::remove(path.c_str());
        }
    }

    // --- Case 5: missing file rejected cleanly (no crash) --------------------
    {
        const std::string path = dir + "dimguard_does_not_exist.ppm";
        std::remove(path.c_str()); // ensure absent
        smatchet::test::GoldenImage img;
        std::string err;
        const bool loaded = smatchet::test::LoadImage(path, img, err);
        Check("missing-file rejected without crash", !loaded && !err.empty(),
              "loaded=" + std::to_string(loaded) + " err=\"" + err + "\"");
    }

    std::printf("Passed: %d  Failed: %d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
