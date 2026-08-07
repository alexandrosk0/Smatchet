// SmatchetIcoDecode doctests — the .ico -> RGBA32 unpacker behind the About dialog's app icon.
//
// stb_image cannot read .ico, so this decoder is hand-written and parses attacker-shaped input
// (well, build-shaped: the bytes are baked in at configure time). The cases below are therefore
// weighted towards truncation and malformed headers, where a missed bound is a read past the end.
//
// Nothing here asserts anything about the REAL Source/Standalone/smatchet.ico: that file is not
// linked into the test rig, and pinning its pixels would make an artwork edit fail the suite.

#include "Persistence/SmatchetIcoDecode.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::icons::DecodeIcoToRgba;

namespace {

void PutU16(std::vector<unsigned char>& out, std::size_t at, unsigned value) {
    out[at] = static_cast<unsigned char>(value & 0xFFu);
    out[at + 1] = static_cast<unsigned char>((value >> 8) & 0xFFu);
}

void PutU32(std::vector<unsigned char>& out, std::size_t at, unsigned value) {
    PutU16(out, at, value & 0xFFFFu);
    PutU16(out, at + 2, (value >> 16) & 0xFFFFu);
}

/// Build a single-entry, uncompressed 32bpp .ico — the exact shape the Win32 resource icon uses.
/// Pixel (x, y) in top-down order gets colour `fill(x, y)` as BGRA, and the AND mask is all-opaque.
struct SyntheticIco {
    std::vector<unsigned char> Bytes;
    std::size_t PixelsAt = 0; // Offset of the BGRA payload (bottom-up rows).
    std::size_t MaskAt = 0;
};

SyntheticIco MakeIco(int width, int height, unsigned char alpha) {
    const std::size_t maskStride = static_cast<std::size_t>(((width + 31) / 32) * 4);
    const std::size_t pixelBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    const std::size_t maskBytes = maskStride * static_cast<std::size_t>(height);
    const std::size_t imageAt = 6 + 16;
    const std::size_t pixelsAt = imageAt + 40;

    SyntheticIco ico;
    ico.Bytes.assign(pixelsAt + pixelBytes + maskBytes, 0);
    ico.PixelsAt = pixelsAt;
    ico.MaskAt = pixelsAt + pixelBytes;

    // ICONDIR: reserved, type = 1 (icon), count = 1.
    PutU16(ico.Bytes, 2, 1);
    PutU16(ico.Bytes, 4, 1);
    // ICONDIRENTRY: width/height bytes are 0 for 256, and advisory anyway — the BITMAPINFOHEADER
    // is authoritative, which is what this decoder reads.
    ico.Bytes[6] = static_cast<unsigned char>(width & 0xFF);
    ico.Bytes[7] = static_cast<unsigned char>(height & 0xFF);
    PutU16(ico.Bytes, 12, 32); // bitCount
    PutU32(ico.Bytes, 6 + 8, static_cast<unsigned>(40 + pixelBytes + maskBytes));
    PutU32(ico.Bytes, 6 + 12, static_cast<unsigned>(imageAt));
    // BITMAPINFOHEADER: size, width, doubled height, planes, bitCount, BI_RGB.
    PutU32(ico.Bytes, imageAt, 40);
    PutU32(ico.Bytes, imageAt + 4, static_cast<unsigned>(width));
    PutU32(ico.Bytes, imageAt + 8, static_cast<unsigned>(height * 2));
    PutU16(ico.Bytes, imageAt + 12, 1);
    PutU16(ico.Bytes, imageAt + 14, 32);
    PutU32(ico.Bytes, imageAt + 16, 0);

    // Bottom-up BGRA. Encode the top-down row index into the red channel so the flip is assertable.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t at =
                pixelsAt + (static_cast<std::size_t>(height - 1 - y) * static_cast<std::size_t>(width) +
                            static_cast<std::size_t>(x)) *
                               4u;
            ico.Bytes[at + 0] = static_cast<unsigned char>(x); // B
            ico.Bytes[at + 1] = 0x40;                          // G
            ico.Bytes[at + 2] = static_cast<unsigned char>(y); // R
            ico.Bytes[at + 3] = alpha;
        }
    }
    return ico;
}

} // namespace

TEST_CASE("DecodeIcoToRgba unpacks a 32bpp entry to top-down RGBA") {
    const SyntheticIco ico = MakeIco(8, 8, 0xFF);
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    std::string error;

    REQUIRE(DecodeIcoToRgba(ico.Bytes.data(), ico.Bytes.size(), rgba, width, height, error));
    CHECK(error.empty());
    CHECK(width == 8);
    CHECK(height == 8);
    REQUIRE(rgba.size() == 8u * 8u * 4u);

    // Row 3, column 5: BGRA -> RGBA channel swap plus the bottom-up -> top-down flip.
    const std::size_t at = (3u * 8u + 5u) * 4u;
    CHECK(static_cast<int>(rgba[at + 0]) == 3);    // R carried the source row
    CHECK(static_cast<int>(rgba[at + 1]) == 0x40); // G
    CHECK(static_cast<int>(rgba[at + 2]) == 5);    // B carried the source column
    CHECK(static_cast<int>(rgba[at + 3]) == 0xFF);
}

TEST_CASE("DecodeIcoToRgba reconstructs alpha from the AND mask when the payload has none") {
    // Pre-XP icons left the alpha channel zeroed and leant on the 1-bit mask. Decoding those
    // literally yields a fully transparent icon, i.e. an invisible About dialog logo.
    SyntheticIco ico = MakeIco(8, 8, 0x00);
    // Mask bit set == transparent. Make the first pixel of the TOP row transparent: mask rows are
    // bottom-up like the pixels, so the top row is the last one.
    const std::size_t maskStride = 4;
    ico.Bytes[ico.MaskAt + maskStride * 7] = 0x80;

    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    std::string error;
    REQUIRE(DecodeIcoToRgba(ico.Bytes.data(), ico.Bytes.size(), rgba, width, height, error));

    CHECK(static_cast<int>(rgba[3]) == 0);                         // (0,0) masked out
    CHECK(static_cast<int>(rgba[(1u * 8u + 0u) * 4u + 3]) == 255); // (0,1) opaque
}

TEST_CASE("DecodeIcoToRgba rejects malformed input without reading out of bounds") {
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    std::string error;

    SUBCASE("null buffer") {
        CHECK_FALSE(DecodeIcoToRgba(nullptr, 0, rgba, width, height, error));
        CHECK_FALSE(error.empty());
    }

    SUBCASE("shorter than an ICONDIR") {
        const unsigned char stub[3] = {0, 0, 1};
        CHECK_FALSE(DecodeIcoToRgba(stub, sizeof(stub), rgba, width, height, error));
    }

    SUBCASE("truncated mid-payload") {
        const SyntheticIco ico = MakeIco(8, 8, 0xFF);
        // The directory entry still claims the full image; the buffer no longer holds it.
        CHECK_FALSE(DecodeIcoToRgba(ico.Bytes.data(), ico.Bytes.size() - 16, rgba, width, height, error));
        CHECK_FALSE(error.empty());
    }

    SUBCASE("zero directory entries") {
        SyntheticIco ico = MakeIco(8, 8, 0xFF);
        PutU16(ico.Bytes, 4, 0);
        CHECK_FALSE(DecodeIcoToRgba(ico.Bytes.data(), ico.Bytes.size(), rgba, width, height, error));
    }

    SUBCASE("PNG-compressed entry is out of scope, not a crash") {
        SyntheticIco ico = MakeIco(8, 8, 0xFF);
        PutU32(ico.Bytes, 6 + 16 + 16, 5); // compression != BI_RGB
        CHECK_FALSE(DecodeIcoToRgba(ico.Bytes.data(), ico.Bytes.size(), rgba, width, height, error));
    }

    SUBCASE("paletted entry is out of scope") {
        SyntheticIco ico = MakeIco(8, 8, 0xFF);
        PutU16(ico.Bytes, 6 + 16 + 14, 8); // 8bpp
        CHECK_FALSE(DecodeIcoToRgba(ico.Bytes.data(), ico.Bytes.size(), rgba, width, height, error));
    }

    SUBCASE("outputs are cleared on failure") {
        rgba.assign(16, 0xAB);
        width = 123;
        height = 456;
        const unsigned char stub[3] = {0, 0, 1};
        CHECK_FALSE(DecodeIcoToRgba(stub, sizeof(stub), rgba, width, height, error));
        CHECK(rgba.empty());
        CHECK(width == 0);
        CHECK(height == 0);
    }
}
