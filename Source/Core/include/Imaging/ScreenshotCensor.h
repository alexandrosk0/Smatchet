#ifndef SMATCHET_IMAGING_SCREENSHOT_CENSOR_H
#define SMATCHET_IMAGING_SCREENSHOT_CENSOR_H

#include <vector>

// ScreenshotCensor — pure image helpers for the bug-report screenshot path.
// Currently just the downscale used to keep captures under the relay payload cap.
// (Text redaction itself is done at render time via the font swap — see
// SmatchetImGuiFonts SmatchetPushRedactionFonts — not by a CPU image filter.)

namespace smatchet {
namespace imaging {

/// Box-average downscale of an interleaved 8-bit image so its longest side is
/// at most `maxDim`. Updates `w`/`h` and shrinks `px` in place. No-op when the
/// image already fits or args are invalid. Keeps bug-report screenshots small
/// enough to base64 under the relay's payload cap (a 1920px frame is ~4x too big).
void DownscaleToMaxDimension(std::vector<unsigned char>& px, int& w, int& h, int comp, int maxDim);

} // namespace imaging
} // namespace smatchet

#endif // SMATCHET_IMAGING_SCREENSHOT_CENSOR_H
