#include "Persistence/SmatchetIcoDecode.h"

#include <cstdint>

namespace {

// Matches the icon cache's own ceiling (SmatchetImageTextureCache kMaxIconDimension) so a decode
// that would be rejected at upload time fails here instead, with a decode-shaped error message.
const int kMaxIcoDimension = 512;

const std::size_t kIconDirSize = 6;
const std::size_t kIconDirEntrySize = 16;
const std::size_t kBitmapInfoHeaderSize = 40;

uint16_t ReadU16(const unsigned char* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t ReadU32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

struct IcoImage {
    std::size_t PixelOffset = 0; ///< First XOR pixel byte, i.e. past the BITMAPINFOHEADER.
    std::size_t PixelBytes = 0;  ///< Bytes available from PixelOffset to the end of this entry.
    int Width = 0;
    int Height = 0; ///< Real height (the header's doubled value halved).
};

/**
 * Locate the largest uncompressed-32bpp entry in the directory. Every bound is re-derived from
 * `byteCount` rather than trusted from the file, so a truncated or hostile .ico fails the size
 * checks instead of reading past the buffer.
 */
bool SelectBestEntry(const unsigned char* bytes, std::size_t byteCount, IcoImage& out, std::string& outError) {
    if (byteCount < kIconDirSize) {
        outError = "ICO: file shorter than its directory header.";
        return false;
    }
    if (ReadU16(bytes) != 0 || ReadU16(bytes + 2) != 1) {
        outError = "ICO: not an icon directory (bad reserved/type).";
        return false;
    }
    const std::size_t count = ReadU16(bytes + 4);
    if (count == 0 || byteCount < kIconDirSize + count * kIconDirEntrySize) {
        outError = "ICO: directory entry table is missing or truncated.";
        return false;
    }

    bool found = false;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char* entry = bytes + kIconDirSize + i * kIconDirEntrySize;
        const std::size_t blobSize = ReadU32(entry + 8);
        const std::size_t blobOffset = ReadU32(entry + 12);
        // Overflow-safe containment test: blobOffset + blobSize could wrap on 32-bit sizes.
        if (blobSize <= kBitmapInfoHeaderSize || blobOffset > byteCount || blobSize > byteCount - blobOffset) {
            continue; // Truncated or out-of-range entry; a sibling entry may still be usable.
        }
        const unsigned char* bmp = bytes + blobOffset;
        if (ReadU32(bmp) != kBitmapInfoHeaderSize) {
            continue; // PNG-compressed entry, or a header revision this decoder does not read.
        }
        const int32_t width = static_cast<int32_t>(ReadU32(bmp + 4));
        const int32_t doubledHeight = static_cast<int32_t>(ReadU32(bmp + 8));
        const uint16_t bitCount = ReadU16(bmp + 14);
        const uint32_t compression = ReadU32(bmp + 16);
        if (bitCount != 32 || compression != 0) {
            continue; // Paletted or RLE entry.
        }
        // Icon resources double biHeight to cover the trailing AND mask; an odd value is malformed.
        if (width <= 0 || doubledHeight <= 0 || (doubledHeight % 2) != 0) {
            continue;
        }
        const int height = doubledHeight / 2;
        if (width > kMaxIcoDimension || height > kMaxIcoDimension) {
            continue;
        }
        const std::size_t pixelBytes = blobSize - kBitmapInfoHeaderSize;
        const std::size_t needed = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        if (pixelBytes < needed) {
            continue;
        }
        if (found && static_cast<long long>(width) * height <= static_cast<long long>(out.Width) * out.Height) {
            continue;
        }
        out.PixelOffset = blobOffset + kBitmapInfoHeaderSize;
        out.PixelBytes = pixelBytes;
        out.Width = width;
        out.Height = height;
        found = true;
    }
    if (!found) {
        outError = "ICO: no uncompressed 32bpp entry found.";
    }
    return found;
}

/**
 * Reconstruct alpha from the 1-bit AND mask that follows the XOR pixels (a set bit means
 * transparent). Mask rows are bottom-up and padded to 4 bytes, MSB first. A mask too short to
 * cover the image leaves the icon fully opaque rather than half-decoded.
 */
void FillAlphaFromMask(const unsigned char* pixels, std::size_t pixelBytes, int width, int height,
                       std::vector<unsigned char>& rgba) {
    const std::size_t xorBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    const std::size_t maskStride = ((static_cast<std::size_t>(width) + 31u) / 32u) * 4u;
    const bool maskPresent = pixelBytes - xorBytes >= maskStride * static_cast<std::size_t>(height);
    if (!maskPresent) {
        return;
    }
    const unsigned char* mask = pixels + xorBytes;
    for (int y = 0; y < height; ++y) {
        const unsigned char* maskRow = mask + static_cast<std::size_t>(height - 1 - y) * maskStride;
        unsigned char* dstRow = rgba.data() + static_cast<std::size_t>(y) * width * 4u;
        for (int x = 0; x < width; ++x) {
            const unsigned char bit = static_cast<unsigned char>((maskRow[x / 8] >> (7 - (x % 8))) & 1u);
            dstRow[x * 4 + 3] = bit ? 0u : 255u;
        }
    }
}

} // namespace

namespace smatchet {
namespace icons {

bool DecodeIcoToRgba(const unsigned char* bytes, std::size_t byteCount, std::vector<unsigned char>& outRgba,
                     int& outWidth, int& outHeight, std::string& outError) {
    outRgba.clear();
    outWidth = 0;
    outHeight = 0;
    outError.clear();
    if (bytes == nullptr || byteCount == 0) {
        outError = "ICO: empty input.";
        return false;
    }

    IcoImage img;
    if (!SelectBestEntry(bytes, byteCount, img, outError)) {
        return false;
    }

    const unsigned char* pixels = bytes + img.PixelOffset;
    const std::size_t rowBytes = static_cast<std::size_t>(img.Width) * 4u;
    outRgba.resize(rowBytes * static_cast<std::size_t>(img.Height));

    // BGRA, bottom-up in the file; RGBA, top-down in the output.
    bool anyAlpha = false;
    for (int y = 0; y < img.Height; ++y) {
        const unsigned char* srcRow = pixels + static_cast<std::size_t>(img.Height - 1 - y) * rowBytes;
        unsigned char* dstRow = outRgba.data() + static_cast<std::size_t>(y) * rowBytes;
        for (int x = 0; x < img.Width; ++x) {
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
            anyAlpha = anyAlpha || srcRow[x * 4 + 3] != 0;
        }
    }
    if (!anyAlpha) {
        // Pre-XP icons leave the alpha channel zeroed and carry transparency in the mask only.
        // Taken literally that renders a fully invisible icon, so fall back to the mask.
        for (std::size_t i = 3; i < outRgba.size(); i += 4) {
            outRgba[i] = 255u;
        }
        FillAlphaFromMask(pixels, img.PixelBytes, img.Width, img.Height, outRgba);
    }

    outWidth = img.Width;
    outHeight = img.Height;
    return true;
}

} // namespace icons
} // namespace smatchet
