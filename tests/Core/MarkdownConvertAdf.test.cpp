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

// ---------------------------------------------------------------------------
// AdfToMarkdown per-block / per-inline handler goldens. Each TEST_CASE pins the
// byte-for-byte output of one block-type or inline-mark handler so the
// table-driven dispatch refactor of EmitAdfBlock / EmitInlineText is provably
// behaviour-preserving. AdfToMarkdown trims trailing whitespace/newlines, so the
// goldens have no trailing blank line.
// ---------------------------------------------------------------------------

namespace {

json AdfDoc(json content) {
    json d;
    d["type"] = "doc";
    d["content"] = std::move(content);
    return d;
}

json AdfText(const std::string& text) {
    json n;
    n["type"] = "text";
    n["text"] = text;
    return n;
}

json AdfPara(json content) {
    json p;
    p["type"] = "paragraph";
    p["content"] = std::move(content);
    return p;
}

std::string Adf2Md(const json& doc) { return MarkdownConvert::AdfToMarkdown(doc, nullptr); }

} // namespace

TEST_CASE("AdfToMarkdown: paragraph block") {
    const json doc = AdfDoc(json::array({AdfPara(json::array({AdfText("hello")}))}));
    CHECK(Adf2Md(doc) == "hello");
}

TEST_CASE("AdfToMarkdown: heading block clamps level 1-6") {
    json h;
    h["type"] = "heading";
    h["attrs"]["level"] = 2;
    h["content"] = json::array({AdfText("Title")});
    CHECK(Adf2Md(AdfDoc(json::array({h}))) == "## Title");

    json h9;
    h9["type"] = "heading";
    h9["attrs"]["level"] = 9;
    h9["content"] = json::array({AdfText("Big")});
    CHECK(Adf2Md(AdfDoc(json::array({h9}))) == "###### Big");
}

TEST_CASE("AdfToMarkdown: bulletList / listItem") {
    json li1;
    li1["type"] = "listItem";
    li1["content"] = json::array({AdfPara(json::array({AdfText("a")}))});
    json li2;
    li2["type"] = "listItem";
    li2["content"] = json::array({AdfPara(json::array({AdfText("b")}))});
    json list;
    list["type"] = "bulletList";
    list["content"] = json::array({li1, li2});
    CHECK(Adf2Md(AdfDoc(json::array({list}))) == "- a\n- b");
}

TEST_CASE("AdfToMarkdown: orderedList honours start and numbers items") {
    json li1;
    li1["type"] = "listItem";
    li1["content"] = json::array({AdfPara(json::array({AdfText("x")}))});
    json li2;
    li2["type"] = "listItem";
    li2["content"] = json::array({AdfPara(json::array({AdfText("y")}))});
    json list;
    list["type"] = "orderedList";
    list["attrs"]["order"] = 3;
    list["content"] = json::array({li1, li2});
    CHECK(Adf2Md(AdfDoc(json::array({list}))) == "3. x\n4. y");
}

TEST_CASE("AdfToMarkdown: codeBlock with language fence") {
    json code;
    code["type"] = "codeBlock";
    code["attrs"]["language"] = "cpp";
    code["content"] = json::array({AdfText("int x = 1;")});
    CHECK(Adf2Md(AdfDoc(json::array({code}))) == "```cpp\nint x = 1;\n```");
}

TEST_CASE("AdfToMarkdown: blockquote prefixes paragraph") {
    json bq;
    bq["type"] = "blockquote";
    bq["content"] = json::array({AdfPara(json::array({AdfText("quoted")}))});
    CHECK(Adf2Md(AdfDoc(json::array({bq}))) == "> quoted");
}

TEST_CASE("AdfToMarkdown: rule emits ---") {
    json rule;
    rule["type"] = "rule";
    CHECK(Adf2Md(AdfDoc(json::array({rule}))) == "---");
}

TEST_CASE("AdfToMarkdown: unknown block type is dropped") {
    json weird;
    weird["type"] = "panel";
    std::vector<std::string> dropped;
    const std::string md = MarkdownConvert::AdfToMarkdown(AdfDoc(json::array({weird})), &dropped);
    CHECK(md.empty());
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0] == "panel");
}

TEST_CASE("AdfToMarkdown: inline text marks (strong/em/strike)") {
    json t = AdfText("x");
    t["marks"] = json::array({json{{"type", "strong"}}, json{{"type", "em"}}});
    CHECK(Adf2Md(AdfDoc(json::array({AdfPara(json::array({t}))}))) == "***x***");

    json s = AdfText("y");
    s["marks"] = json::array({json{{"type", "strike"}}});
    CHECK(Adf2Md(AdfDoc(json::array({AdfPara(json::array({s}))}))) == "~~y~~");
}

TEST_CASE("AdfToMarkdown: inline code mark wins over emphasis") {
    json t = AdfText("z");
    t["marks"] = json::array({json{{"type", "strong"}}, json{{"type", "code"}}});
    CHECK(Adf2Md(AdfDoc(json::array({AdfPara(json::array({t}))}))) == "`z`");
}

TEST_CASE("AdfToMarkdown: inline link mark and autolink shortcut") {
    json link = AdfText("label");
    link["marks"] = json::array({json{{"type", "link"}, {"attrs", {{"href", "http://e.com"}}}}});
    CHECK(Adf2Md(AdfDoc(json::array({AdfPara(json::array({link}))}))) == "[label](http://e.com)");

    json bare = AdfText("http://e.com");
    bare["marks"] = json::array({json{{"type", "link"}, {"attrs", {{"href", "http://e.com"}}}}});
    CHECK(Adf2Md(AdfDoc(json::array({AdfPara(json::array({bare}))}))) == "http://e.com");
}

TEST_CASE("AdfToMarkdown: hardBreak inline node") {
    json hb;
    hb["type"] = "hardBreak";
    const std::string md = Adf2Md(AdfDoc(json::array({AdfPara(json::array({AdfText("a"), hb, AdfText("b")}))})));
    CHECK(md == "a  \nb");
}

TEST_CASE("AdfToMarkdown: mention falls back to id when text missing") {
    json m;
    m["type"] = "mention";
    m["attrs"]["id"] = "u123";
    CHECK(Adf2Md(AdfDoc(json::array({AdfPara(json::array({m}))}))) == "@u123");

    json m2;
    m2["type"] = "mention";
    m2["attrs"]["text"] = "@alice";
    CHECK(Adf2Md(AdfDoc(json::array({AdfPara(json::array({m2}))}))) == "@alice");
}

TEST_CASE("AdfToMarkdown: inlineCard renders bare url") {
    json card;
    card["type"] = "inlineCard";
    card["attrs"]["url"] = "http://card.example";
    CHECK(Adf2Md(AdfDoc(json::array({AdfPara(json::array({card}))}))) == "http://card.example");
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

// BACKLOG B5 — table cells used to emit only first-level paragraphs, running multiple paragraphs
// together and silently dropping lists. They now join blocks with <br> and represent list items.
namespace {
json AdfCell(const std::string& type, json content) {
    json c;
    c["type"] = type; // "tableCell" or "tableHeader"
    c["content"] = std::move(content);
    return c;
}
json AdfRow(json cells) {
    json r;
    r["type"] = "tableRow";
    r["content"] = std::move(cells);
    return r;
}
json AdfTable(json rows) {
    json t;
    t["type"] = "table";
    t["content"] = std::move(rows);
    return t;
}
json AdfListItemPara(const std::string& text) {
    json li;
    li["type"] = "listItem";
    li["content"] = json::array({AdfPara(json::array({AdfText(text)}))});
    return li;
}
} // namespace

TEST_CASE("AdfToMarkdown: table cell with multiple paragraphs joins with <br> (BACKLOG B5)") {
    json cell = AdfCell(
        "tableCell", json::array({AdfPara(json::array({AdfText("line1")})), AdfPara(json::array({AdfText("line2")}))}));
    const json table = AdfTable(json::array({AdfRow(json::array({cell}))}));
    CHECK(Adf2Md(AdfDoc(json::array({table}))) == "| line1<br>line2 |\n| --- |");
}

TEST_CASE("AdfToMarkdown: table cell with a bulletList preserves items instead of dropping (BACKLOG B5)") {
    json list;
    list["type"] = "bulletList";
    list["content"] = json::array({AdfListItemPara("a"), AdfListItemPara("b")});
    json cell = AdfCell("tableCell", json::array({list}));
    const json table = AdfTable(json::array({AdfRow(json::array({cell}))}));
    CHECK(Adf2Md(AdfDoc(json::array({table}))) == "| - a<br>- b |\n| --- |");
}

TEST_CASE("AdfToMarkdown: table header + rich body row (BACKLOG B5)") {
    json h1 = AdfCell("tableHeader", json::array({AdfPara(json::array({AdfText("Name")}))}));
    json h2 = AdfCell("tableHeader", json::array({AdfPara(json::array({AdfText("Detail")}))}));
    json body1 = AdfCell("tableCell",
                         json::array({AdfPara(json::array({AdfText("l1")})), AdfPara(json::array({AdfText("l2")}))}));
    json list;
    list["type"] = "bulletList";
    list["content"] = json::array({AdfListItemPara("x")});
    json body2 = AdfCell("tableCell", json::array({list}));
    const json table = AdfTable(json::array({AdfRow(json::array({h1, h2})), AdfRow(json::array({body1, body2}))}));
    CHECK(Adf2Md(AdfDoc(json::array({table}))) == "| Name | Detail |\n| --- | --- |\n| l1<br>l2 | - x |");
}
