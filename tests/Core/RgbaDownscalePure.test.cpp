// RgbaDownscalePure.test.cpp — P1.4 cross-platform attachment-thumbnail decode (mobile).
//
// Unit-tests the pure area-average RGBA32 downscale that the non-Windows thumbnail decode arm of
// SmatchetAttachmentPreviewUi uses after stb_image's full-resolution decode (stb cannot decode-scale
// the way WIC does on Windows). Pure / ImGui-free / renderer-free, so it runs on the Windows doctest
// rig. Pins: the longest-side fit math, the already-fits / maxDim<=0 passthrough, box averaging
// (not point sampling), aspect-ratio preservation, and the bad-input guards.

#include "RgbaDownscalePure.h"

#include "doctest/doctest.h"

#include <vector>

using smatchet::image_scale::DownscaleRgba32;
using smatchet::image_scale::FitWithinLongestSide;

namespace {

// Build a solid-colour WxH RGBA32 buffer (every pixel == {r,g,b,a}).
std::vector<unsigned char> SolidRgba(int w, int h, unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    for (std::size_t i = 0; i < px.size(); i += 4) {
        px[i + 0] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = a;
    }
    return px;
}

} // namespace

TEST_CASE("FitWithinLongestSide: scales landscape to the longest-side budget, preserving aspect") {
    int w = 0;
    int h = 0;
    FitWithinLongestSide(4000, 3000, 2048, w, h);
    // 4000 is the longest side -> 2048; 3000 * 2048 / 4000 = 1536 (exact).
    CHECK(w == 2048);
    CHECK(h == 1536);
}

TEST_CASE("FitWithinLongestSide: portrait clamps the height to the budget") {
    int w = 0;
    int h = 0;
    FitWithinLongestSide(3000, 4000, 2048, w, h);
    CHECK(w == 1536);
    CHECK(h == 2048);
}

TEST_CASE("FitWithinLongestSide: already-within-budget passes through unchanged") {
    int w = 0;
    int h = 0;
    FitWithinLongestSide(640, 480, 2048, w, h);
    CHECK(w == 640);
    CHECK(h == 480);
}

TEST_CASE("FitWithinLongestSide: maxDimension<=0 or bad dims pass through") {
    int w = 0;
    int h = 0;
    FitWithinLongestSide(4000, 3000, 0, w, h);
    CHECK(w == 4000);
    CHECK(h == 3000);
    FitWithinLongestSide(4000, 3000, -1, w, h);
    CHECK(w == 4000);
    CHECK(h == 3000);
}

TEST_CASE("FitWithinLongestSide: extreme aspect ratio clamps the minor side to >=1") {
    int w = 0;
    int h = 0;
    // 5000x1 down to longest 2048: minor side 1 * 2048 / 5000 floors to 0 -> clamped to 1.
    FitWithinLongestSide(5000, 1, 2048, w, h);
    CHECK(w == 2048);
    CHECK(h == 1);
}

TEST_CASE("DownscaleRgba32: already within budget copies the source through unchanged") {
    const std::vector<unsigned char> src = SolidRgba(4, 4, 10, 20, 30, 40);
    std::vector<unsigned char> out;
    int ow = 0;
    int oh = 0;
    CHECK(DownscaleRgba32(src, 4, 4, 8, out, ow, oh));
    CHECK(ow == 4);
    CHECK(oh == 4);
    CHECK(out == src);
}

TEST_CASE("DownscaleRgba32: maxDimension<=0 is a passthrough") {
    const std::vector<unsigned char> src = SolidRgba(3, 2, 1, 2, 3, 4);
    std::vector<unsigned char> out;
    int ow = 0;
    int oh = 0;
    CHECK(DownscaleRgba32(src, 3, 2, 0, out, ow, oh));
    CHECK(ow == 3);
    CHECK(oh == 2);
    CHECK(out == src);
}

TEST_CASE("DownscaleRgba32: 2x2 -> 1x1 averages all four source pixels per channel") {
    // Four distinct pixels chosen so each channel's mean is exact (sum divisible by 4).
    std::vector<unsigned char> src(2 * 2 * 4);
    // (0,0)
    src[0] = 0;
    src[1] = 40;
    src[2] = 80;
    src[3] = 0;
    // (1,0)
    src[4] = 20;
    src[5] = 60;
    src[6] = 100;
    src[7] = 40;
    // (0,1)
    src[8] = 40;
    src[9] = 80;
    src[10] = 120;
    src[11] = 80;
    // (1,1)
    src[12] = 60;
    src[13] = 100;
    src[14] = 140;
    src[15] = 120;
    std::vector<unsigned char> out;
    int ow = 0;
    int oh = 0;
    CHECK(DownscaleRgba32(src, 2, 2, 1, out, ow, oh));
    CHECK(ow == 1);
    CHECK(oh == 1);
    REQUIRE(out.size() == 4u);
    CHECK(static_cast<int>(out[0]) == 30);  // (0+20+40+60)/4
    CHECK(static_cast<int>(out[1]) == 70);  // (40+60+80+100)/4
    CHECK(static_cast<int>(out[2]) == 110); // (80+100+120+140)/4
    CHECK(static_cast<int>(out[3]) == 60);  // (0+40+80+120)/4
}

TEST_CASE("DownscaleRgba32: 4x2 -> 2x1 averages each 2x2 source block (aspect preserved)") {
    // Left half solid (10,10,10,255), right half solid (200,200,200,255). longest=4 -> 2x1.
    // dst[0] covers columns 0..1 (all left), dst[1] covers columns 2..3 (all right); each box is
    // 2 wide x 2 tall, so the average is the block's solid colour.
    std::vector<unsigned char> src(4 * 2 * 4);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            const std::size_t p = (static_cast<std::size_t>(y) * 4u + static_cast<std::size_t>(x)) * 4u;
            const unsigned char v = (x < 2) ? 10 : 200;
            src[p + 0] = v;
            src[p + 1] = v;
            src[p + 2] = v;
            src[p + 3] = 255;
        }
    }
    std::vector<unsigned char> out;
    int ow = 0;
    int oh = 0;
    CHECK(DownscaleRgba32(src, 4, 2, 2, out, ow, oh));
    CHECK(ow == 2);
    CHECK(oh == 1);
    REQUIRE(out.size() == 8u);
    CHECK(static_cast<int>(out[0]) == 10); // left block
    CHECK(static_cast<int>(out[3]) == 255);
    CHECK(static_cast<int>(out[4]) == 200); // right block
    CHECK(static_cast<int>(out[7]) == 255);
}

TEST_CASE("DownscaleRgba32: rejects non-positive dimensions") {
    const std::vector<unsigned char> src = SolidRgba(2, 2, 1, 1, 1, 1);
    std::vector<unsigned char> out;
    int ow = -1;
    int oh = -1;
    CHECK_FALSE(DownscaleRgba32(src, 0, 2, 1, out, ow, oh));
    CHECK(out.empty());
    CHECK(ow == 0);
    CHECK(oh == 0);
    CHECK_FALSE(DownscaleRgba32(src, 2, -1, 1, out, ow, oh));
    CHECK(out.empty());
}

TEST_CASE("DownscaleRgba32: rejects a source buffer smaller than srcW*srcH*4") {
    std::vector<unsigned char> tooSmall(2 * 2 * 4 - 1); // one byte short of a 2x2 RGBA image
    std::vector<unsigned char> out;
    int ow = -1;
    int oh = -1;
    CHECK_FALSE(DownscaleRgba32(tooSmall, 2, 2, 1, out, ow, oh));
    CHECK(out.empty());
    CHECK(ow == 0);
    CHECK(oh == 0);
}
