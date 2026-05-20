#include <doctest/doctest.h>

#include "MarkdownConvert.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <numeric>

using nlohmann::json;

namespace {

const json* FindFirst(const json& node, const std::string& type) {
    if (node.is_object()) {
        if (node.value("type", std::string()) == type)
            return &node;
        if (node.contains("content"))
            return FindFirst(node["content"], type);
        return nullptr;
    }
    if (node.is_array()) {
        const json* hit = nullptr;
        (void)std::find_if(node.begin(), node.end(), [&](const json& child) {
            hit = FindFirst(child, type);
            return hit != nullptr;
        });
        return hit;
    }
    return nullptr;
}

int CountByType(const json& node, const std::string& type) {
    if (node.is_object()) {
        const int self = (node.value("type", std::string()) == type) ? 1 : 0;
        const int kids = node.contains("content") ? CountByType(node["content"], type) : 0;
        return self + kids;
    }
    if (node.is_array()) {
        return std::accumulate(node.begin(), node.end(), 0,
                               [&](int acc, const json& child) { return acc + CountByType(child, type); });
    }
    return 0;
}

} // namespace

TEST_CASE("MarkdownToAdf: nested blockquote flattens to a single blockquote") {
    // Jira ADF blockquote content schema disallows blockquote child. Markdown
    // `> >` style nesting must collapse into the outer blockquote so the payload
    // doesn't HTTP-400 with INVALID_INPUT.
    const std::string md = "> Outer line.\n"
                           "> > Inner line.\n"
                           "> > \n"
                           "> > - With a list\n"
                           "> > - inside\n";
    const json adf = MarkdownConvert::MarkdownToAdf(md);

    CHECK(CountByType(adf, "blockquote") == 1);

    const json* bq = FindFirst(adf, "blockquote");
    REQUIRE(bq != nullptr);
    REQUIRE(bq->contains("content"));
    for (const auto& child : (*bq)["content"]) {
        const std::string t = child.value("type", std::string());
        // Only paragraph / bulletList / orderedList are valid blockquote children in Jira ADF.
        CHECK((t == "paragraph" || t == "bulletList" || t == "orderedList"));
    }
}

TEST_CASE("MarkdownToAdf: external image becomes a text link, not mediaInline") {
    // Jira ADF mediaInline requires a file-store `id` + type "file" | "link".
    // External URLs are not accepted; the payload HTTP-400s with INVALID_INPUT.
    // Fallback: emit a text node with a link mark so the URL survives validation.
    const std::string md = "![Alt text](https://via.placeholder.com/150)";
    const json adf = MarkdownConvert::MarkdownToAdf(md);

    CHECK(CountByType(adf, "mediaInline") == 0);

    const json* text = FindFirst(adf, "text");
    REQUIRE(text != nullptr);
    CHECK(text->value("text", std::string()) == "Alt text");
    REQUIRE(text->contains("marks"));
    REQUIRE((*text)["marks"].is_array());
    REQUIRE_FALSE((*text)["marks"].empty());
    const auto& mark = (*text)["marks"][0];
    CHECK(mark.value("type", std::string()) == "link");
    CHECK(mark["attrs"].value("href", std::string()) == "https://via.placeholder.com/150");
}

TEST_CASE("MarkdownToAdf: nested list never lands inside paragraph content") {
    // Jira ADF paragraph content is `inline*`. md4c emits a nested bulletList as a sibling
    // block inside the parent listItem; the prior wrap helper merged the whole sibling array
    // into one paragraph, putting bulletList inside paragraph and HTTP-400ing the PUT.
    const std::string md = "- Outer\n"
                           "  - Inner one\n"
                           "  - Inner two\n";
    const json adf = MarkdownConvert::MarkdownToAdf(md);

    const json* outerList = FindFirst(adf, "bulletList");
    REQUIRE(outerList != nullptr);
    REQUIRE((*outerList)["content"].is_array());
    REQUIRE_FALSE((*outerList)["content"].empty());
    const json& outerItem = (*outerList)["content"][0];
    REQUIRE(outerItem.value("type", std::string()) == "listItem");
    REQUIRE(outerItem["content"].is_array());

    // listItem content must be block*; check no paragraph has a block-typed child.
    for (const auto& block : outerItem["content"]) {
        if (block.value("type", std::string()) == "paragraph") {
            REQUIRE(block["content"].is_array());
            for (const auto& inl : block["content"]) {
                const std::string t = inl.value("type", std::string());
                CHECK((t == "text" || t == "hardBreak" || t == "mention" || t == "emoji" || t == "mediaInline" ||
                       t == "inlineCard"));
            }
        }
    }
}

TEST_CASE("MarkdownToAdf: attachment image keeps mediaInline with type=file") {
    // Regression guard: the external-URL fallback must not affect attachment refs.
    const std::string md = "![Screenshot](attachment:abc-123)";
    const json adf = MarkdownConvert::MarkdownToAdf(md);

    const json* media = FindFirst(adf, "mediaInline");
    REQUIRE(media != nullptr);
    REQUIRE(media->contains("attrs"));
    CHECK((*media)["attrs"].value("type", std::string()) == "file");
    CHECK((*media)["attrs"].value("id", std::string()) == "abc-123");
    CHECK((*media)["attrs"].value("alt", std::string()) == "Screenshot");
}
