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
