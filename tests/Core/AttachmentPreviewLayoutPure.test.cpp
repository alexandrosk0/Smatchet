// Pure-logic coverage of the layout / classification helpers extracted from
// drawAttachmentPreviewWindow (function-size decomposition). These mirror the inline
// fit-math and card-label/status if/else chains byte-for-byte, so each expectation pins
// exactly what the production monolith rendered for the same inputs.

#include "AttachmentPreviewLayoutPure.h"

#include <doctest/doctest.h>

using smatchet::attach::CardLabel;
using smatchet::attach::CardStatus;
using smatchet::attach::ClassifyAttachmentCardLabel;
using smatchet::attach::ClassifyAttachmentCardStatus;
using smatchet::attach::ComputeImageDrawSize;
using smatchet::attach::ImageDrawSize;

TEST_CASE("ComputeImageDrawSize fits inside box preserving aspect ratio") {
    // 200x100 into a 50x50 box: longest-side scale 0.25 -> 50x25.
    const ImageDrawSize s = ComputeImageDrawSize(200, 100, 50.0f, 50.0f, false);
    CHECK(s.Width == doctest::Approx(50.0f));
    CHECK(s.Height == doctest::Approx(25.0f));
}

TEST_CASE("ComputeImageDrawSize clampToOne never upscales") {
    // 10x10 into a 320x320 box would upscale 32x; clamp pins it to native 10x10.
    const ImageDrawSize clamped = ComputeImageDrawSize(10, 10, 320.0f, 320.0f, true);
    CHECK(clamped.Width == doctest::Approx(10.0f));
    CHECK(clamped.Height == doctest::Approx(10.0f));
    // Without clamp the same input upscales to fill the box.
    const ImageDrawSize unclamped = ComputeImageDrawSize(10, 10, 320.0f, 320.0f, false);
    CHECK(unclamped.Width == doctest::Approx(320.0f));
    CHECK(unclamped.Height == doctest::Approx(320.0f));
}

TEST_CASE("ComputeImageDrawSize treats non-positive dimensions as 1") {
    const ImageDrawSize s = ComputeImageDrawSize(0, 0, 100.0f, 100.0f, false);
    // 1x1 scaled by 100 -> 100x100.
    CHECK(s.Width == doctest::Approx(100.0f));
    CHECK(s.Height == doctest::Approx(100.0f));
}

TEST_CASE("ComputeImageDrawSize uses the tighter of the two axes") {
    // 100x400 into 200x200: width-scale 2.0, height-scale 0.5 -> tighter 0.5 -> 50x200.
    const ImageDrawSize s = ComputeImageDrawSize(100, 400, 200.0f, 200.0f, false);
    CHECK(s.Width == doctest::Approx(50.0f));
    CHECK(s.Height == doctest::Approx(200.0f));
}

TEST_CASE("ClassifyAttachmentCardLabel order-sensitive chain") {
    // preview-error wins over everything.
    CHECK(ClassifyAttachmentCardLabel(true, true, true, true, true) == CardLabel::PreviewError);
    // image + request issued + no local path -> loading.
    CHECK(ClassifyAttachmentCardLabel(false, true, true, true, true) == CardLabel::Loading);
    // image, has local path, no bitmap backend -> metadata.
    CHECK(ClassifyAttachmentCardLabel(false, true, true, false, false) == CardLabel::Metadata);
    // image, has local path, bitmap backend -> image.
    CHECK(ClassifyAttachmentCardLabel(false, true, true, false, true) == CardLabel::Image);
    // non-image -> file.
    CHECK(ClassifyAttachmentCardLabel(false, false, false, false, true) == CardLabel::File);
    // image not yet requested (request not issued) falls through Loading -> image.
    CHECK(ClassifyAttachmentCardLabel(false, true, false, true, true) == CardLabel::Image);
}

TEST_CASE("ClassifyAttachmentCardStatus order-sensitive chain") {
    CHECK(ClassifyAttachmentCardStatus(true, true, true, true, true) == CardStatus::PreviewFailed);
    // image with known dimensions -> dimensions.
    CHECK(ClassifyAttachmentCardStatus(false, true, true, true, true) == CardStatus::Dimensions);
    // image, no dimensions, request issued -> loading.
    CHECK(ClassifyAttachmentCardStatus(false, true, false, true, true) == CardStatus::Loading);
    // image, no dimensions, not requested, no bitmap backend -> metadata.
    CHECK(ClassifyAttachmentCardStatus(false, true, false, false, false) == CardStatus::Metadata);
    // image, no dimensions, not requested, bitmap backend -> image.
    CHECK(ClassifyAttachmentCardStatus(false, true, false, false, true) == CardStatus::Image);
    // non-image -> file.
    CHECK(ClassifyAttachmentCardStatus(false, false, false, false, true) == CardStatus::File);
}
