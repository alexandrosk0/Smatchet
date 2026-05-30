#include "ScreenshotCensor.h"

#include <algorithm>
#include <cmath>

namespace smatchet {
namespace imaging {

void MosaicCensorInPlace(unsigned char* px, int w, int h, int comp, int block) {
    if (px == nullptr || w <= 0 || h <= 0 || comp <= 0 || block <= 0) {
        return;
    }
    const std::size_t rowStride = static_cast<std::size_t>(w) * static_cast<std::size_t>(comp);
    for (int by = 0; by < h; by += block) {
        const int cellH = std::min(block, h - by);
        for (int bx = 0; bx < w; bx += block) {
            const int cellW = std::min(block, w - bx);
            const int count = cellW * cellH;
            // Average each component over the cell, then write it back.
            for (int c = 0; c < comp; ++c) {
                unsigned long sum = 0;
                for (int y = 0; y < cellH; ++y) {
                    const unsigned char* row = px + static_cast<std::size_t>(by + y) * rowStride +
                                               static_cast<std::size_t>(bx) * static_cast<std::size_t>(comp) +
                                               static_cast<std::size_t>(c);
                    for (int x = 0; x < cellW; ++x) {
                        sum += row[static_cast<std::size_t>(x) * static_cast<std::size_t>(comp)];
                    }
                }
                const unsigned char avg = static_cast<unsigned char>(sum / static_cast<unsigned long>(count));
                for (int y = 0; y < cellH; ++y) {
                    unsigned char* row = px + static_cast<std::size_t>(by + y) * rowStride +
                                         static_cast<std::size_t>(bx) * static_cast<std::size_t>(comp) +
                                         static_cast<std::size_t>(c);
                    for (int x = 0; x < cellW; ++x) {
                        row[static_cast<std::size_t>(x) * static_cast<std::size_t>(comp)] = avg;
                    }
                }
            }
        }
    }
}

void DownscaleToMaxDimension(std::vector<unsigned char>& px, int& w, int& h, int comp, int maxDim) {
    if (w <= 0 || h <= 0 || comp <= 0 || maxDim <= 0) {
        return;
    }
    const int longest = std::max(w, h);
    if (longest <= maxDim) {
        return; // already small enough
    }
    const double scale = static_cast<double>(maxDim) / static_cast<double>(longest);
    const int dw = std::max(1, static_cast<int>(std::lround(w * scale)));
    const int dh = std::max(1, static_cast<int>(std::lround(h * scale)));
    if (static_cast<std::size_t>(px.size()) < static_cast<std::size_t>(w) * h * comp) {
        return; // malformed buffer — refuse rather than read OOB
    }

    std::vector<unsigned char> out(static_cast<std::size_t>(dw) * dh * comp);
    const std::size_t srcStride = static_cast<std::size_t>(w) * comp;
    for (int y = 0; y < dh; ++y) {
        // Source row span covered by this destination row (box filter).
        const int sy0 = static_cast<int>(static_cast<double>(y) * h / dh);
        const int sy1 = std::max(sy0 + 1, static_cast<int>(static_cast<double>(y + 1) * h / dh));
        for (int x = 0; x < dw; ++x) {
            const int sx0 = static_cast<int>(static_cast<double>(x) * w / dw);
            const int sx1 = std::max(sx0 + 1, static_cast<int>(static_cast<double>(x + 1) * w / dw));
            for (int c = 0; c < comp; ++c) {
                unsigned long sum = 0;
                int count = 0;
                for (int sy = sy0; sy < sy1 && sy < h; ++sy) {
                    for (int sx = sx0; sx < sx1 && sx < w; ++sx) {
                        sum += px[sy * srcStride + static_cast<std::size_t>(sx) * comp + c];
                        ++count;
                    }
                }
                out[(static_cast<std::size_t>(y) * dw + x) * comp + c] =
                    static_cast<unsigned char>(count > 0 ? sum / static_cast<unsigned long>(count) : 0);
            }
        }
    }
    px.swap(out);
    w = dw;
    h = dh;
}

int RecommendedCensorBlock(int w, int h) {
    if (w <= 0 || h <= 0) {
        return 12;
    }
    const int minDim = std::min(w, h);
    const long scaled = std::lround(static_cast<double>(minDim) / 64.0);
    const long clamped = std::max(12L, std::min(48L, scaled));
    return static_cast<int>(clamped);
}

} // namespace imaging
} // namespace smatchet
