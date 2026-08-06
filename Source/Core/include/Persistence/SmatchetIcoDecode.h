#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace smatchet {
namespace icons {

/**
 * Decode a Windows .ico image into tightly-packed, top-down RGBA32 pixels.
 *
 * Exists because stb_image cannot read .ico at all: the icon payload is a BMP without a
 * BITMAPFILEHEADER, whose BITMAPINFOHEADER height is doubled to cover a trailing 1-bit AND mask.
 * The app icon (`Source/Standalone/smatchet.ico`) is baked into the binary as bytes and unpacked
 * here so the About dialog can show it with no runtime file access and no second image asset.
 *
 * Scope is deliberately narrow — uncompressed 32bpp (BI_RGB) directory entries only. PNG-compressed
 * entries (Vista+ 256px) and paletted entries return false rather than growing a second decoder;
 * every caller already needs a no-texture fallback path, and false routes to it.
 *
 * Largest qualifying directory entry wins. Alpha comes from the BGRA payload, except when that
 * channel is entirely zero (pre-XP icons left it unset and leant on the mask), in which case it is
 * reconstructed from the AND mask.
 *
 * @return true with `outRgba` sized `outWidth * outHeight * 4`; false with `outError` set,
 *         leaving the outputs cleared. Never throws, never reads outside `bytes[0, byteCount)`.
 */
bool DecodeIcoToRgba(const unsigned char* bytes, std::size_t byteCount, std::vector<unsigned char>& outRgba,
                     int& outWidth, int& outHeight, std::string& outError);

} // namespace icons
} // namespace smatchet
