// Bucket-A tests for the pure rich-content -> markdown helpers extracted from
// OfflineQueueService::TickOfflineFieldEdits during the decompose-top-20-monoliths phase-split.
// These cover OfflineFieldEditMergeDetail::{DetectRichKindIsAdf, RichToMarkdown,
// MineValueToMarkdown} in isolation (no cache, no network, no AppController).

#include "OfflineQueueService.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>
#include <string>

using namespace OfflineFieldEditMergeDetail;

TEST_CASE("DetectRichKindIsAdf: leading '{' after whitespace means ADF") {
    CHECK(DetectRichKindIsAdf("{\"type\":\"doc\"}"));
    CHECK(DetectRichKindIsAdf("   \n\t {\"type\":\"doc\"}"));
    CHECK_FALSE(DetectRichKindIsAdf("<p>hi</p>"));
    CHECK_FALSE(DetectRichKindIsAdf("plain markdown"));
    CHECK_FALSE(DetectRichKindIsAdf(""));
    CHECK_FALSE(DetectRichKindIsAdf("   "));
}

TEST_CASE("RichToMarkdown: empty input passes through unchanged") { CHECK(RichToMarkdown("") == ""); }

TEST_CASE("RichToMarkdown: plain (non-ADF, non-HTML) text passes through unchanged") {
    CHECK(RichToMarkdown("just some text") == "just some text");
}

TEST_CASE("RichToMarkdown: malformed JSON beginning with '{' falls through to passthrough") {
    // Not valid JSON -> parse throws internally -> neither ADF nor HTML -> returned as-is.
    const std::string malformed = "{not valid json";
    CHECK(RichToMarkdown(malformed) == malformed);
}

TEST_CASE("RichToMarkdown: ADF doc converts to markdown text") {
    const std::string adf =
        "{\"type\":\"doc\",\"version\":1,\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\","
        "\"text\":\"hello world\"}]}]}";
    const std::string md = RichToMarkdown(adf);
    CHECK(md.find("hello world") != std::string::npos);
}

TEST_CASE("RichToMarkdown: JSON object that is not an ADF doc is returned as-is") {
    // Valid JSON object but type != "doc": not converted, returned verbatim.
    const std::string notDoc = "{\"type\":\"panel\"}";
    CHECK(RichToMarkdown(notDoc) == notDoc);
}

TEST_CASE("MineValueToMarkdown: ADF doc object converts to markdown") {
    nlohmann::json doc;
    doc["type"] = "doc";
    doc["version"] = 1;
    nlohmann::json para;
    para["type"] = "paragraph";
    nlohmann::json textNode;
    textNode["type"] = "text";
    textNode["text"] = "mine text";
    para["content"] = nlohmann::json::array({textNode});
    doc["content"] = nlohmann::json::array({para});

    const std::string md = MineValueToMarkdown(doc);
    CHECK(md.find("mine text") != std::string::npos);
}

TEST_CASE("MineValueToMarkdown: non-object / non-string JSON yields empty string") {
    CHECK(MineValueToMarkdown(nlohmann::json(42)) == "");
    CHECK(MineValueToMarkdown(nlohmann::json(true)) == "");
    CHECK(MineValueToMarkdown(nlohmann::json::array({1, 2, 3})) == "");
}

TEST_CASE("MineValueToMarkdown: plain string value is preserved as markdown") {
    // A non-HTML string round-trips through the HTML-subset path and falls back to passthrough.
    const std::string md = MineValueToMarkdown(nlohmann::json("plain mine"));
    CHECK(md.find("plain mine") != std::string::npos);
}
