#ifndef SMATCHET_IMAGING_SCREENSHOT_CENSOR_H
#define SMATCHET_IMAGING_SCREENSHOT_CENSOR_H

#include <vector>

// ScreenshotCensor — in-place mosaic (pixelation) of an interleaved 8-bit image
// so a bug-report screenshot carries layout/colour context but no readable text.
// Pure (no GL / no allocation of the image — operates on the caller's buffer) so
// it lives in Core and is reusable by both standalone capture and tests.

namespace smatchet {
namespace imaging {

/// Block-average mosaic in place. `px` is `w*h*comp` interleaved 8-bit samples
/// (e.g. comp=3 RGB). Each `block`x`block` cell is replaced by its own average,
/// destroying glyph-scale detail. Edge cells clamp to the in-bounds region.
/// No-op when `px` is null, dims/comp/block are non-positive.
void MosaicCensorInPlace(unsigned char* px, int w, int h, int comp, int block);

/// Box-average downscale of an interleaved 8-bit image so its longest side is
/// at most `maxDim`. Updates `w`/`h` and shrinks `px` in place. No-op when the
/// image already fits or args are invalid. Keeps bug-report screenshots small
/// enough to base64 under the relay's payload cap (a 1920px frame is ~4x too big).
void DownscaleToMaxDimension(std::vector<unsigned char>& px, int& w, int& h, int comp, int maxDim);

/// Recommended mosaic block size for a `w`x`h` capture:
/// `clamp(round(min(w,h)/64), 12, 48)`. The 12px floor guarantees ~13px body
/// text is unreadable; the 48px ceiling keeps large captures from turning into
/// a handful of giant squares.
int RecommendedCensorBlock(int w, int h);

} // namespace imaging
} // namespace smatchet

#endif // SMATCHET_IMAGING_SCREENSHOT_CENSOR_H
