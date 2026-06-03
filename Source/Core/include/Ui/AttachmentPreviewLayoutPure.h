#pragma once

// Pure layout / classification helpers extracted from drawAttachmentPreviewWindow
// (function-size decomposition). No ImGui, file-I/O, or app-state dependency, so the
// fit-math and card-label/status classification are unit-testable in isolation. Each
// helper is a byte-for-byte extraction of the inline branch chains the monolith
// shipped — the production frame behaviour is unchanged.

#include <string>

namespace smatchet {
namespace attach {

// Scaled draw size that fits image (imageWidth x imageHeight) inside a maxWidth x
// maxHeight box, preserving aspect ratio. When clampToOne is true the image is never
// upscaled past its native size (the scale is capped at 1.0). Mirrors the inline
// tooltip / details-pane math: a non-positive dimension is treated as 1, and a
// non-positive scale leaves the (clamped-to-1) source dimensions untouched.
struct ImageDrawSize {
    float Width = 0.0f;
    float Height = 0.0f;
};

inline ImageDrawSize ComputeImageDrawSize(int imageWidth, int imageHeight, float maxWidth, float maxHeight,
                                          bool clampToOne) {
    float w = static_cast<float>(imageWidth > 0 ? imageWidth : 1);
    float h = static_cast<float>(imageHeight > 0 ? imageHeight : 1);
    const float scaleW = maxWidth / w;
    const float scaleH = maxHeight / h;
    const float scale = scaleW < scaleH ? scaleW : scaleH;
    if (scale > 0.0f) {
        const float applied = clampToOne ? (scale < 1.0f ? scale : 1.0f) : scale;
        w *= applied;
        h *= applied;
    }
    ImageDrawSize out;
    out.Width = w;
    out.Height = h;
    return out;
}

// Classification of the placeholder button label shown for an attachment card when no
// bitmap thumbnail is available. Order-sensitive: mirrors the inline if/else-if chain.
enum class CardLabel {
    PreviewError, // "Preview error"
    Loading,      // "Loading..."
    Metadata,     // "Metadata"  (image, no bitmap backend)
    Image,        // "Image"
    File          // "File"  (non-image)
};

inline CardLabel ClassifyAttachmentCardLabel(bool hasPreviewError, bool isSupportedImage, bool previewRequestIssued,
                                             bool localPathEmpty, bool canRenderBitmap) {
    if (hasPreviewError) {
        return CardLabel::PreviewError;
    }
    if (isSupportedImage && previewRequestIssued && localPathEmpty) {
        return CardLabel::Loading;
    }
    if (isSupportedImage && !canRenderBitmap) {
        return CardLabel::Metadata;
    }
    if (isSupportedImage) {
        return CardLabel::Image;
    }
    return CardLabel::File;
}

// Classification of the small status text under an attachment card's filename.
// Order-sensitive: mirrors the inline if/else-if chain.
enum class CardStatus {
    PreviewFailed, // "preview failed"
    Dimensions,    // "%dx%d"  (image with known dimensions)
    Loading,       // "loading"
    Metadata,      // "metadata"  (image, no bitmap backend)
    Image,         // "image"
    File           // "file"  (non-image)
};

inline CardStatus ClassifyAttachmentCardStatus(bool hasPreviewError, bool isSupportedImage, bool hasDimensions,
                                               bool previewRequestIssued, bool canRenderBitmap) {
    if (hasPreviewError) {
        return CardStatus::PreviewFailed;
    }
    if (isSupportedImage) {
        if (hasDimensions) {
            return CardStatus::Dimensions;
        }
        if (previewRequestIssued) {
            return CardStatus::Loading;
        }
        if (!canRenderBitmap) {
            return CardStatus::Metadata;
        }
        return CardStatus::Image;
    }
    return CardStatus::File;
}

} // namespace attach
} // namespace smatchet
