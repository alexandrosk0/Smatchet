// AnnotateContextCommentPure doctest — the Annotate-context comment as Markdown, so it posts through the
// ordinary comment path and the offline queue (Quality Pillar 6). Asserted through the real Jira
// Markdown→ADF conversion (TrackerFieldPayloadPure::AdfCommentBodyFromMarkdown): the comment still has
// the shape the retired Jira-only builder posted — a header paragraph, an optional note paragraph and
// a `cpp` codeBlock — with punctuation-heavy text kept literal and the snippet kept inside one block.

#include "Tracker/AnnotateContextCommentPure.h"
#include "Tracker/TrackerFieldPayloadPure.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

using smatchet::tracker::AnnotateContextFields;
using smatchet::tracker::BuildAnnotateContextCommentMarkdown;

namespace {

AnnotateContextFields Sample() {
    AnnotateContextFields f;
    f.P4User = "p4user";
    f.FunctionName = "MyFunction";
    f.FilePath = "//depot/src/foo.cpp";
    f.LineNumber = 42;
    f.Changelist = "12345";
    f.Date = "2026-06-01";
    f.CodeSnippet = "int foo() { return 0; }";
    return f;
}

// The text of an ADF block: its text nodes joined (a parser may split a run at an escape).
std::string BlockText(const nlohmann::json& block) {
    std::string text;
    if (!block.contains("content") || !block["content"].is_array()) {
        return text;
    }
    for (const nlohmann::json& node : block["content"]) {
        if (node.value("type", std::string()) == "text") {
            text += node.value("text", std::string());
        }
    }
    return text;
}

nlohmann::json ToAdf(const AnnotateContextFields& f) {
    return TrackerFieldPayloadPure::AdfCommentBodyFromMarkdown(BuildAnnotateContextCommentMarkdown(f));
}

} // namespace

TEST_CASE("Annotate-context comment — header paragraph and a cpp code block") {
    const nlohmann::json doc = ToAdf(Sample());
    REQUIRE(doc["content"].is_array());
    REQUIRE(doc["content"].size() == 2);
    CHECK(doc["content"][0]["type"] == "paragraph");
    CHECK(BlockText(doc["content"][0]) ==
          "Annotate \xE2\x80\x94 p4user | MyFunction | //depot/src/foo.cpp:42 | CL 12345 | 2026-06-01");
    const nlohmann::json& code = doc["content"][1];
    CHECK(code["type"] == "codeBlock");
    CHECK(code["attrs"]["language"] == "cpp");
    const std::string codeText = BlockText(code);
    CHECK(codeText.find("L42  CL:12345  p4user\nint foo() { return 0; }") == 0);
}

TEST_CASE("Annotate-context comment — an approximated line adds a note paragraph") {
    AnnotateContextFields f = Sample();
    f.Approximated = true;
    const nlohmann::json doc = ToAdf(f);
    REQUIRE(doc["content"].size() == 3);
    CHECK(doc["content"][1]["type"] == "paragraph");
    CHECK(BlockText(doc["content"][1]) == "Note: annotate is approximated (exact line not found in annotate).");
    CHECK(doc["content"][2]["type"] == "codeBlock");
}

TEST_CASE("Annotate-context comment — Markdown syntax in the fields stays literal text") {
    AnnotateContextFields f = Sample();
    f.P4User = "jane_doe";
    f.FunctionName = "Foo<int>::operator*";
    f.FilePath = "src/*draft*/[old] #1.cpp";
    const nlohmann::json doc = ToAdf(f);
    REQUIRE(doc["content"].size() == 2);
    CHECK(doc["content"][0]["type"] == "paragraph");
    CHECK(BlockText(doc["content"][0]) == "Annotate \xE2\x80\x94 jane_doe | Foo<int>::operator* | src/*draft*/[old] "
                                          "#1.cpp:42 | CL 12345 | 2026-06-01");
}

TEST_CASE("Annotate-context comment — a snippet with a code fence stays inside one code block") {
    AnnotateContextFields f = Sample();
    f.CodeSnippet = "const char* md = \"```cpp\\n\";\n````\nint after = 1;";
    const std::string md = BuildAnnotateContextCommentMarkdown(f);
    CHECK(md.find("`````cpp\n") != std::string::npos); // one backtick longer than the snippet's longest run
    const nlohmann::json doc = ToAdf(f);
    REQUIRE(doc["content"].size() == 2);
    REQUIRE(doc["content"][1]["type"] == "codeBlock");
    const std::string codeText = BlockText(doc["content"][1]);
    CHECK(codeText.find("````\nint after = 1;") != std::string::npos);
}
