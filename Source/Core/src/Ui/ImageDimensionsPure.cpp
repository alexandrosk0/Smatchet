#include "ImageDimensionsPure.h"

#include "AttachmentMimeUtils.h"

#include <cstdint>

namespace smatchet {
namespace image_dim {

namespace {

std::uint32_t ReadU32BE(const unsigned char* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

std::uint16_t ReadU16LE(const unsigned char* data) {
    return static_cast<std::uint16_t>(data[0]) | static_cast<std::uint16_t>(data[1] << 8);
}

std::uint32_t ReadU24LE(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16);
}

// Each per-format helper returns true when the buffer carries that format's
// signature (and therefore `result` is final — either Ok with dimensions or a
// format-specific error). Returns false to let the caller try the next format.
// Byte offsets, bounds-checks, and endianness reads are preserved verbatim from
// the original monolith.

bool TryParsePng(const std::vector<unsigned char>& bytes, ParsedImageInfo& result) {
    if (!(bytes.size() >= 24 && bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 &&
          bytes[4] == 0x0D && bytes[5] == 0x0A && bytes[6] == 0x1A && bytes[7] == 0x0A)) {
        return false;
    }
    const std::uint32_t width = ReadU32BE(&bytes[16]);
    const std::uint32_t height = ReadU32BE(&bytes[20]);
    if (width == 0 || height == 0) {
        result.Error = "PNG dimensions are invalid.";
        return true;
    }
    result.Ok = true;
    result.Width = static_cast<int>(width);
    result.Height = static_cast<int>(height);
    return true;
}

bool TryParseGif(const std::vector<unsigned char>& bytes, ParsedImageInfo& result) {
    if (!(bytes.size() >= 10 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F' && (bytes[3] == '8') &&
          (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a')) {
        return false;
    }
    const std::uint16_t width = ReadU16LE(&bytes[6]);
    const std::uint16_t height = ReadU16LE(&bytes[8]);
    if (width == 0 || height == 0) {
        result.Error = "GIF dimensions are invalid.";
        return true;
    }
    result.Ok = true;
    result.Width = static_cast<int>(width);
    result.Height = static_cast<int>(height);
    return true;
}

bool TryParseWebp(const std::vector<unsigned char>& bytes, ParsedImageInfo& result) {
    if (!(bytes.size() >= 30 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F' &&
          bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P')) {
        return false;
    }
    // Only the extended VP8X form carries dimensions at a fixed offset; other
    // WEBP forms fall through to the generic "could not parse" degradation
    // (matching the original: the outer RIFF/WEBP match alone does not finalise).
    if (bytes[12] == 'V' && bytes[13] == 'P' && bytes[14] == '8' && bytes[15] == 'X') {
        const std::uint32_t widthMinusOne = ReadU24LE(&bytes[24]);
        const std::uint32_t heightMinusOne = ReadU24LE(&bytes[27]);
        result.Ok = true;
        result.Width = static_cast<int>(widthMinusOne + 1);
        result.Height = static_cast<int>(heightMinusOne + 1);
        return true;
    }
    return false;
}

bool TryParseJpeg(const std::vector<unsigned char>& bytes, ParsedImageInfo& result) {
    if (!(bytes.size() >= 4 && bytes[0] == 0xFF && bytes[1] == 0xD8)) {
        return false;
    }
    size_t i = 2;
    while (i + 9 < bytes.size()) {
        if (bytes[i] != 0xFF) {
            ++i;
            continue;
        }
        while (i < bytes.size() && bytes[i] == 0xFF) {
            ++i;
        }
        if (i >= bytes.size()) {
            break;
        }
        const unsigned char marker = bytes[i++];
        if (marker == 0xD8 || marker == 0xD9) {
            continue;
        }
        if (i + 1 >= bytes.size()) {
            break;
        }
        const std::uint16_t segmentLength =
            static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[i]) << 8) | bytes[i + 1]);
        if (segmentLength < 2 || i + segmentLength > bytes.size()) {
            break;
        }
        const bool isSofMarker = (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
                                 (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
        if (isSofMarker && segmentLength >= 7) {
            const std::uint16_t height =
                static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[i + 3]) << 8) | bytes[i + 4]);
            const std::uint16_t width =
                static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[i + 5]) << 8) | bytes[i + 6]);
            if (width == 0 || height == 0) {
                result.Error = "JPEG dimensions are invalid.";
                return true;
            }
            result.Ok = true;
            result.Width = static_cast<int>(width);
            result.Height = static_cast<int>(height);
            return true;
        }
        i += segmentLength;
    }
    // JPEG SOI matched but no SOF parsed within the window — fall through to the
    // generic degradation message (matching the original control flow).
    return false;
}

} // namespace

ParsedImageInfo ParseImageDimensionsFromBytes(const std::vector<unsigned char>& bytes, const std::string& mimeType) {
    ParsedImageInfo result;
    if (bytes.empty()) {
        result.Error = "Downloaded attachment file is empty.";
        return result;
    }
    if (TryParsePng(bytes, result)) {
        return result;
    }
    if (TryParseGif(bytes, result)) {
        return result;
    }
    if (TryParseWebp(bytes, result)) {
        return result;
    }
    if (TryParseJpeg(bytes, result)) {
        return result;
    }

    result.Error = IsSupportedImageMime(mimeType)
                       ? "Image format detected but dimensions could not be parsed by the in-app preview."
                       : "Attachment is not a supported image format for in-app preview.";
    return result;
}

} // namespace image_dim
} // namespace smatchet
