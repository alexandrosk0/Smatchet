// ADF -> Markdown (recursive walker) + HTML-subset -> Markdown state machine. Split out of
// MarkdownConvert.cpp (see the god-file-splits plan). Behavior-identical body move;
// state + entry-point declarations live in MarkdownConvert_Internal.h.

#include "MarkdownConvert.h"
#include "MarkdownConvert_Internal.h"

#include "Logger.h"

extern "C" {
#include "md4c.h"
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using nlohmann::json;

namespace MarkdownConvert {
namespace md_detail {

void AppendIndent(std::ostringstream& out, int spaces) {
    for (int i = 0; i < spaces; ++i)
        out << ' ';
}

// Per-node-type inline emitters for the ADF→Markdown walk. `EmitInlineText` is a
// thin dispatch that looks up a handler keyed by node `type` and invokes it
// against the shared output stream. Every handler emits exactly the bytes the
// original if-else branch produced — output is byte-for-byte identical (pinned
// by tests/Core/MarkdownConvert.test.cpp / MarkdownConvertAdf.test.cpp).

// Collect the emphasis wrappers and link/code state for a text node's `marks`
// array. `isCode` short-circuits other emphasis (inline code wins).
void CollectInlineMarks(const json& node, std::vector<const char*>& openWrap, std::vector<const char*>& closeWrap,
                        std::string& href, bool& isCode) {
    if (!node.contains("marks") || !node["marks"].is_array())
        return;
    for (const auto& m : node["marks"]) {
        const std::string mt = m.value("type", std::string());
        if (mt == "strong") {
            openWrap.push_back("**");
            closeWrap.push_back("**");
        } else if (mt == "em") {
            openWrap.push_back("*");
            closeWrap.push_back("*");
        } else if (mt == "strike") {
            openWrap.push_back("~~");
            closeWrap.push_back("~~");
        } else if (mt == "code") {
            isCode = true;
        } else if (mt == "link") {
            if (m.contains("attrs") && m["attrs"].is_object()) {
                href = m["attrs"].value("href", std::string());
            }
        }
    }
}

void EmitInlineTextNode(const json& node, std::ostringstream& out) {
    // nlohmann value() throws type_error when the key exists but is not a string
    // (e.g. a malformed server node with a numeric "text"), which would escape
    // AdfToMarkdown and abort the offline-queue merge. Read it type-safely.
    std::string text =
        (node.contains("text") && node["text"].is_string()) ? node["text"].get<std::string>() : std::string();
    // Apply marks innermost-first when emitting; ADF stores marks innermost-last in its array.
    // Scratch buffers are thread_local + const char* (no std::string heap churn on the hot
    // text-node path); capacity persists across calls within a thread.
    static thread_local std::vector<const char*> openWrap;
    static thread_local std::vector<const char*> closeWrap;
    openWrap.clear();
    closeWrap.clear();
    std::string href;
    bool isCode = false;
    CollectInlineMarks(node, openWrap, closeWrap, href, isCode);
    if (isCode) {
        // Inline code wins: drop other emphasis on this run.
        out << "`" << text << "`";
        return;
    }
    for (const auto& w : openWrap)
        out << w;
    // Autolink shortcut: when the visible text equals the link target, emit a bare URL
    // instead of [url](url). md4c's MD_FLAG_PERMISSIVEAUTOLINKS turns it back into a link
    // on save, so round-trip semantics are preserved with a cleaner editor surface.
    const bool autolink = !href.empty() && text == href;
    if (!href.empty() && !autolink)
        out << "[";
    out << text;
    if (!href.empty() && !autolink) {
        out << "](" << href << ")";
    }
    for (auto it = closeWrap.rbegin(); it != closeWrap.rend(); ++it) {
        out << *it;
    }
}

void EmitInlineHardBreak(const json&, std::ostringstream& out) { out << "  \n"; }

void EmitInlineEmoji(const json& node, std::ostringstream& out) {
    out << node.value("attrs", json::object()).value("shortName", node.value("text", std::string("")));
}

void EmitInlineCard(const json& node, std::ostringstream& out) {
    // Jira smart link / Atlassian inline card. Render as a bare URL — Markdown's permissive
    // autolinks turn it back into a hyperlink on save, so the round-trip preserves the link
    // semantics with the cleanest possible editor surface (one URL, no [text](url) wrapping).
    // The smart-card rendering is lost on save (becomes a regular hyperlink in ADF).
    if (node.contains("attrs") && node["attrs"].is_object()) {
        const std::string url = node["attrs"].value("url", std::string());
        if (!url.empty()) {
            out << url;
        }
    }
}

void EmitInlineMention(const json& node, std::ostringstream& out) {
    // ADF @-mention. Use the display text from attrs.text; fall back to id if missing.
    if (node.contains("attrs") && node["attrs"].is_object()) {
        std::string mtxt = node["attrs"].value("text", std::string());
        if (mtxt.empty())
            mtxt = std::string("@") + node["attrs"].value("id", std::string());
        out << mtxt;
    }
}

void EmitInlineMedia(const json& node, std::ostringstream& out) {
    const json attrs = node.value("attrs", json::object());
    std::string url = attrs.value("url", std::string());
    if (url.empty()) {
        const std::string id = attrs.value("id", std::string());
        if (!id.empty())
            url = "attachment:" + id;
    }
    std::string alt = attrs.value("alt", std::string());
    std::string escAlt;
    escAlt.reserve(alt.size() + 4);
    for (char ch : alt) {
        if (ch == ']' || ch == '\\')
            escAlt += '\\';
        escAlt += ch;
    }
    out << "![" << escAlt << "](" << url << ")";
}

using InlineEmitter = void (*)(const json&, std::ostringstream&);

const std::unordered_map<std::string, InlineEmitter>& InlineEmitters() {
    static const std::unordered_map<std::string, InlineEmitter> m = {
        {"text", EmitInlineTextNode},   {"hardBreak", EmitInlineHardBreak}, {"emoji", EmitInlineEmoji},
        {"inlineCard", EmitInlineCard}, {"mention", EmitInlineMention},     {"mediaInline", EmitInlineMedia},
        {"media", EmitInlineMedia},
    };
    return m;
}

void EmitInlineText(const json& node, std::ostringstream& out) {
    if (!node.is_object())
        return;
    const std::string type = node.value("type", std::string());
    const auto& emitters = InlineEmitters();
    const auto it = emitters.find(type);
    if (it != emitters.end())
        it->second(node, out);
}

static bool MatchStoredTaskPrefix(const json& paraContent, bool* doneOut) {
    if (!paraContent.is_array() || paraContent.empty())
        return false;
    if (paraContent[0].value("type", std::string()) != "text")
        return false;
    // A malformed node can have the text type but no string text value. Reading it as a
    // string would raise a JSON type error that escapes the ADF-to-Markdown conversion,
    // which has no catch, and aborts the offline-queue merge — so guard first.
    if (!paraContent[0].contains("text") || !paraContent[0]["text"].is_string())
        return false;
    const std::string& t = paraContent[0]["text"].get_ref<const std::string&>();
    if (t.size() >= 4 && t.compare(0, 4, "[ ] ") == 0) {
        *doneOut = false;
        return true;
    }
    if (t.size() >= 4 && t.compare(0, 4, "[x] ") == 0) {
        *doneOut = true;
        return true;
    }
    if (t.size() >= 4 && t.compare(0, 4, "[X] ") == 0) {
        *doneOut = true;
        return true;
    }
    return false;
}

static void EmitParagraphInlineSkipTaskPrefix(const json& paraContent, std::ostringstream& out) {
    if (!paraContent.is_array())
        return;
    constexpr size_t kSkip = 4;
    bool firstText = true;
    for (const auto& c : paraContent) {
        if (firstText && c.is_object() && c.value("type", std::string()) == "text") {
            const std::string t = c.value("text", std::string());
            if (t.size() > kSkip) {
                json c2 = c;
                c2["text"] = t.substr(kSkip);
                EmitInlineText(c2, out);
            } else if (t.size() < kSkip) {
                EmitInlineText(c, out);
            }
            firstText = false;
            continue;
        }
        EmitInlineText(c, out);
        if (c.is_object() && c.value("type", std::string()) == "text")
            firstText = false;
    }
}

void EmitInlineRun(const json& contentArr, std::ostringstream& out) {
    if (!contentArr.is_array())
        return;
    for (const auto& child : contentArr) {
        EmitInlineText(child, out);
    }
}

// Flatten one inline run to a single line (GFM cells are single-line — collapse any newline).
static std::string MarkdownCellFlattenInline(const json& inlineArr) {
    std::ostringstream o;
    EmitInlineRun(inlineArr, o);
    std::string t = o.str();
    std::replace_if(t.begin(), t.end(), [](char ch) { return ch == '\n' || ch == '\r'; }, ' ');
    return t;
}

// BACKLOG B5: a GFM table cell is single-line and holds no block content, so ADF cell blocks
// must be flattened. The prior version emitted only first-level paragraphs — silently dropping
// lists and running multiple paragraphs together. Instead, collect each block's inline text and
// join blocks with an HTML `<br>` (the GFM-safe in-cell line break), and represent list items
// with a marker, so multiple paragraphs and lists survive the ADF→Markdown conversion instead
// of being merged or dropped. Deeper fidelity (code blocks, nested lists/tables inside a cell)
// remains tracked in RICH_TEXT_EDITING_V2.
static std::string MarkdownCellPlainInner(const json& cell) {
    std::vector<std::string> segments;
    if (cell.contains("content") && cell["content"].is_array()) {
        for (const auto& blk : cell["content"]) {
            const std::string bt = blk.value("type", std::string());
            if (bt == "paragraph") {
                std::string t = MarkdownCellFlattenInline(blk.value("content", json::array()));
                if (!t.empty())
                    segments.push_back(std::move(t));
            } else if (bt == "bulletList" || bt == "orderedList") {
                const bool ordered = (bt == "orderedList");
                int order = 1;
                if (ordered && blk.contains("attrs") && blk["attrs"].is_object())
                    order = blk["attrs"].value("order", 1);
                if (blk.contains("content") && blk["content"].is_array()) {
                    for (const auto& li : blk["content"]) {
                        if (li.value("type", std::string()) != "listItem")
                            continue;
                        std::ostringstream lo;
                        if (li.contains("content") && li["content"].is_array()) {
                            for (const auto& lblk : li["content"]) {
                                if (lblk.value("type", std::string()) == "paragraph")
                                    EmitInlineRun(lblk.value("content", json::array()), lo);
                            }
                        }
                        std::string t = lo.str();
                        std::replace_if(t.begin(), t.end(), [](char ch) { return ch == '\n' || ch == '\r'; }, ' ');
                        if (!t.empty())
                            segments.push_back((ordered ? std::to_string(order) + ". " : "- ") + t);
                        if (ordered)
                            ++order;
                    }
                }
            }
            // Other block types (codeBlock, nested table / mediaSingle) remain unrepresented — see B5.
        }
    }
    std::string joined;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i != 0)
            joined += "<br>";
        joined += segments[i];
    }
    return joined;
}

static void EmitMarkdownTable(const json& table, AdfWalkState& s) {
    if (!table.contains("content") || !table["content"].is_array())
        return;
    std::vector<std::vector<std::string>> rows;
    for (const auto& row : table["content"]) {
        if (row.value("type", std::string()) != "tableRow")
            continue;
        std::vector<std::string> cells;
        if (row.contains("content") && row["content"].is_array()) {
            for (const auto& cell : row["content"]) {
                const std::string ct = cell.value("type", std::string());
                if (ct == "tableHeader" || ct == "tableCell") {
                    cells.push_back(MarkdownCellPlainInner(cell));
                }
            }
        }
        if (!cells.empty())
            rows.push_back(std::move(cells));
    }
    if (rows.empty())
        return;
    const size_t ncol = std::accumulate(rows.begin(), rows.end(), size_t{},
                                        [](size_t acc, const auto& r) { return (std::max)(acc, r.size()); });
    auto escPipe = [](std::string c) {
        for (size_t i = 0; i < c.size(); ++i) {
            if (c[i] == '|') {
                c.insert(i, "\\");
                ++i;
            }
        }
        return c;
    };
    AppendIndent(s.out, s.listIndent);
    bool firstRow = true;
    for (const auto& r : rows) {
        s.out << '|';
        for (size_t i = 0; i < ncol; ++i) {
            s.out << ' ';
            if (i < r.size())
                s.out << escPipe(r[i]);
            s.out << " |";
        }
        s.out << '\n';
        if (firstRow) {
            s.out << '|';
            for (size_t i = 0; i < ncol; ++i)
                s.out << " --- |";
            s.out << '\n';
            firstRow = false;
        }
    }
    s.out << '\n';
}

void EmitAdfBlock(const json& node, AdfWalkState& s);

void EmitAdfChildren(const json& node, AdfWalkState& s) {
    if (!node.is_object())
        return;
    if (!node.contains("content") || !node["content"].is_array())
        return;
    for (const auto& child : node["content"]) {
        EmitAdfBlock(child, s);
    }
}

// Table-driven ADF-block → Markdown dispatch. `EmitAdfBlock` is a thin lookup
// that, per node `type`, finds a handler in `AdfBlockHandlers()` and invokes it
// against the shared `AdfWalkState`. Unknown types record a dropped-node entry.
// Every handler emits exactly the bytes the original if-else branch produced —
// output is byte-for-byte identical (pinned by the MarkdownConvert tests).

void EmitAdfParagraph(const json& node, AdfWalkState& s) {
    AppendIndent(s.out, s.listIndent);
    if (s.insideBlockquote)
        s.out << "> ";
    EmitInlineRun(node.value("content", json::array()), s.out);
    s.out << "\n\n";
}

void EmitAdfHeading(const json& node, AdfWalkState& s) {
    const int level = (node.contains("attrs") && node["attrs"].is_object()) ? node["attrs"].value("level", 1) : 1;
    const int clamped = (std::max)(1, (std::min)(6, level));
    for (int i = 0; i < clamped; ++i)
        s.out << '#';
    s.out << ' ';
    EmitInlineRun(node.value("content", json::array()), s.out);
    s.out << "\n\n";
}

void EmitAdfBulletList(const json& node, AdfWalkState& s) {
    s.listMarkerStack.push_back('-');
    s.orderedCounters.push_back(0);
    EmitAdfChildren(node, s);
    s.listMarkerStack.pop_back();
    s.orderedCounters.pop_back();
    if (s.listIndent == 0)
        s.out << "\n";
}

void EmitAdfOrderedList(const json& node, AdfWalkState& s) {
    const int start = (node.contains("attrs") && node["attrs"].is_object()) ? node["attrs"].value("order", 1) : 1;
    s.listMarkerStack.push_back('1');
    s.orderedCounters.push_back(start - 1);
    EmitAdfChildren(node, s);
    s.listMarkerStack.pop_back();
    s.orderedCounters.pop_back();
    if (s.listIndent == 0)
        s.out << "\n";
}

// Emit the list-item marker ("1. ", "- ", or their task-list variants).
void EmitListItemMarker(AdfWalkState& s, bool taskItem, bool taskDone) {
    const char marker = s.listMarkerStack.empty() ? '-' : s.listMarkerStack.back();
    if (marker == '1') {
        ++s.orderedCounters.back();
        s.out << s.orderedCounters.back();
        if (taskItem) {
            s.out << (taskDone ? ". [x] " : ". [ ] ");
        } else {
            s.out << ". ";
        }
    } else {
        if (taskItem) {
            s.out << (taskDone ? "- [x] " : "- [ ] ");
        } else {
            s.out << "- ";
        }
    }
}

// Render listItem children inline-style: the first paragraph stays on the marker
// line, subsequent blocks indent.
void EmitListItemChildren(const json& node, AdfWalkState& s, bool taskItem) {
    if (!node.contains("content") || !node["content"].is_array()) {
        s.out << "\n";
        return;
    }
    const auto& children = node["content"];
    bool first = true;
    const int prevIndent = s.listIndent;
    for (const auto& c : children) {
        const std::string ct = c.is_object() ? c.value("type", std::string()) : std::string();
        if (first && ct == "paragraph") {
            if (taskItem) {
                EmitParagraphInlineSkipTaskPrefix(c.value("content", json::array()), s.out);
            } else {
                EmitInlineRun(c.value("content", json::array()), s.out);
            }
            s.out << "\n";
            first = false;
        } else {
            s.listIndent = prevIndent + 2;
            EmitAdfBlock(c, s);
            s.listIndent = prevIndent;
            first = false;
        }
    }
    if (first) {
        // Empty list item — still need newline.
        s.out << "\n";
    }
}

void EmitAdfListItem(const json& node, AdfWalkState& s) {
    AppendIndent(s.out, s.listIndent);
    bool taskItem = false;
    bool taskDone = false;
    if (node.contains("content") && node["content"].is_array() && !node["content"].empty()) {
        const auto& fc = node["content"][0];
        if (fc.is_object() && fc.value("type", std::string()) == "paragraph") {
            taskItem = MatchStoredTaskPrefix(fc.value("content", json::array()), &taskDone);
        }
    }
    EmitListItemMarker(s, taskItem, taskDone);
    EmitListItemChildren(node, s, taskItem);
}

void EmitAdfMediaSingle(const json& node, AdfWalkState& s) {
    AppendIndent(s.out, s.listIndent);
    if (node.contains("content") && node["content"].is_array()) {
        for (const auto& ch : node["content"]) {
            if (ch.value("type", std::string()) == "media") {
                EmitInlineText(ch, s.out);
            }
        }
    }
    s.out << "\n\n";
}

void EmitAdfTable(const json& node, AdfWalkState& s) { EmitMarkdownTable(node, s); }

void EmitAdfCodeBlock(const json& node, AdfWalkState& s) {
    std::string lang;
    if (node.contains("attrs") && node["attrs"].is_object()) {
        lang = node["attrs"].value("language", std::string());
    }
    AppendIndent(s.out, s.listIndent);
    s.out << "```" << lang << "\n";
    if (node.contains("content") && node["content"].is_array()) {
        for (const auto& c : node["content"]) {
            if (c.is_object() && c.value("type", std::string()) == "text") {
                s.out << c.value("text", std::string());
            }
        }
    }
    if (s.out.tellp() > 0) {
        const std::string current = s.out.str();
        if (!current.empty() && current.back() != '\n')
            s.out << '\n';
    }
    AppendIndent(s.out, s.listIndent);
    s.out << "```\n\n";
}

void EmitAdfBlockquote(const json& node, AdfWalkState& s) {
    const bool prev = s.insideBlockquote;
    s.insideBlockquote = true;
    EmitAdfChildren(node, s);
    s.insideBlockquote = prev;
}

void EmitAdfRule(const json&, AdfWalkState& s) { s.out << "---\n\n"; }

void EmitAdfHardBreak(const json&, AdfWalkState& s) { s.out << "  \n"; }

void EmitAdfDoc(const json& node, AdfWalkState& s) { EmitAdfChildren(node, s); }

using AdfBlockEmitter = void (*)(const json&, AdfWalkState&);

const std::unordered_map<std::string, AdfBlockEmitter>& AdfBlockHandlers() {
    static const std::unordered_map<std::string, AdfBlockEmitter> m = {
        {"paragraph", EmitAdfParagraph},   {"heading", EmitAdfHeading},
        {"bulletList", EmitAdfBulletList}, {"orderedList", EmitAdfOrderedList},
        {"listItem", EmitAdfListItem},     {"mediaSingle", EmitAdfMediaSingle},
        {"table", EmitAdfTable},           {"codeBlock", EmitAdfCodeBlock},
        {"blockquote", EmitAdfBlockquote}, {"rule", EmitAdfRule},
        {"hardBreak", EmitAdfHardBreak},   {"doc", EmitAdfDoc},
    };
    return m;
}

void EmitAdfBlock(const json& node, AdfWalkState& s) {
    if (!node.is_object()) {
        return;
    }
    // Depth cap: every nested ADF node re-enters here (EmitAdfChildren / list /
    // table / blockquote handlers all funnel back through EmitAdfBlock), so one
    // guard at this chokepoint bounds the whole mutual recursion. A hostile
    // deeply-nested ADF tree (server-supplied) would otherwise overflow the C++
    // stack — Pillar 3. 256 matches json_safe::kDefaultMaxDepth.
    constexpr int kMaxAdfDepth = 256;
    if (s.depth > kMaxAdfDepth) {
        s.dropped.push_back("<depth-capped>");
        return;
    }
    ++s.depth;
    const std::string type = node.value("type", std::string());
    const auto& handlers = AdfBlockHandlers();
    const auto it = handlers.find(type);
    if (it != handlers.end()) {
        it->second(node, s);
    } else {
        s.dropped.push_back(type.empty() ? std::string("<unknown>") : type);
    }
    --s.depth;
}

// HTML subset -> Markdown (state machine over a small tag allowlist)

const std::unordered_set<std::string>& HtmlAllowedTags() {
    static const std::unordered_set<std::string> tags = {
        "p",
        "br",
        "hr",
        "h1",
        "h2",
        "h3",
        "h4",
        "h5",
        "h6",
        "strong",
        "b",
        "em",
        "i",
        "s",
        "del",
        "u",
        "code",
        "pre",
        "ul",
        "ol",
        "li",
        "a",
        "blockquote",
        "div",
        "span", // soft-allowed:
                // stripped to
                // plain text
                // wrapping
        "table",
        "thead",
        "tbody",
        "tr",
        "th",
        "td",
        "img",
    };
    return tags;
}

std::string DecodeHtmlEntities(const std::string& s) {
    // Static map built once; O(1) lookup replaces O(entity-list-len) linear scan (§2.2).
    static const std::unordered_map<std::string, char> kNamedEntities = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"#39", '\''}, {"nbsp", ' '},
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            const size_t end = s.find(';', i);
            if (end != std::string::npos && end - i <= 8) {
                const std::string ent = s.substr(i + 1, end - i - 1);
                const auto it = kNamedEntities.find(ent);
                if (it != kNamedEntities.end()) {
                    out += it->second;
                    i = end + 1;
                    continue;
                }
                if (!ent.empty() && ent[0] == '#') {
                    int code = 0;
                    if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X')) {
                        for (size_t k = 2; k < ent.size(); ++k) {
                            const char c = ent[k];
                            if (c >= '0' && c <= '9')
                                code = code * 16 + (c - '0');
                            else if (c >= 'a' && c <= 'f')
                                code = code * 16 + (c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F')
                                code = code * 16 + (c - 'A' + 10);
                            else {
                                code = 0;
                                break;
                            }
                        }
                    } else {
                        for (size_t k = 1; k < ent.size(); ++k) {
                            const char c = ent[k];
                            if (c < '0' || c > '9') {
                                code = 0;
                                break;
                            }
                            code = code * 10 + (c - '0');
                        }
                    }
                    if (code > 0 && code < 128) {
                        out += static_cast<char>(code);
                        i = end + 1;
                        continue;
                    }
                }
            }
        }
        out += s[i];
        ++i;
    }
    return out;
}

struct HtmlTagToken {
    std::string name;          // lowercased tag name
    bool isClose = false;      // true for </tag>
    bool isSelfClosed = false; // true for <tag/>
    std::string href;          // populated for <a> opens
    std::string src;           // <img>
    std::string alt;           // <img>
};

// Consume one whitespace-delimited tag-name run (alnum + '-'), lowercased.
// Advances `pos` past the run and returns the collected name.
std::string ConsumeHtmlTagName(const std::string& html, size_t& pos) {
    std::string name;
    while (pos < html.size() && (std::isalnum(static_cast<unsigned char>(html[pos])) || html[pos] == '-')) {
        name += static_cast<char>(std::tolower(static_cast<unsigned char>(html[pos])));
        ++pos;
    }
    return name;
}

// Parse one `name[=value]` attribute starting at `pos`. `pos` must point at a
// non-space, non-'/' attribute-name char. Fills `attrName` (lowercased) and
// `attrValue` (raw, undecoded) and advances `pos` past the attribute.
void ParseOneHtmlAttribute(const std::string& html, size_t& pos, std::string& attrName, std::string& attrValue) {
    while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) && html[pos] != '=' &&
           html[pos] != '>' && html[pos] != '/') {
        attrName += static_cast<char>(std::tolower(static_cast<unsigned char>(html[pos])));
        ++pos;
    }
    if (pos < html.size() && html[pos] == '=') {
        ++pos;
        if (pos < html.size() && (html[pos] == '"' || html[pos] == '\'')) {
            const char quote = html[pos++];
            while (pos < html.size() && html[pos] != quote) {
                attrValue += html[pos++];
            }
            if (pos < html.size())
                ++pos;
        } else {
            while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) && html[pos] != '>' &&
                   html[pos] != '/') {
                attrValue += html[pos++];
            }
        }
    }
}

// Skip a comment / declaration / processing instruction (`<!...>` or `<?...>`).
// `pos` points just past '<'. Returns true if a (skipped) token was consumed.
bool ParseHtmlBangOrPi(const std::string& html, size_t& pos, HtmlTagToken& outTok) {
    const size_t end = html.find('>', pos);
    if (end == std::string::npos) {
        pos = html.size();
        return false;
    }
    pos = end + 1;
    outTok = HtmlTagToken{};
    outTok.name = "!ignored";
    return true;
}

// Apply one parsed attribute to the token, decoding entities for the subset we
// care about: anchor href, plus image src and alt.
void ApplyHtmlAttribute(const std::string& attrName, const std::string& attrValue, HtmlTagToken& outTok) {
    if (attrName == "href" && outTok.name == "a") {
        outTok.href = DecodeHtmlEntities(attrValue);
    }
    if (attrName == "src" && outTok.name == "img") {
        outTok.src = DecodeHtmlEntities(attrValue);
    }
    if (attrName == "alt" && outTok.name == "img") {
        outTok.alt = DecodeHtmlEntities(attrValue);
    }
}

bool ParseHtmlTag(const std::string& html, size_t& pos, HtmlTagToken& outTok, bool& outFell) {
    // pos points at '<'; returns true if a tag was consumed; advances pos past '>'.
    if (pos >= html.size() || html[pos] != '<')
        return false;
    const size_t start = pos;
    ++pos;
    if (pos >= html.size()) {
        pos = start;
        return false;
    }
    if (html[pos] == '!' || html[pos] == '?') {
        return ParseHtmlBangOrPi(html, pos, outTok);
    }

    if (html[pos] == '/') {
        outTok.isClose = true;
        ++pos;
    }

    std::string name = ConsumeHtmlTagName(html, pos);
    if (name.empty()) {
        pos = start;
        return false;
    }
    outTok.name = name;

    // Parse attributes (only `href` matters for our subset).
    while (pos < html.size() && html[pos] != '>') {
        if (html[pos] == '/') {
            outTok.isSelfClosed = true;
            ++pos;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(html[pos]))) {
            ++pos;
            continue;
        }
        std::string attrName;
        std::string attrValue;
        ParseOneHtmlAttribute(html, pos, attrName, attrValue);
        ApplyHtmlAttribute(attrName, attrValue, outTok);
        // Other attributes outside our allowlist tip the fallback when the tag itself is allowlisted
        // but the attribute changes meaning (e.g. `class` on <code> is fine; `onclick` on anything
        // else is suspicious). Conservative default: don't trip on unknown attrs to keep Plane's
        // realistic outputs working — only unknown TAGS trip the fallback.
        (void)outFell;
    }
    if (pos < html.size() && html[pos] == '>')
        ++pos;
    return true;
}

// Table-driven HTML→Markdown tag dispatch. `HtmlToMarkdown` is a thin parse
// loop that, per tag, looks up a handler in one of two dispatch tables
// (`HtmlOpenHandlers` / `HtmlCloseHandlers`) keyed by tag name and invokes it
// against a shared `HtmlMdCtx`. Every handler renders exactly the bytes the
// original if-else branch emitted — output is byte-for-byte identical (pinned
// by tests/Core/MarkdownConvert.test.cpp).

// One open element on the parse stack.
struct HtmlMdFrame {
    std::string tag;
    int olCounter = 0; // <ol> running index; reused on <a> frames as a 1-based linkHrefs index.
    int listIndent = 0;
};

// All mutable state the tag handlers share. The former in-function lambdas
// (`tail`, `flushBuffer`, `appendPipeTable`) are now methods so their branches
// live outside the parse loop's body.
struct HtmlMdCtx {
    std::ostringstream out;
    std::string buffer;
    std::vector<std::string*> outPtrStack;
    std::vector<HtmlMdFrame> stack;
    // Href stack for <a> tags. A plain per-call member (never static): a mid-parse exception must
    // not leave stale state that corrupts the next call. Open/close handlers index into it via
    // HtmlMdFrame::olCounter.
    std::vector<std::string> linkHrefs;
    std::vector<std::vector<std::string>> tableRows;
    std::string cellAcc;
    int tableNest = 0;
    bool fellBack = false;

    HtmlMdCtx() { outPtrStack.push_back(&buffer); }

    std::string& tail() { return *outPtrStack.back(); }

    void flushBuffer(bool addBlankLine) {
        std::string& tref = *outPtrStack.back();
        if (outPtrStack.size() == 1) {
            if (!tref.empty()) {
                out << tref;
                tref.clear();
            }
            if (addBlankLine)
                out << "\n\n";
        } else {
            if (addBlankLine)
                tref += "\n\n";
        }
    }

    void appendPipeTable(const std::vector<std::vector<std::string>>& rows) {
        if (rows.empty())
            return;
        const size_t ncol =
            std::accumulate(rows.begin(), rows.end(), size_t{},
                            [](size_t acc, const std::vector<std::string>& r) { return (std::max)(acc, r.size()); });
        auto escPipe = [](std::string c) {
            for (size_t i = 0; i < c.size(); ++i) {
                if (c[i] == '|') {
                    c.insert(i, "\\");
                    ++i;
                }
            }
            return c;
        };
        bool firstRow = true;
        for (const auto& r : rows) {
            out << '|';
            for (size_t i = 0; i < ncol; ++i) {
                out << ' ';
                if (i < r.size())
                    out << escPipe(r[i]);
                out << " |";
            }
            out << '\n';
            if (firstRow) {
                out << '|';
                for (size_t i = 0; i < ncol; ++i)
                    out << " --- |";
                out << '\n';
                firstRow = false;
            }
        }
        out << '\n';
    }
};

std::string HtmlInlineOpenMd(const std::string& t) {
    if (t == "strong" || t == "b")
        return "**";
    if (t == "em" || t == "i")
        return "*";
    if (t == "s" || t == "del")
        return "~~";
    if (t == "code")
        return "`";
    // <u>: Markdown has no underline; pass through plain.
    return "";
}

using HtmlTagHandler = void (*)(HtmlMdCtx&, const HtmlTagToken&);

// Open-tag handlers.

void HtmlOpenTable(HtmlMdCtx& c, const HtmlTagToken&) {
    if (c.tableNest > 0) {
        c.fellBack = true;
        return;
    }
    ++c.tableNest;
    c.flushBuffer(true);
    c.tableRows.clear();
    c.stack.push_back({"table", 0, 0});
}

void HtmlOpenTableSection(HtmlMdCtx& c, const HtmlTagToken& tok) {
    c.stack.push_back({tok.name, 0, 0}); // thead / tbody
}

void HtmlOpenTr(HtmlMdCtx& c, const HtmlTagToken&) {
    c.tableRows.emplace_back();
    c.stack.push_back({"tr", 0, 0});
}

void HtmlOpenCell(HtmlMdCtx& c, const HtmlTagToken& tok) {
    c.cellAcc.clear();
    c.outPtrStack.push_back(&c.cellAcc);
    c.stack.push_back({tok.name, 0, 0}); // td / th
}

void HtmlOpenBlock(HtmlMdCtx& c, const HtmlTagToken& tok) {
    c.stack.push_back({tok.name, 0, 0}); // p / div
}

void HtmlOpenHeading(HtmlMdCtx& c, const HtmlTagToken& tok) {
    c.stack.push_back({tok.name, 0, 0});
    const int level = tok.name[1] - '0';
    for (int i = 0; i < level; ++i)
        c.tail() += '#';
    c.tail() += ' ';
}

void HtmlOpenList(HtmlMdCtx& c, const HtmlTagToken& tok) {
    HtmlMdFrame f{tok.name, 0, 0}; // ul / ol
    if (!c.stack.empty())
        f.listIndent = c.stack.back().listIndent + 2;
    c.stack.push_back(f);
    c.tail() += "\n";
}

void HtmlOpenLi(HtmlMdCtx& c, const HtmlTagToken&) {
    HtmlMdFrame f{"li", 0, 0};
    if (!c.stack.empty())
        f.listIndent = c.stack.back().listIndent;
    c.stack.push_back(f);
    for (int i = 0; i < f.listIndent; ++i)
        c.tail() += ' ';
    bool ordered = false;
    for (auto it = c.stack.rbegin() + 1; it != c.stack.rend(); ++it) {
        if (it->tag == "ol") {
            ++it->olCounter;
            c.tail() += std::to_string(it->olCounter) + ". ";
            ordered = true;
            break;
        }
        if (it->tag == "ul") {
            c.tail() += "- ";
            break;
        }
    }
    if (!ordered && c.stack.size() == 1)
        c.tail() += "- ";
}

void HtmlOpenBlockquote(HtmlMdCtx& c, const HtmlTagToken&) {
    c.stack.push_back({"blockquote", 0, 0});
    c.tail() += "> ";
}

void HtmlOpenPre(HtmlMdCtx& c, const HtmlTagToken&) {
    c.stack.push_back({"pre", 0, 0});
    c.tail() += "\n```\n";
}

void HtmlOpenInlineMark(HtmlMdCtx& c, const HtmlTagToken& tok) {
    c.stack.push_back({tok.name, 0, 0});
    c.tail() += HtmlInlineOpenMd(tok.name);
}

void HtmlOpenAnchor(HtmlMdCtx& c, const HtmlTagToken& tok) {
    c.stack.push_back({"a", 0, 0});
    c.tail() += "[";
    c.linkHrefs.push_back(tok.href);
    c.stack.back().olCounter = static_cast<int>(c.linkHrefs.size());
}

void HtmlOpenSpan(HtmlMdCtx& c, const HtmlTagToken&) { c.stack.push_back({"span", 0, 0}); }

// Void-tag handlers (br / hr / img).

void HtmlVoidBr(HtmlMdCtx& c, const HtmlTagToken&) { c.tail() += "  \n"; }

void HtmlVoidHr(HtmlMdCtx& c, const HtmlTagToken&) { c.tail() += "\n---\n\n"; }

void HtmlVoidImg(HtmlMdCtx& c, const HtmlTagToken& tok) {
    std::string escAlt;
    for (char ch : tok.alt) {
        if (ch == ']' || ch == '\\')
            escAlt += '\\';
        escAlt += ch;
    }
    c.tail() += "![" + escAlt + "](" + tok.src + ")";
}

// Close-tag handlers: pop the matching open frame off the stack (shared
// `HtmlCloseGenericPop`), then emit the per-tag suffix. The td/th and table
// close paths run extra table-accumulation logic before the generic pop.

// Pops back to and including the nearest frame whose tag == t. Returns the
// popped <a> frame's href (empty otherwise) so the anchor handler can close
// the `](href)` syntax.
std::string HtmlCloseGenericPop(HtmlMdCtx& c, const std::string& t) {
    std::string poppedLinkHref;
    const auto itClose =
        std::find_if(c.stack.rbegin(), c.stack.rend(), [&](const HtmlMdFrame& fr) { return fr.tag == t; });
    if (itClose != c.stack.rend()) {
        auto it = itClose;
        if (t == "a" && it->olCounter > 0) {
            const size_t idx = static_cast<size_t>(it->olCounter) - 1;
            if (idx < c.linkHrefs.size()) {
                poppedLinkHref = c.linkHrefs[idx];
            }
            if (idx + 1 == c.linkHrefs.size())
                c.linkHrefs.pop_back();
        }
        const size_t targetIdx = std::distance(it, c.stack.rend()) - 1;
        while (c.stack.size() > targetIdx + 1)
            c.stack.pop_back();
        c.stack.pop_back();
    }
    return poppedLinkHref;
}

void HtmlCloseCell(HtmlMdCtx& c, const HtmlTagToken& tok) {
    if (c.outPtrStack.size() > 1 && c.outPtrStack.back() == &c.cellAcc) {
        std::string finished = std::move(c.cellAcc);
        c.cellAcc.clear();
        c.outPtrStack.pop_back();
        if (!c.tableRows.empty()) {
            c.tableRows.back().push_back(std::move(finished));
        }
    }
    HtmlCloseGenericPop(c, tok.name); // td / th
}

void HtmlCloseTable(HtmlMdCtx& c, const HtmlTagToken&) {
    c.appendPipeTable(c.tableRows);
    c.tableRows.clear();
    if (c.tableNest > 0)
        --c.tableNest;
    HtmlCloseGenericPop(c, "table");
}

void HtmlCloseBlock(HtmlMdCtx& c, const HtmlTagToken& tok) {
    HtmlCloseGenericPop(c, tok.name); // p / div / h1-h6 / blockquote all emit "\n\n"
    c.tail() += "\n\n";
}

void HtmlCloseLi(HtmlMdCtx& c, const HtmlTagToken&) {
    HtmlCloseGenericPop(c, "li");
    c.tail() += "\n";
}

void HtmlCloseList(HtmlMdCtx& c, const HtmlTagToken& tok) {
    HtmlCloseGenericPop(c, tok.name); // ul / ol
    c.flushBuffer(true);
}

void HtmlClosePre(HtmlMdCtx& c, const HtmlTagToken&) {
    HtmlCloseGenericPop(c, "pre");
    c.tail() += "\n```\n\n";
}

void HtmlCloseInlineMark(HtmlMdCtx& c, const HtmlTagToken& tok) {
    HtmlCloseGenericPop(c, tok.name);
    c.tail() += HtmlInlineOpenMd(tok.name);
}

void HtmlCloseAnchor(HtmlMdCtx& c, const HtmlTagToken&) {
    const std::string href = HtmlCloseGenericPop(c, "a");
    c.tail() += "](";
    c.tail() += href;
    c.tail() += ")";
}

// Pop-only close (thead / tbody / tr / span) — no suffix emitted.
void HtmlClosePopOnly(HtmlMdCtx& c, const HtmlTagToken& tok) { HtmlCloseGenericPop(c, tok.name); }

// Dispatch tables: tag name → handler.

const std::unordered_map<std::string, HtmlTagHandler>& HtmlOpenHandlers() {
    static const std::unordered_map<std::string, HtmlTagHandler> m = {
        {"table", HtmlOpenTable},
        {"thead", HtmlOpenTableSection},
        {"tbody", HtmlOpenTableSection},
        {"tr", HtmlOpenTr},
        {"td", HtmlOpenCell},
        {"th", HtmlOpenCell},
        {"p", HtmlOpenBlock},
        {"div", HtmlOpenBlock},
        {"h1", HtmlOpenHeading},
        {"h2", HtmlOpenHeading},
        {"h3", HtmlOpenHeading},
        {"h4", HtmlOpenHeading},
        {"h5", HtmlOpenHeading},
        {"h6", HtmlOpenHeading},
        {"ul", HtmlOpenList},
        {"ol", HtmlOpenList},
        {"li", HtmlOpenLi},
        {"blockquote", HtmlOpenBlockquote},
        {"pre", HtmlOpenPre},
        {"strong", HtmlOpenInlineMark},
        {"b", HtmlOpenInlineMark},
        {"em", HtmlOpenInlineMark},
        {"i", HtmlOpenInlineMark},
        {"s", HtmlOpenInlineMark},
        {"del", HtmlOpenInlineMark},
        {"u", HtmlOpenInlineMark},
        {"code", HtmlOpenInlineMark},
        {"a", HtmlOpenAnchor},
        {"span", HtmlOpenSpan},
        {"br", HtmlVoidBr},
        {"hr", HtmlVoidHr},
        {"img", HtmlVoidImg},
    };
    return m;
}

const std::unordered_map<std::string, HtmlTagHandler>& HtmlCloseHandlers() {
    static const std::unordered_map<std::string, HtmlTagHandler> m = {
        {"td", HtmlCloseCell},        {"th", HtmlCloseCell},           {"table", HtmlCloseTable},
        {"p", HtmlCloseBlock},        {"div", HtmlCloseBlock},         {"h1", HtmlCloseBlock},
        {"h2", HtmlCloseBlock},       {"h3", HtmlCloseBlock},          {"h4", HtmlCloseBlock},
        {"h5", HtmlCloseBlock},       {"h6", HtmlCloseBlock},          {"blockquote", HtmlCloseBlock},
        {"li", HtmlCloseLi},          {"ul", HtmlCloseList},           {"ol", HtmlCloseList},
        {"pre", HtmlClosePre},        {"strong", HtmlCloseInlineMark}, {"b", HtmlCloseInlineMark},
        {"em", HtmlCloseInlineMark},  {"i", HtmlCloseInlineMark},      {"s", HtmlCloseInlineMark},
        {"del", HtmlCloseInlineMark}, {"u", HtmlCloseInlineMark},      {"code", HtmlCloseInlineMark},
        {"a", HtmlCloseAnchor},       {"thead", HtmlClosePopOnly},     {"tbody", HtmlClosePopOnly},
        {"tr", HtmlClosePopOnly},     {"span", HtmlClosePopOnly},
    };
    return m;
}

// Collapse runs of 3+ newlines down to 2 (paragraph break).
std::string HtmlCollapseNewlines(const std::string& result) {
    std::string collapsed;
    collapsed.reserve(result.size());
    int newlineRun = 0;
    for (char ch : result) {
        if (ch == '\n') {
            ++newlineRun;
            if (newlineRun <= 2)
                collapsed += ch;
        } else {
            newlineRun = 0;
            collapsed += ch;
        }
    }
    return collapsed;
}

std::string HtmlToMarkdown(const std::string& html, bool* outFellBack) {
    HtmlMdCtx c;
    const auto& allowed = HtmlAllowedTags();
    const auto& openHandlers = HtmlOpenHandlers();
    const auto& closeHandlers = HtmlCloseHandlers();

    size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] != '<') {
            const size_t lt = html.find('<', pos);
            const size_t end = (lt == std::string::npos) ? html.size() : lt;
            c.tail() += DecodeHtmlEntities(html.substr(pos, end - pos));
            pos = end;
            continue;
        }

        HtmlTagToken tok;
        const size_t before = pos;
        if (!ParseHtmlTag(html, pos, tok, c.fellBack)) {
            c.tail() += html[before];
            pos = before + 1;
            continue;
        }
        if (tok.name == "!ignored")
            continue;

        // Unknown tag → trip the fallback flag and suppress markup (text inside still emits).
        if (allowed.find(tok.name) == allowed.end()) {
            c.fellBack = true;
            continue;
        }

        if (tok.isClose) {
            const auto it = closeHandlers.find(tok.name);
            if (it != closeHandlers.end())
                it->second(c, tok);
            continue;
        }

        const auto it = openHandlers.find(tok.name);
        if (it != openHandlers.end())
            it->second(c, tok);
    }

    // Malformed HTML (e.g. unclosed <td>) can leave a nested text sink active — merge back and
    // flag fallback so callers can prefer raw-mode.
    while (c.outPtrStack.size() > 1) {
        c.fellBack = true;
        std::string orphan = std::move(*c.outPtrStack.back());
        c.outPtrStack.pop_back();
        c.tail() += orphan;
    }
    // If the document ended mid-table (missing </table>), appendPipeTable never fired on the
    // </table> close path and the accumulated rows would be lost. Flush them here so the
    // partial table at least renders as best-effort pipe-table output before the fallback flag
    // routes the caller to raw mode.
    if (!c.tableRows.empty()) {
        c.fellBack = true;
        c.appendPipeTable(c.tableRows);
        c.tableRows.clear();
    }

    c.flushBuffer(false);
    if (outFellBack)
        *outFellBack = c.fellBack;
    return HtmlCollapseNewlines(c.out.str());
}

} // namespace md_detail
} // namespace MarkdownConvert
