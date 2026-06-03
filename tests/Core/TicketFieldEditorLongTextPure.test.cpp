#include <doctest/doctest.h>

#include "TicketFieldEditorLongTextPure.h"

#include <string>
#include <vector>

using TicketFieldEditorLongTextPure::ClassifyRichValue;
using TicketFieldEditorLongTextPure::ComputeLongTextSeed;
using TicketFieldEditorLongTextPure::ComputeRoundTripPreview;
using TicketFieldEditorLongTextPure::LongTextRichKind;
using TicketFieldEditorLongTextPure::RoundTripPreview;

TEST_CASE("ClassifyRichValue: empty and whitespace") {
    CHECK(ClassifyRichValue("") == LongTextRichKind::None);
    CHECK(ClassifyRichValue("   ") == LongTextRichKind::None);
    CHECK(ClassifyRichValue("\t\n\r ") == LongTextRichKind::None);
}

TEST_CASE("ClassifyRichValue: ADF detection requires type==doc") {
    SUBCASE("valid ADF doc object") {
        const std::string adf = R"({"type":"doc","version":1,"content":[]})";
        CHECK(ClassifyRichValue(adf) == LongTextRichKind::Adf);
    }
    SUBCASE("leading whitespace before brace is skipped") {
        const std::string adf = "  \n\t{\"type\":\"doc\",\"version\":1,\"content\":[]}";
        CHECK(ClassifyRichValue(adf) == LongTextRichKind::Adf);
    }
    SUBCASE("JSON object without type=doc is not classified as ADF") {
        CHECK(ClassifyRichValue(R"({"foo":"bar"})") == LongTextRichKind::None);
        CHECK(ClassifyRichValue(R"({"type":"paragraph"})") == LongTextRichKind::None);
    }
    SUBCASE("malformed JSON starting with { is not ADF (parse-with-allow-exceptions=false swallows)") {
        CHECK(ClassifyRichValue("{not valid json") == LongTextRichKind::None);
    }
}

TEST_CASE("ClassifyRichValue: HTML detection") {
    CHECK(ClassifyRichValue("<p>hi</p>") == LongTextRichKind::Html);
    CHECK(ClassifyRichValue("  <p>leading ws</p>") == LongTextRichKind::Html);
    CHECK(ClassifyRichValue("<!doctype html>") == LongTextRichKind::Html);
}

TEST_CASE("ClassifyRichValue: plain text falls through to None") {
    CHECK(ClassifyRichValue("just plain text") == LongTextRichKind::None);
    CHECK(ClassifyRichValue("1234567890") == LongTextRichKind::None);
}

TEST_CASE("ComputeLongTextSeed: None returns stripped fallback") {
    std::vector<std::string> dropped;
    bool rawMode = true;
    const std::string seed = ComputeLongTextSeed(LongTextRichKind::None, "", "stripped text here", dropped, rawMode);
    CHECK(seed == "stripped text here");
    CHECK(rawMode == false);
    CHECK(dropped.empty());
}

TEST_CASE("ComputeLongTextSeed: Adf with malformed JSON falls back to stripped value") {
    std::vector<std::string> dropped;
    bool rawMode = true;
    const std::string seed =
        ComputeLongTextSeed(LongTextRichKind::Adf, "{ not parseable", "fallback stripped", dropped, rawMode);
    CHECK(seed == "fallback stripped");
    CHECK(rawMode == false);
}

TEST_CASE("ComputeLongTextSeed: Adf with valid minimal ADF returns Markdown") {
    std::vector<std::string> dropped;
    bool rawMode = true;
    const std::string adf =
        R"({"type":"doc","version":1,"content":[{"type":"paragraph","content":[{"type":"text","text":"hello"}]}]})";
    const std::string seed = ComputeLongTextSeed(LongTextRichKind::Adf, adf, "should-not-be-used", dropped, rawMode);
    CHECK(seed.find("hello") != std::string::npos);
    CHECK(rawMode == false);
}

TEST_CASE("ComputeLongTextSeed: Html happy-path returns Markdown") {
    std::vector<std::string> dropped;
    bool rawMode = true;
    const std::string seed =
        ComputeLongTextSeed(LongTextRichKind::Html, "<p>hello world</p>", "fallback", dropped, rawMode);
    CHECK(seed.find("hello world") != std::string::npos);
    CHECK(rawMode == false);
    CHECK(dropped.empty());
}

TEST_CASE("ComputeLongTextSeed: clears outDroppedAdfNodeTypes on entry") {
    std::vector<std::string> dropped;
    dropped.push_back("pre-existing");
    dropped.push_back("more");
    bool rawMode = false;
    (void)ComputeLongTextSeed(LongTextRichKind::None, "", "x", dropped, rawMode);
    CHECK(dropped.empty());
}

TEST_CASE("ComputeLongTextSeed: resets outRawMode to false on entry, except on HTML fallback") {
    std::vector<std::string> dropped;
    bool rawMode = true;
    (void)ComputeLongTextSeed(LongTextRichKind::None, "", "x", dropped, rawMode);
    CHECK(rawMode == false);

    rawMode = true;
    const std::string adf = R"({"type":"doc","version":1,"content":[]})";
    (void)ComputeLongTextSeed(LongTextRichKind::Adf, adf, "x", dropped, rawMode);
    CHECK(rawMode == false);
}

TEST_CASE("ComputeRoundTripPreview: plain markdown survives the ADF round-trip non-lossy") {
    const RoundTripPreview rt = ComputeRoundTripPreview(LongTextRichKind::Adf, "Hello world");
    CHECK(rt.Rendered.find("Hello world") != std::string::npos);
    CHECK(rt.Lossy == false);
}

TEST_CASE("ComputeRoundTripPreview: None kind defaults to the ADF path") {
    const RoundTripPreview rt = ComputeRoundTripPreview(LongTextRichKind::None, "# Heading");
    CHECK(rt.Rendered.find("Heading") != std::string::npos);
    CHECK(rt.Lossy == false);
}

TEST_CASE("ComputeRoundTripPreview: Html kind round-trips through the HTML subset converter") {
    const RoundTripPreview rt = ComputeRoundTripPreview(LongTextRichKind::Html, "Plain paragraph text");
    CHECK(rt.Rendered.find("Plain paragraph text") != std::string::npos);
    // A plain paragraph is fully representable in the HTML subset → not lossy.
    CHECK(rt.Lossy == false);
}

TEST_CASE("ComputeRoundTripPreview: empty markdown yields empty render, not lossy") {
    const RoundTripPreview rt = ComputeRoundTripPreview(LongTextRichKind::Adf, "");
    CHECK(rt.Lossy == false);
}
