#pragma once

// IconDimensionsPolicy — the pure accept/reject decision for an image's declared dimensions,
// shared by the icon texture cache so a malicious/huge image can be rejected from the cheap
// header read (stbi_info) BEFORE the full stb decode allocates pixel storage (memory-pressure
// DoS, SECURITY_AUDIT_2026-06-13 synthesis #9). Pure + header-only so it is unit-testable
// without a GPU context, an ImGui frame, or real stb decode.

#include <cstddef>

namespace smatchet {

/// True when an image of `width` x `height` (RGBA32, 4 bytes/pixel) is within the per-icon
/// dimension cap AND its pixel-storage byte estimate fits `maxPixelBytes`. Returns false for
/// any non-positive dimension. The byte check is overflow-safe: the per-side dimension cap is
/// applied first, so `width * height * 4` is bounded by `maxDim^2 * 4` (well under SIZE_MAX for
/// any sane cap) and the std::size_t multiply cannot wrap. `maxPixelBytes` is a second,
/// aggregate-pixel guard so a pathological aspect ratio that slips both side caps still can't
/// request an unbounded allocation.
inline bool IconDimensionsWithinCap(int width, int height, int maxDim, std::size_t maxPixelBytes) {
    if (width <= 0 || height <= 0 || maxDim <= 0) {
        return false;
    }
    if (width > maxDim || height > maxDim) {
        return false;
    }
    // Side caps passed, so width,height <= maxDim: the size_t product is bounded by
    // maxDim*maxDim*4 and cannot overflow for any realistic cap. Compute in size_t regardless.
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::size_t bytes = pixels * static_cast<std::size_t>(4u);
    return bytes <= maxPixelBytes;
}

} // namespace smatchet
