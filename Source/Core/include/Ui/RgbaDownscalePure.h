#pragma once

#include <cstddef>
#include <vector>

// Pure, ImGui-free, allocation-bounded RGBA32 (4-byte) area-average downscale. Lives in a header
// seam so it unit-tests on the doctest rig (no renderer / ImGui / WIC / stb dependency). Used by
// the non-Windows attachment-thumbnail decode path: stb_image cannot decode-scale (unlike WIC), so
// the full-resolution buffer is materialised on the S5 worker and then box-averaged down here to the
// same longest-side budget the WIC scaler enforces on Windows. Box (area) averaging — not nearest —
// so a multi-megapixel photo shrinks without the aliasing a point sample would leave.
namespace smatchet {
namespace image_scale {

// Compute the dimensions that fit `srcW`x`srcH` inside a `maxDimension` longest-side budget,
// preserving aspect ratio (integer math, floor). Returns {srcW,srcH} unchanged when maxDimension<=0
// or the image already fits. dst dims are clamped to >=1 whenever the src dim is >=1.
inline void FitWithinLongestSide(int srcW, int srcH, int maxDimension, int& dstW, int& dstH) {
    dstW = srcW;
    dstH = srcH;
    if (maxDimension <= 0 || srcW <= 0 || srcH <= 0) {
        return;
    }
    const int longest = (srcW > srcH) ? srcW : srcH;
    if (longest <= maxDimension) {
        return;
    }
    // dst = src * maxDimension / longest, done in 64-bit to avoid the intermediate overflow for a
    // pathologically large source dimension; clamp to >=1 so an extreme aspect ratio never yields 0.
    long long w = static_cast<long long>(srcW) * static_cast<long long>(maxDimension) / static_cast<long long>(longest);
    long long h = static_cast<long long>(srcH) * static_cast<long long>(maxDimension) / static_cast<long long>(longest);
    dstW = (w < 1) ? 1 : static_cast<int>(w);
    dstH = (h < 1) ? 1 : static_cast<int>(h);
}

// Area-average (box) downscale `src` (RGBA32, row-major, `srcW`x`srcH`) so its longest side is at
// most `maxDimension`. Writes the result into `outPixels` and the chosen size into `outW`/`outH`.
//
// Returns false (leaving outPixels empty, outW/outH = 0) on bad input: empty src, non-positive
// dims, or src smaller than srcW*srcH*4. When the image already fits (or maxDimension<=0) it copies
// the source through unchanged (outW=srcW, outH=srcH) and returns true. Pure: no allocation beyond
// the single `outPixels` resize, no global state, deterministic.
inline bool DownscaleRgba32(const std::vector<unsigned char>& src, int srcW, int srcH, int maxDimension,
                            std::vector<unsigned char>& outPixels, int& outW, int& outH) {
    outPixels.clear();
    outW = 0;
    outH = 0;
    if (srcW <= 0 || srcH <= 0) {
        return false;
    }
    const std::size_t srcCount = static_cast<std::size_t>(srcW) * static_cast<std::size_t>(srcH) * 4u;
    if (src.size() < srcCount) {
        return false;
    }

    int dstW = srcW;
    int dstH = srcH;
    FitWithinLongestSide(srcW, srcH, maxDimension, dstW, dstH);

    if (dstW == srcW && dstH == srcH) {
        // Already within budget — pass the source through unchanged.
        outPixels.assign(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(srcCount));
        outW = srcW;
        outH = srcH;
        return true;
    }

    outPixels.resize(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH) * 4u);

    // Each destination pixel averages the source rectangle [sx0,sx1) x [sy0,sy1). Using the floor
    // partition (dx*srcW/dstW) tiles the source exactly once across all dst columns/rows; the >sx0
    // guard keeps every box at least one pixel wide/tall (defensive — for downscale dst<=src the
    // floor partition is already non-empty). u64 accumulators: max box ~= srcW*srcH pixels * 255
    // fits comfortably.
    for (int dy = 0; dy < dstH; ++dy) {
        int sy0 = static_cast<int>(static_cast<long long>(dy) * srcH / dstH);
        int sy1 = static_cast<int>(static_cast<long long>(dy + 1) * srcH / dstH);
        if (sy1 <= sy0) {
            sy1 = sy0 + 1;
        }
        if (sy1 > srcH) {
            sy1 = srcH;
        }
        for (int dx = 0; dx < dstW; ++dx) {
            int sx0 = static_cast<int>(static_cast<long long>(dx) * srcW / dstW);
            int sx1 = static_cast<int>(static_cast<long long>(dx + 1) * srcW / dstW);
            if (sx1 <= sx0) {
                sx1 = sx0 + 1;
            }
            if (sx1 > srcW) {
                sx1 = srcW;
            }

            unsigned long long r = 0;
            unsigned long long g = 0;
            unsigned long long b = 0;
            unsigned long long a = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const std::size_t rowBase = static_cast<std::size_t>(sy) * static_cast<std::size_t>(srcW) * 4u;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const std::size_t p = rowBase + static_cast<std::size_t>(sx) * 4u;
                    r += src[p + 0];
                    g += src[p + 1];
                    b += src[p + 2];
                    a += src[p + 3];
                }
            }
            const unsigned long long count =
                static_cast<unsigned long long>(sx1 - sx0) * static_cast<unsigned long long>(sy1 - sy0);
            const std::size_t dstP =
                (static_cast<std::size_t>(dy) * static_cast<std::size_t>(dstW) + static_cast<std::size_t>(dx)) * 4u;
            outPixels[dstP + 0] = static_cast<unsigned char>(r / count);
            outPixels[dstP + 1] = static_cast<unsigned char>(g / count);
            outPixels[dstP + 2] = static_cast<unsigned char>(b / count);
            outPixels[dstP + 3] = static_cast<unsigned char>(a / count);
        }
    }
    outW = dstW;
    outH = dstH;
    return true;
}

} // namespace image_scale
} // namespace smatchet
