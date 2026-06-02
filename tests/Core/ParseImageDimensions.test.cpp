// Pure-logic coverage of the byte-level image-dimension parser extracted from
// SmatchetAttachmentPreviewUi.cpp into ImageDimensionsPure.cpp (function-size
// compliance PR A9-A10). These cases parse *untrusted* header bytes, so they
// pin every signature match, bounds-check, byte-offset, and endianness read
// against the behaviour the production monolith shipped.
//
// Goldens are the current (pre-extraction) behaviour: the helper is a mechanical
// byte-for-byte extraction, so each expectation below mirrors exactly what the
// original inline parser returned for the same input.

#include "ImageDimensionsPure.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::image_dim::ParsedImageInfo;
using smatchet::image_dim::ParseImageDimensionsFromBytes;

namespace {

std::vector<unsigned char> Bytes(std::initializer_list<int> vals) {
    std::vector<unsigned char> out;
    out.reserve(vals.size());
    for (int v : vals) {
        out.push_back(static_cast<unsigned char>(v & 0xFF));
    }
    return out;
}

// Pad a header to at least `n` bytes with zeros so the size-gate passes without
// changing the signature region.
std::vector<unsigned char> Pad(std::vector<unsigned char> b, std::size_t n) {
    if (b.size() < n) {
        b.resize(n, 0u);
    }
    return b;
}

const std::string kPng = "image/png";
const std::string kJpeg = "image/jpeg";
const std::string kGif = "image/gif";
const std::string kWebp = "image/webp";

} // namespace

TEST_CASE("ParseImageDimensions: empty buffer → empty-file error") {
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(std::vector<unsigned char>(), kPng);
    CHECK_FALSE(r.Ok);
    CHECK(r.Error == "Downloaded attachment file is empty.");
}

TEST_CASE("ParseImageDimensions: valid PNG → IHDR width/height (big-endian @16,20)") {
    // 8-byte signature, then IHDR length+type (bytes 8..15), then BE width @16, height @20.
    std::vector<unsigned char> b =
        Bytes({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R'});
    // width = 0x00000280 = 640 @ offset 16
    b.push_back(0x00);
    b.push_back(0x00);
    b.push_back(0x02);
    b.push_back(0x80);
    // height = 0x000001E0 = 480 @ offset 20
    b.push_back(0x00);
    b.push_back(0x00);
    b.push_back(0x01);
    b.push_back(0xE0);
    REQUIRE(b.size() >= 24);
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kPng);
    CHECK(r.Ok);
    CHECK(r.Width == 640);
    CHECK(r.Height == 480);
}

TEST_CASE("ParseImageDimensions: PNG with zero dimension → invalid error") {
    std::vector<unsigned char> b =
        Bytes({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 0, // width 0
               0,    0,    1,    0}); // height 256
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kPng);
    CHECK_FALSE(r.Ok);
    CHECK(r.Error == "PNG dimensions are invalid.");
}

TEST_CASE("ParseImageDimensions: truncated PNG (signature only) → not parseable") {
    // 8-byte signature but < 24 bytes → size gate fails, falls through to MIME message.
    const std::vector<unsigned char> b = Bytes({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kPng);
    CHECK_FALSE(r.Ok);
    CHECK(r.Error == "Image format detected but dimensions could not be parsed by the in-app preview.");
}

TEST_CASE("ParseImageDimensions: valid GIF89a → little-endian width/height @6,8") {
    // "GIF89a" then LE width @6 (0x0140 = 320), LE height @8 (0x00F0 = 240).
    std::vector<unsigned char> b = Bytes({'G', 'I', 'F', '8', '9', 'a', 0x40, 0x01, 0xF0, 0x00});
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kGif);
    CHECK(r.Ok);
    CHECK(r.Width == 320);
    CHECK(r.Height == 240);
}

TEST_CASE("ParseImageDimensions: GIF87a variant also accepted") {
    std::vector<unsigned char> b = Bytes({'G', 'I', 'F', '8', '7', 'a', 0x10, 0x00, 0x20, 0x00});
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kGif);
    CHECK(r.Ok);
    CHECK(r.Width == 16);
    CHECK(r.Height == 32);
}

TEST_CASE("ParseImageDimensions: GIF with zero dimension → invalid error") {
    std::vector<unsigned char> b = Bytes({'G', 'I', 'F', '8', '9', 'a', 0x00, 0x00, 0x20, 0x00});
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kGif);
    CHECK_FALSE(r.Ok);
    CHECK(r.Error == "GIF dimensions are invalid.");
}

TEST_CASE("ParseImageDimensions: WEBP VP8X → 24-bit LE (width-1)@24 (height-1)@27, +1") {
    // RIFF....WEBP VP8X header; dims are 24-bit LE minus-one at offsets 24/27.
    std::vector<unsigned char> b = Bytes({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', 'X'});
    b = Pad(b, 30);
    // widthMinusOne = 0x00027F (639) @24 → width 640
    b[24] = 0x7F;
    b[25] = 0x02;
    b[26] = 0x00;
    // heightMinusOne = 0x0001DF (479) @27 → height 480
    b[27] = 0xDF;
    b[28] = 0x01;
    b[29] = 0x00;
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kWebp);
    CHECK(r.Ok);
    CHECK(r.Width == 640);
    CHECK(r.Height == 480);
}

TEST_CASE("ParseImageDimensions: WEBP non-VP8X (no fixed dims) → falls through to MIME message") {
    // RIFF/WEBP signature matches but the chunk is VP8 (lossy), not VP8X → original
    // code does not finalise here; falls through to the generic degradation message.
    std::vector<unsigned char> b = Bytes({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '});
    b = Pad(b, 30);
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kWebp);
    CHECK_FALSE(r.Ok);
    CHECK(r.Error == "Image format detected but dimensions could not be parsed by the in-app preview.");
}

TEST_CASE("ParseImageDimensions: valid JPEG SOF0 → height@(i+3) width@(i+5), big-endian") {
    // FFD8 (SOI), then FFC0 (SOF0) marker, segment length 0x0011 (17), precision byte,
    // then height (BE) and width (BE).
    std::vector<unsigned char> b =
        Bytes({0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, 0x01, 0x90, // height = 0x0190 = 400 @ i+3..i+4
               0x02, 0x8A});                                         // width  = 0x028A = 650 @ i+5..i+6
    // Pad to satisfy the `i + 9 < size` loop guard plus the segment-length bound.
    b = Pad(b, 32);
    // Restore segment length high byte clobbered by nothing (already set); ensure
    // segmentLength(17) + i(4) <= size — size is 32, fine.
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kJpeg);
    CHECK(r.Ok);
    CHECK(r.Height == 400);
    CHECK(r.Width == 650);
}

TEST_CASE("ParseImageDimensions: JPEG SOI only / no SOF → falls through to MIME message") {
    std::vector<unsigned char> b = Bytes({0xFF, 0xD8, 0xFF, 0xD9}); // SOI + EOI, no SOF
    b = Pad(b, 16);
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kJpeg);
    CHECK_FALSE(r.Ok);
    CHECK(r.Error == "Image format detected but dimensions could not be parsed by the in-app preview.");
}

TEST_CASE("ParseImageDimensions: garbage bytes with supported MIME → 'could not be parsed'") {
    std::vector<unsigned char> b = Bytes({0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33});
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, kPng);
    CHECK_FALSE(r.Ok);
    CHECK(r.Error == "Image format detected but dimensions could not be parsed by the in-app preview.");
}

TEST_CASE("ParseImageDimensions: garbage bytes with UNsupported MIME → 'not a supported image format'") {
    std::vector<unsigned char> b = Bytes({0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33});
    const ParsedImageInfo r = ParseImageDimensionsFromBytes(b, "application/pdf");
    CHECK_FALSE(r.Ok);
    CHECK(r.Error == "Attachment is not a supported image format for in-app preview.");
}
