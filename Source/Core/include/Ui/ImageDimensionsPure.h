#pragma once

// Pure byte-level image-dimension parser, extracted from the attachment-preview
// UI so the format decoders (PNG / GIF / WEBP-VP8X / JPEG-SOF) are unit-testable
// without file I/O or an ImGui context. These functions parse *untrusted* file
// bytes — every bounds-check, byte-offset, and endianness read is load-bearing.
//
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

// Parse image dimensions from an already-read header byte buffer. `mimeType` is
// only consulted for the final "unsupported vs unparseable" error wording when
// no format signature matches. Empty buffer yields the empty-file error.
ParsedImageInfo ParseImageDimensionsFromBytes(const std::vector<unsigned char>& bytes, const std::string& mimeType);

} // namespace image_dim
} // namespace smatchet
