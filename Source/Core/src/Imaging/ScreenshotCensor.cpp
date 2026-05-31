#include "ScreenshotCensor.h"

#include <algorithm>
#include <cmath>

namespace smatchet {
namespace imaging {

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

} // namespace imaging
} // namespace smatchet
