#pragma once

// Pure byte-level image-dimension parser, extracted from the attachment-preview
// UI so the format decoders (PNG / GIF / WEBP-VP8X / JPEG-SOF) are unit-testable
// without file I/O or an ImGui context. These functions parse *untrusted* file
// bytes — every bounds-check, byte-offset, and endianness read is load-bearing.

// The UI wrapper (SmatchetAttachmentPreviewUi.cpp) does the bounded header read
// off disk, then hands the byte buffer here. Behaviour is byte-for-byte
// identical to the pre-extraction monolith.

#include <string>
#include <vector>

namespace smatchet {
namespace image_dim {

struct ParsedImageInfo {
    bool Ok = false;
    int Width = 0;
    int Height = 0;
    std::string Error;
};

// Inclusive upper bound on accepted image dimensions. Rejecting past this cap
// bounds preview memory and keeps the value below INT_MAX, so the parsers'
// `static_cast<int>` can never yield a negative dimension feeding UI layout /
// texture alloc. Parallels post-decode `kMaxGoldenImageDim` (test harness).
constexpr int kMaxImageDimension = 16384;

// Parse image dimensions from an already-read header byte buffer. `mimeType` is
// only consulted for the final "unsupported vs unparseable" error wording when
// no format signature matches. Empty buffer yields the empty-file error.
ParsedImageInfo ParseImageDimensionsFromBytes(const std::vector<unsigned char>& bytes, const std::string& mimeType);

} // namespace image_dim
} // namespace smatchet
