#pragma once

#include "StringUtil.h"

#include <string>

inline std::string MimeTypeWithoutParams(const std::string& mimeType) {
    const std::string trimmed = TrimCopyAsciiWhitespace(mimeType);
    if (trimmed.empty()) {
        return std::string();
    }
    const size_t sep = trimmed.find(';');
    const std::string value = sep == std::string::npos ? trimmed : TrimCopyAsciiWhitespace(trimmed.substr(0, sep));
    return ToLowerAsciiCopy(value);
}

inline bool IsSupportedImageMime(const std::string& mimeType) {
    const std::string normalized = MimeTypeWithoutParams(mimeType);
    return normalized == "image/png" || normalized == "image/jpeg" || normalized == "image/jpg" ||
           normalized == "image/gif" || normalized == "image/webp";
}

inline std::string ExtensionFromMime(const std::string& mimeType) {
    const std::string normalized = MimeTypeWithoutParams(mimeType);
    if (normalized == "image/png")
        return ".png";
    if (normalized == "image/jpeg")
        return ".jpg";
    if (normalized == "image/jpg")
        return ".jpg";
    if (normalized == "image/gif")
        return ".gif";
    if (normalized == "image/webp")
        return ".webp";
    if (normalized == "application/pdf")
        return ".pdf";
    return ".bin";
}
