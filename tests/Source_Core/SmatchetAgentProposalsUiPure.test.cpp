#include <doctest/doctest.h>

#include "SmatchetAgentProposalsUiPure.h"

#include <string>

using SmatchetAgentProposalsUiPure::kRationalePreviewBytes;
using SmatchetAgentProposalsUiPure::TruncatePreview;

TEST_CASE("TruncatePreview: empty input returns empty") {
    CHECK(TruncatePreview("") == "");
}

TEST_CASE("TruncatePreview: input shorter than cutoff returns input unchanged") {
    const std::string shortInput = "Short rationale.";
    REQUIRE(shortInput.size() < kRationalePreviewBytes);
    CHECK(TruncatePreview(shortInput) == shortInput);
}

TEST_CASE("TruncatePreview: input exactly at cutoff returns input unchanged") {
    const std::string exact(kRationalePreviewBytes, 'a');
    REQUIRE(exact.size() == kRationalePreviewBytes);
    CHECK(TruncatePreview(exact) == exact);
}

TEST_CASE("TruncatePreview: ASCII-only above cutoff truncates at exact byte position") {
    const std::string overflow(kRationalePreviewBytes + 50, 'x');
    const std::string out = TruncatePreview(overflow);
    REQUIRE(out.size() == kRationalePreviewBytes + 3);
    CHECK(out.substr(0, kRationalePreviewBytes) == std::string(kRationalePreviewBytes, 'x'));
    CHECK(out.substr(kRationalePreviewBytes) == "...");
}

TEST_CASE("TruncatePreview: UTF-8 multibyte sequence at cutoff backs off to codepoint boundary") {
    // Build input: 198 ASCII bytes, then a 3-byte UTF-8 codepoint (U+2014 EM DASH = 0xE2 0x80 0x94).
    // kRationalePreviewBytes = 200, so the cut lands inside the 3-byte sequence (at byte 200,
    // which is a continuation byte 0x80). TruncatePreview must back off to the 0xE2 lead byte (198).
    std::string input(198, 'a');
    input += "\xE2\x80\x94";  // U+2014
    input += std::string(50, 'b');
    REQUIRE(input.size() > kRationalePreviewBytes);
    const std::string out = TruncatePreview(input);
    // Output = 198 a's + "..." (the partial codepoint dropped)
    CHECK(out.size() == 198 + 3);
    CHECK(out.substr(0, 198) == std::string(198, 'a'));
    CHECK(out.substr(198) == "...");
}

TEST_CASE("TruncatePreview: UTF-8 4-byte sequence at cutoff backs off cleanly") {
    // U+1F600 (grinning face emoji) = 0xF0 0x9F 0x98 0x80 = 4 bytes.
    // Place it at offset 197 so byte 200 is the 4th byte (continuation byte).
    std::string input(197, 'a');
    input += "\xF0\x9F\x98\x80";  // U+1F600
    input += std::string(50, 'b');
    REQUIRE(input.size() > kRationalePreviewBytes);
    const std::string out = TruncatePreview(input);
    // Cut walks back from 200 → 199 → 198 → 197 (the 0xF0 lead byte) — none of these
    // are valid codepoint boundaries either (they're continuation bytes of the same
    // codepoint). The lead byte 0xF0 at 197 IS the boundary. Output = first 197 a's + "..."
    CHECK(out.size() == 197 + 3);
    CHECK(out.substr(0, 197) == std::string(197, 'a'));
    CHECK(out.substr(197) == "...");
}

TEST_CASE("TruncatePreview: UTF-8 sequence ending before cutoff is preserved intact") {
    // Build input: ASCII a's filling exactly to a codepoint boundary at byte 195,
    // then a 2-byte codepoint (U+00E9 = 0xC3 0xA9) at 195-196, then ASCII to overflow.
    // Cutoff at byte 200 lands inside ASCII territory (byte 197 onwards), so no back-off.
    std::string input(195, 'a');
    input += "\xC3\xA9";  // U+00E9 — ends at byte 197
    input += std::string(50, 'b');
    REQUIRE(input.size() > kRationalePreviewBytes);
    const std::string out = TruncatePreview(input);
    // Cutoff at 200 is a regular ASCII byte; no back-off; output = first 200 + "..."
    CHECK(out.size() == kRationalePreviewBytes + 3);
    CHECK(out.substr(out.size() - 3) == "...");
}

TEST_CASE("TruncatePreview: very-long input still truncates to cutoff + 3") {
    const std::string huge(10000, 'q');
    const std::string out = TruncatePreview(huge);
    CHECK(out.size() == kRationalePreviewBytes + 3);
}
