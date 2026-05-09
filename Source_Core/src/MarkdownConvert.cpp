#include "MarkdownConvert.h"

extern "C" {
#include "md4c.h"
}

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <unordered_set>

using nlohmann::json;

namespace MarkdownConvert {

namespace {

constexpr unsigned kMd4cFlags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS |
                                MD_FLAG_PERMISSIVEAUTOLINKS;

std::string MdAttrToString(const MD_ATTRIBUTE& attr) {
    if (attr.text == nullptr || attr.size == 0) return std::string();
    return std::string(attr.text, attr.size);
}

// ============================================================================
// Markdown -> ADF (Atlassian Document Format JSON)
// ============================================================================

struct AdfBuilder {
    json doc;
    /// Stack of pointers into `doc` — each entry is the `content` array of the currently-open
    /// container block. Top of stack is where new child nodes get pushed.
    std::vector<json*> contentStack;
    /// Inline marks currently active for emitted text nodes (innermost first).
    std::vector<json> markStack;
    int codeBlockDepth = 0;
    /// Set while traversing a table; subtree silently dropped (tables not in ADF subset).
    bool dropTable = false;
    int tableDepth = 0;

    AdfBuilder() {
        doc = {{"type", "doc"}, {"version", 1}, {"content", json::array()}};
        contentStack.push_back(&doc["content"]);
    }

    json* topContent() { return contentStack.back(); }
};

void AdfEmitText(AdfBuilder& b, const std::string& text) {
    if (text.empty()) return;
    json node = {{"type", "text"}, {"text", text}};
    if (!b.markStack.empty()) {
        node["marks"] = b.markStack;
    }
    b.topContent()->push_back(std::move(node));
}

int AdfEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);
    if (b.dropTable) {
        if (type == MD_BLOCK_TABLE) ++b.tableDepth;
        return 0;
    }

    json node;
    bool pushChildContent = true;
    switch (type) {
        case MD_BLOCK_DOC:
            return 0;
        case MD_BLOCK_QUOTE:
            node = {{"type", "blockquote"}, {"content", json::array()}};
            break;
        case MD_BLOCK_UL:
            node = {{"type", "bulletList"}, {"content", json::array()}};
            break;
        case MD_BLOCK_OL: {
            auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
            node = {{"type", "orderedList"}, {"content", json::array()}};
            if (d && d->start != 1) {
                node["attrs"] = {{"order", static_cast<int>(d->start)}};
            }
            break;
        }
        case MD_BLOCK_LI:
            node = {{"type", "listItem"}, {"content", json::array()}};
            break;
        case MD_BLOCK_HR:
            node = {{"type", "rule"}};
            pushChildContent = false;
            break;
        case MD_BLOCK_H: {
            auto* d = static_cast<MD_BLOCK_H_DETAIL*>(detail);
            const int level = d ? static_cast<int>(d->level) : 1;
            node = {{"type", "heading"},
                    {"attrs", {{"level", level}}},
                    {"content", json::array()}};
            break;
        }
        case MD_BLOCK_CODE: {
            auto* d = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            const std::string lang = d ? MdAttrToString(d->lang) : std::string();
            node = {{"type", "codeBlock"}, {"content", json::array()}};
            if (!lang.empty()) {
                node["attrs"] = {{"language", lang}};
            }
            ++b.codeBlockDepth;
            break;
        }
        case MD_BLOCK_P:
            node = {{"type", "paragraph"}, {"content", json::array()}};
            break;
        case MD_BLOCK_TABLE:
            b.dropTable = true;
            ++b.tableDepth;
            return 0;
        case MD_BLOCK_HTML:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
        case MD_BLOCK_TR:
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            return 0;
    }

    auto* parent = b.topContent();
    parent->push_back(std::move(node));
    if (pushChildContent) {
        b.contentStack.push_back(&parent->back()["content"]);
    }
    return 0;
}

int AdfLeaveBlock(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);
    if (type == MD_BLOCK_TABLE) {
        if (--b.tableDepth <= 0) {
            b.tableDepth = 0;
            b.dropTable = false;
        }
        return 0;
    }
    if (b.dropTable) return 0;

    switch (type) {
        case MD_BLOCK_DOC:
        case MD_BLOCK_HR:
        case MD_BLOCK_HTML:
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:
        case MD_BLOCK_TR:
        case MD_BLOCK_TH:
        case MD_BLOCK_TD:
            return 0;
        case MD_BLOCK_CODE:
            --b.codeBlockDepth;
            if (b.contentStack.size() > 1) b.contentStack.pop_back();
            return 0;
        case MD_BLOCK_LI: {
            // ADF requires listItem children to be block nodes (paragraph, etc.), not inline
            // text nodes. Tight Markdown lists produce no MD_BLOCK_P, so text lands directly
            // in listItem.content. Wrap any top-level text/hardBreak nodes in a paragraph
            // before popping, making the ADF valid for Jira's API.
            if (b.contentStack.size() > 1) {
                json* listContent = b.contentStack.back();
                if (listContent && !listContent->empty()) {
                    bool hasDirectInline = false;
                    for (const auto& child : *listContent) {
                        if (child.is_object()) {
                            const std::string ct = child.value("type", std::string());
                            if (ct == "text" || ct == "hardBreak") {
                                hasDirectInline = true;
                                break;
                            }
                        }
                    }
                    if (hasDirectInline) {
                        // Wrap all current children in a paragraph.
                        nlohmann::json para = {{"type", "paragraph"}, {"content", *listContent}};
                        *listContent = nlohmann::json::array({std::move(para)});
                    }
                }
                b.contentStack.pop_back();
            }
            return 0;
        }
        default:
            if (b.contentStack.size() > 1) b.contentStack.pop_back();
            return 0;
    }
}

int AdfEnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);
    if (b.dropTable) return 0;
    switch (type) {
        case MD_SPAN_EM:
            b.markStack.push_back({{"type", "em"}});
            break;
        case MD_SPAN_STRONG:
            b.markStack.push_back({{"type", "strong"}});
            break;
        case MD_SPAN_DEL:
            b.markStack.push_back({{"type", "strike"}});
            break;
        case MD_SPAN_CODE:
            b.markStack.push_back({{"type", "code"}});
            break;
        case MD_SPAN_A: {
            auto* d = static_cast<MD_SPAN_A_DETAIL*>(detail);
            const std::string href = d ? MdAttrToString(d->href) : std::string();
            b.markStack.push_back({{"type", "link"}, {"attrs", {{"href", href}}}});
            break;
        }
        default:
            // MD_SPAN_IMG / LATEXMATH / WIKILINK / U: not in subset.
            break;
    }
    return 0;
}

int AdfLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);
    if (b.dropTable) return 0;
    switch (type) {
        case MD_SPAN_EM:
        case MD_SPAN_STRONG:
        case MD_SPAN_DEL:
        case MD_SPAN_CODE:
        case MD_SPAN_A:
            if (!b.markStack.empty()) b.markStack.pop_back();
            break;
        default:
            break;
    }
    return 0;
}

int AdfTextCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);
    if (b.dropTable) return 0;
    if (type == MD_TEXT_NULLCHAR) return 0;
    if (type == MD_TEXT_HTML) return 0;

    if (type == MD_TEXT_BR) {
        b.topContent()->push_back({{"type", "hardBreak"}});
        return 0;
    }

    std::string txt(text, size);
    if (type == MD_TEXT_SOFTBR) {
        AdfEmitText(b, " ");
        return 0;
    }

    if (b.codeBlockDepth > 0) {
        // ADF codeBlock children are plain text — coalesce into a single text node so
        // multi-line blocks don't fragment into one node per parser chunk.
        json* parent = b.topContent();
        if (!parent->empty() && parent->back().value("type", std::string()) == "text") {
            parent->back()["text"] = parent->back()["text"].get<std::string>() + txt;
        } else {
            parent->push_back({{"type", "text"}, {"text", txt}});
        }
        return 0;
    }

    AdfEmitText(b, txt);
    return 0;
}

// ============================================================================
// Markdown -> HTML (Plane subset)
// ============================================================================

struct HtmlBuilder {
    std::ostringstream out;
    int codeBlockDepth = 0;
    bool dropTable = false;
    int tableDepth = 0;
};

void HtmlEscape(std::ostringstream& out, const std::string& text) {
    for (char c : text) {
        switch (c) {
            case '&': out << "&amp;"; break;
            case '<': out << "&lt;"; break;
            case '>': out << "&gt;"; break;
            case '"': out << "&quot;"; break;
            case '\'': out << "&#39;"; break;
            default: out << c;
        }
    }
}

void HtmlEscapeAttr(std::ostringstream& out, const std::string& text) {
    for (char c : text) {
        switch (c) {
            case '&': out << "&amp;"; break;
            case '"': out << "&quot;"; break;
            case '<': out << "&lt;"; break;
            case '>': out << "&gt;"; break;
            default: out << c;
        }
    }
}

int HtmlEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    if (b.dropTable) {
        if (type == MD_BLOCK_TABLE) ++b.tableDepth;
        return 0;
    }
    switch (type) {
        case MD_BLOCK_DOC: break;
        case MD_BLOCK_QUOTE: b.out << "<blockquote>"; break;
        case MD_BLOCK_UL: b.out << "<ul>"; break;
        case MD_BLOCK_OL: {
            auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
            if (d && d->start != 1) b.out << "<ol start=\"" << d->start << "\">";
            else b.out << "<ol>";
            break;
        }
        case MD_BLOCK_LI: b.out << "<li>"; break;
        case MD_BLOCK_HR: b.out << "<hr/>"; break;
        case MD_BLOCK_H: {
            auto* d = static_cast<MD_BLOCK_H_DETAIL*>(detail);
            const int level = d ? static_cast<int>(d->level) : 1;
            b.out << "<h" << level << ">";
            break;
        }
        case MD_BLOCK_CODE: {
            auto* d = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
            const std::string lang = d ? MdAttrToString(d->lang) : std::string();
            b.out << "<pre><code";
            if (!lang.empty()) {
                b.out << " class=\"language-";
                HtmlEscapeAttr(b.out, lang);
                b.out << "\"";
            }
            b.out << ">";
            ++b.codeBlockDepth;
            break;
        }
        case MD_BLOCK_P: b.out << "<p>"; break;
        case MD_BLOCK_TABLE:
            b.dropTable = true;
            ++b.tableDepth;
            break;
        default:
            break;
    }
    return 0;
}

int HtmlLeaveBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    if (type == MD_BLOCK_TABLE) {
        if (--b.tableDepth <= 0) {
            b.tableDepth = 0;
            b.dropTable = false;
        }
        return 0;
    }
    if (b.dropTable) return 0;
    switch (type) {
        case MD_BLOCK_DOC:
        case MD_BLOCK_HR:
            break;
        case MD_BLOCK_QUOTE: b.out << "</blockquote>"; break;
        case MD_BLOCK_UL: b.out << "</ul>"; break;
        case MD_BLOCK_OL: b.out << "</ol>"; break;
        case MD_BLOCK_LI: b.out << "</li>"; break;
        case MD_BLOCK_H: {
            auto* d = static_cast<MD_BLOCK_H_DETAIL*>(detail);
            const int level = d ? static_cast<int>(d->level) : 1;
            b.out << "</h" << level << ">";
            break;
        }
        case MD_BLOCK_CODE:
            --b.codeBlockDepth;
            b.out << "</code></pre>";
            break;
        case MD_BLOCK_P: b.out << "</p>"; break;
        default: break;
    }
    return 0;
}

int HtmlEnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    if (b.dropTable) return 0;
    switch (type) {
        case MD_SPAN_EM: b.out << "<em>"; break;
        case MD_SPAN_STRONG: b.out << "<strong>"; break;
        case MD_SPAN_DEL: b.out << "<s>"; break;
        case MD_SPAN_CODE: b.out << "<code>"; break;
        case MD_SPAN_A: {
            auto* d = static_cast<MD_SPAN_A_DETAIL*>(detail);
            const std::string href = d ? MdAttrToString(d->href) : std::string();
            b.out << "<a href=\"";
            HtmlEscapeAttr(b.out, href);
            b.out << "\">";
            break;
        }
        default: break;
    }
    return 0;
}

int HtmlLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    if (b.dropTable) return 0;
    switch (type) {
        case MD_SPAN_EM: b.out << "</em>"; break;
        case MD_SPAN_STRONG: b.out << "</strong>"; break;
        case MD_SPAN_DEL: b.out << "</s>"; break;
        case MD_SPAN_CODE: b.out << "</code>"; break;
        case MD_SPAN_A: b.out << "</a>"; break;
        default: break;
    }
    return 0;
}

int HtmlTextCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    if (b.dropTable) return 0;
    if (type == MD_TEXT_NULLCHAR) return 0;
    if (type == MD_TEXT_HTML) return 0;
    if (type == MD_TEXT_BR) {
        b.out << "<br/>";
        return 0;
    }
    if (type == MD_TEXT_SOFTBR) {
        b.out << '\n';
        return 0;
    }
    const std::string txt(text, size);
    if (type == MD_TEXT_ENTITY) {
        // md4c verified the entity reference; passthrough.
        b.out << txt;
        return 0;
    }
    HtmlEscape(b.out, txt);
    return 0;
}

// ============================================================================
// ADF -> Markdown (recursive walker)
// ============================================================================

struct AdfWalkState {
    std::ostringstream out;
    std::vector<std::string> dropped;
    /// List nesting indent (each level adds two spaces).
    int listIndent = 0;
    /// Stack of list kinds (`'-'` for bullet, `'1'` for ordered) so listItem knows its bullet.
    std::vector<char> listMarkerStack;
    /// Per-ordered-list counter so we emit "1.", "2.", ...
    std::vector<int> orderedCounters;
    bool insideBlockquote = false;
};

void AppendIndent(std::ostringstream& out, int spaces) {
    for (int i = 0; i < spaces; ++i) out << ' ';
}

void EmitInlineText(const json& node, std::ostringstream& out) {
    if (!node.is_object()) return;
    const std::string type = node.value("type", std::string());
    if (type == "text") {
        std::string text = node.value("text", std::string());
        // Apply marks innermost-first when emitting; ADF stores marks innermost-last in its array.
        std::vector<std::string> openWrap, closeWrap;
        std::string href;
        bool isCode = false;
        if (node.contains("marks") && node["marks"].is_array()) {
            for (const auto& m : node["marks"]) {
                const std::string mt = m.value("type", std::string());
                if (mt == "strong") { openWrap.push_back("**"); closeWrap.push_back("**"); }
                else if (mt == "em") { openWrap.push_back("*"); closeWrap.push_back("*"); }
                else if (mt == "strike") { openWrap.push_back("~~"); closeWrap.push_back("~~"); }
                else if (mt == "code") { isCode = true; }
                else if (mt == "link") {
                    if (m.contains("attrs") && m["attrs"].is_object()) {
                        href = m["attrs"].value("href", std::string());
                    }
                }
            }
        }
        if (isCode) {
            // Inline code wins: drop other emphasis on this run.
            out << "`" << text << "`";
            return;
        }
        for (const auto& w : openWrap) out << w;
        // Autolink shortcut: when the visible text equals the link target, emit a bare URL
        // instead of [url](url). md4c's MD_FLAG_PERMISSIVEAUTOLINKS turns it back into a link
        // on save, so round-trip semantics are preserved with a cleaner editor surface.
        const bool autolink = !href.empty() && text == href;
        if (!href.empty() && !autolink) out << "[";
        out << text;
        if (!href.empty() && !autolink) {
            out << "](" << href << ")";
        }
        for (auto it = closeWrap.rbegin(); it != closeWrap.rend(); ++it) {
            out << *it;
        }
    } else if (type == "hardBreak") {
        out << "  \n";
    } else if (type == "emoji") {
        out << node.value("attrs", json::object()).value("shortName", node.value("text", std::string("")));
    } else if (type == "inlineCard") {
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
    } else if (type == "mention") {
        // ADF @-mention. Use the display text from attrs.text; fall back to id if missing.
        if (node.contains("attrs") && node["attrs"].is_object()) {
            std::string mtxt = node["attrs"].value("text", std::string());
            if (mtxt.empty()) mtxt = std::string("@") + node["attrs"].value("id", std::string());
            out << mtxt;
        }
    }
    // Other inline types (mediaInline, etc.) are silently dropped.
}

void EmitInlineRun(const json& contentArr, std::ostringstream& out) {
    if (!contentArr.is_array()) return;
    for (const auto& child : contentArr) {
        EmitInlineText(child, out);
    }
}

void EmitAdfBlock(const json& node, AdfWalkState& s);

void EmitAdfChildren(const json& node, AdfWalkState& s) {
    if (!node.is_object()) return;
    if (!node.contains("content") || !node["content"].is_array()) return;
    for (const auto& child : node["content"]) {
        EmitAdfBlock(child, s);
    }
}

void EmitAdfBlock(const json& node, AdfWalkState& s) {
    if (!node.is_object()) {
        return;
    }
    const std::string type = node.value("type", std::string());

    if (type == "paragraph") {
        AppendIndent(s.out, s.listIndent);
        if (s.insideBlockquote) s.out << "> ";
        EmitInlineRun(node.value("content", json::array()), s.out);
        s.out << "\n\n";
    } else if (type == "heading") {
        const int level = (node.contains("attrs") && node["attrs"].is_object())
                              ? node["attrs"].value("level", 1)
                              : 1;
        const int clamped = (std::max)(1, (std::min)(6, level));
        for (int i = 0; i < clamped; ++i) s.out << '#';
        s.out << ' ';
        EmitInlineRun(node.value("content", json::array()), s.out);
        s.out << "\n\n";
    } else if (type == "bulletList") {
        s.listMarkerStack.push_back('-');
        s.orderedCounters.push_back(0);
        EmitAdfChildren(node, s);
        s.listMarkerStack.pop_back();
        s.orderedCounters.pop_back();
        if (s.listIndent == 0) s.out << "\n";
    } else if (type == "orderedList") {
        const int start = (node.contains("attrs") && node["attrs"].is_object())
                              ? node["attrs"].value("order", 1)
                              : 1;
        s.listMarkerStack.push_back('1');
        s.orderedCounters.push_back(start - 1);
        EmitAdfChildren(node, s);
        s.listMarkerStack.pop_back();
        s.orderedCounters.pop_back();
        if (s.listIndent == 0) s.out << "\n";
    } else if (type == "listItem") {
        AppendIndent(s.out, s.listIndent);
        const char marker = s.listMarkerStack.empty() ? '-' : s.listMarkerStack.back();
        if (marker == '1') {
            ++s.orderedCounters.back();
            s.out << s.orderedCounters.back() << ". ";
        } else {
            s.out << "- ";
        }
        // Render listItem children inline-style: the first paragraph stays on the marker line,
        // subsequent blocks indent.
        if (node.contains("content") && node["content"].is_array()) {
            const auto& children = node["content"];
            bool first = true;
            const int prevIndent = s.listIndent;
            for (const auto& c : children) {
                const std::string ct = c.is_object() ? c.value("type", std::string()) : std::string();
                if (first && ct == "paragraph") {
                    EmitInlineRun(c.value("content", json::array()), s.out);
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
        } else {
            s.out << "\n";
        }
    } else if (type == "codeBlock") {
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
            if (!current.empty() && current.back() != '\n') s.out << '\n';
        }
        AppendIndent(s.out, s.listIndent);
        s.out << "```\n\n";
    } else if (type == "blockquote") {
        const bool prev = s.insideBlockquote;
        s.insideBlockquote = true;
        EmitAdfChildren(node, s);
        s.insideBlockquote = prev;
    } else if (type == "rule") {
        s.out << "---\n\n";
    } else if (type == "hardBreak") {
        s.out << "  \n";
    } else if (type == "doc") {
        EmitAdfChildren(node, s);
    } else {
        s.dropped.push_back(type.empty() ? std::string("<unknown>") : type);
    }
}

// ============================================================================
// HTML subset -> Markdown (state machine over a small tag allowlist)
// ============================================================================

const std::unordered_set<std::string>& HtmlAllowedTags() {
    static const std::unordered_set<std::string> tags = {
        "p", "br", "hr",
        "h1", "h2", "h3", "h4", "h5", "h6",
        "strong", "b", "em", "i", "s", "del", "u",
        "code", "pre",
        "ul", "ol", "li",
        "a",
        "blockquote",
        "div", "span"  // soft-allowed: stripped to plain text wrapping
    };
    return tags;
}

std::string DecodeHtmlEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            const size_t end = s.find(';', i);
            if (end != std::string::npos && end - i <= 8) {
                const std::string ent = s.substr(i + 1, end - i - 1);
                if (ent == "amp") { out += '&'; i = end + 1; continue; }
                if (ent == "lt") { out += '<'; i = end + 1; continue; }
                if (ent == "gt") { out += '>'; i = end + 1; continue; }
                if (ent == "quot") { out += '"'; i = end + 1; continue; }
                if (ent == "apos" || ent == "#39") { out += '\''; i = end + 1; continue; }
                if (ent == "nbsp") { out += ' '; i = end + 1; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    int code = 0;
                    if (ent.size() > 2 && (ent[1] == 'x' || ent[1] == 'X')) {
                        for (size_t k = 2; k < ent.size(); ++k) {
                            const char c = ent[k];
                            if (c >= '0' && c <= '9') code = code * 16 + (c - '0');
                            else if (c >= 'a' && c <= 'f') code = code * 16 + (c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') code = code * 16 + (c - 'A' + 10);
                            else { code = 0; break; }
                        }
                    } else {
                        for (size_t k = 1; k < ent.size(); ++k) {
                            const char c = ent[k];
                            if (c < '0' || c > '9') { code = 0; break; }
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
};

bool ParseHtmlTag(const std::string& html, size_t& pos, HtmlTagToken& outTok, bool& outFell) {
    // pos points at '<'; returns true if a tag was consumed; advances pos past '>'.
    if (pos >= html.size() || html[pos] != '<') return false;
    const size_t start = pos;
    ++pos;
    if (pos >= html.size()) {
        pos = start;
        return false;
    }
    if (html[pos] == '!' || html[pos] == '?') {
        // Comment / declaration / processing instruction — skip until '>' or end.
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

    if (html[pos] == '/') {
        outTok.isClose = true;
        ++pos;
    }

    std::string name;
    while (pos < html.size() && (std::isalnum(static_cast<unsigned char>(html[pos])) || html[pos] == '-')) {
        name += static_cast<char>(std::tolower(static_cast<unsigned char>(html[pos])));
        ++pos;
    }
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
        while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) &&
               html[pos] != '=' && html[pos] != '>' && html[pos] != '/') {
            attrName += static_cast<char>(std::tolower(static_cast<unsigned char>(html[pos])));
            ++pos;
        }
        std::string attrValue;
        if (pos < html.size() && html[pos] == '=') {
            ++pos;
            if (pos < html.size() && (html[pos] == '"' || html[pos] == '\'')) {
                const char quote = html[pos++];
                while (pos < html.size() && html[pos] != quote) {
                    attrValue += html[pos++];
                }
                if (pos < html.size()) ++pos;
            } else {
                while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) &&
                       html[pos] != '>' && html[pos] != '/') {
                    attrValue += html[pos++];
                }
            }
        }
        if (attrName == "href" && outTok.name == "a") {
            outTok.href = DecodeHtmlEntities(attrValue);
        }
        // Other attributes outside our allowlist tip the fallback when the tag itself is allowlisted
        // but the attribute changes meaning (e.g. `class` on <code> is fine; `onclick` on anything
        // else is suspicious). Conservative default: don't trip on unknown attrs to keep Plane's
        // realistic outputs working — only unknown TAGS trip the fallback.
        (void)outFell;
    }
    if (pos < html.size() && html[pos] == '>') ++pos;
    return true;
}

std::string HtmlToMarkdown(const std::string& html, bool* outFellBack) {
    std::ostringstream out;
    bool fellBack = false;
    const auto& allowed = HtmlAllowedTags();

    /// Accumulator for paragraph-mode text; flushed on block boundaries.
    std::string buffer;
    auto flushBuffer = [&](bool addBlankLine) {
        if (!buffer.empty()) {
            out << buffer;
            buffer.clear();
        }
        if (addBlankLine) out << "\n\n";
    };

    /// Tracks open-tag context (innermost first). Used so inline marks unwrap in the right order
    /// and so `<li>` knows whether it belongs to an `<ol>` or `<ul>`.
    struct Frame {
        std::string tag;
        int olCounter = 0; // for <ol> only
        int listIndent = 0;
    };
    std::vector<Frame> stack;

    auto isInlineMark = [](const std::string& t) {
        return t == "strong" || t == "b" || t == "em" || t == "i" ||
               t == "s" || t == "del" || t == "u" || t == "code";
    };

    auto inlineOpenMd = [](const std::string& t) -> std::string {
        if (t == "strong" || t == "b") return "**";
        if (t == "em" || t == "i") return "*";
        if (t == "s" || t == "del") return "~~";
        if (t == "code") return "`";
        if (t == "u") return ""; // Markdown has no underline; pass through plain.
        return "";
    };

    size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] == '<') {
            HtmlTagToken tok;
            const size_t before = pos;
            if (!ParseHtmlTag(html, pos, tok, fellBack)) {
                buffer += html[before];
                pos = before + 1;
                continue;
            }
            if (tok.name == "!ignored") continue;

            // Unknown tag → trip the fallback flag and suppress markup (text inside still emits).
            if (allowed.find(tok.name) == allowed.end()) {
                fellBack = true;
                continue;
            }

            const std::string& t = tok.name;
            const bool isVoid = (t == "br" || t == "hr");
            if (tok.isClose) {
                // Pop matching frame; capture link href before popping for </a>.
                std::string poppedLinkHref;
                bool poppedAny = false;
                for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                    if (it->tag == t) {
                        if (t == "a" && it->olCounter > 0) {
                            static thread_local std::vector<std::string> sLinkHrefs;
                            const size_t idx = static_cast<size_t>(it->olCounter) - 1;
                            if (idx < sLinkHrefs.size()) {
                                poppedLinkHref = sLinkHrefs[idx];
                            }
                            // Trim the popped href slot if it was the last one (LIFO usage).
                            if (idx + 1 == sLinkHrefs.size()) sLinkHrefs.pop_back();
                        }
                        const size_t targetIdx = std::distance(it, stack.rend()) - 1;
                        while (stack.size() > targetIdx + 1) stack.pop_back();
                        stack.pop_back();
                        poppedAny = true;
                        break;
                    }
                }
                (void)poppedAny;
                if (t == "p" || t == "div") {
                    buffer += "\n\n";
                } else if (t == "h1" || t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6") {
                    buffer += "\n\n";
                } else if (t == "li") {
                    buffer += "\n";
                } else if (t == "ul" || t == "ol") {
                    flushBuffer(true);
                } else if (t == "blockquote") {
                    buffer += "\n\n";
                } else if (t == "pre") {
                    buffer += "\n```\n\n";
                } else if (isInlineMark(t)) {
                    buffer += inlineOpenMd(t);
                } else if (t == "a") {
                    buffer += "](";
                    buffer += poppedLinkHref;
                    buffer += ")";
                }
                continue;
            }

            // Opening / void tag.
            if (isVoid) {
                if (t == "br") buffer += "  \n";
                else if (t == "hr") buffer += "\n---\n\n";
                continue;
            }

            if (t == "p" || t == "div") {
                stack.push_back({t, 0, 0});
                continue;
            }
            if (t == "h1" || t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6") {
                stack.push_back({t, 0, 0});
                const int level = t[1] - '0';
                for (int i = 0; i < level; ++i) buffer += '#';
                buffer += ' ';
                continue;
            }
            if (t == "ul" || t == "ol") {
                Frame f{t, 0, 0};
                if (!stack.empty()) f.listIndent = stack.back().listIndent + 2;
                stack.push_back(f);
                buffer += "\n";
                continue;
            }
            if (t == "li") {
                Frame f{"li", 0, 0};
                if (!stack.empty()) f.listIndent = stack.back().listIndent;
                stack.push_back(f);
                for (int i = 0; i < f.listIndent; ++i) buffer += ' ';
                // Find enclosing ol/ul for marker.
                bool ordered = false;
                for (auto it = stack.rbegin() + 1; it != stack.rend(); ++it) {
                    if (it->tag == "ol") {
                        ++it->olCounter;
                        buffer += std::to_string(it->olCounter) + ". ";
                        ordered = true;
                        break;
                    }
                    if (it->tag == "ul") {
                        buffer += "- ";
                        break;
                    }
                }
                if (!ordered && stack.size() == 1) buffer += "- ";
                continue;
            }
            if (t == "blockquote") {
                stack.push_back({t, 0, 0});
                buffer += "> ";
                continue;
            }
            if (t == "pre") {
                stack.push_back({t, 0, 0});
                buffer += "\n```\n";
                continue;
            }
            if (isInlineMark(t)) {
                stack.push_back({t, 0, 0});
                buffer += inlineOpenMd(t);
                continue;
            }
            if (t == "a") {
                stack.push_back({"a", 0, 0});
                // Stash href in a parallel side-channel by appending a sentinel — simplest
                // approach: emit "[" now, record href, append "](href)" on close.
                buffer += "[";
                // The close handler can't easily recover the href; defer by inlining now via
                // a trailing append handled at close. To keep state minimal, repurpose the
                // Frame's olCounter as a slot index into a side vector of hrefs.
                // Implementation: store on a static parallel stack.
                static thread_local std::vector<std::string> sLinkHrefs;
                sLinkHrefs.push_back(tok.href);
                stack.back().olCounter = static_cast<int>(sLinkHrefs.size()); // 1-based marker
                continue;
            }
            // span: just push to track close, emit nothing.
            if (t == "span") {
                stack.push_back({t, 0, 0});
                continue;
            }
            continue;
        }

        // Plain text run — read until next '<'.
        const size_t lt = html.find('<', pos);
        const size_t end = (lt == std::string::npos) ? html.size() : lt;
        const std::string raw = html.substr(pos, end - pos);
        buffer += DecodeHtmlEntities(raw);
        pos = end;
    }

    flushBuffer(false);
    if (outFellBack) *outFellBack = fellBack;
    std::string result = out.str();
    // Collapse 3+ consecutive newlines into 2 (paragraph break).
    std::string collapsed;
    collapsed.reserve(result.size());
    int newlineRun = 0;
    for (char c : result) {
        if (c == '\n') {
            ++newlineRun;
            if (newlineRun <= 2) collapsed += c;
        } else {
            newlineRun = 0;
            collapsed += c;
        }
    }
    return collapsed;
}

} // namespace

// ============================================================================
// Public API
// ============================================================================

nlohmann::json MarkdownToAdf(const std::string& md) {
    AdfBuilder builder;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = kMd4cFlags;
    parser.enter_block = AdfEnterBlock;
    parser.leave_block = AdfLeaveBlock;
    parser.enter_span = AdfEnterSpan;
    parser.leave_span = AdfLeaveSpan;
    parser.text = AdfTextCallback;
    parser.debug_log = nullptr;
    parser.syntax = nullptr;

    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &builder);
    return builder.doc;
}

std::string AdfToMarkdown(const nlohmann::json& adf, std::vector<std::string>* outDroppedNodeTypes) {
    AdfWalkState s;
    if (adf.is_object()) {
        EmitAdfBlock(adf, s);
    } else if (adf.is_array()) {
        for (const auto& child : adf) EmitAdfBlock(child, s);
    }
    if (outDroppedNodeTypes) *outDroppedNodeTypes = std::move(s.dropped);

    // Trim trailing whitespace — most blocks emit "\n\n" suffix.
    std::string out = s.out.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

std::string MarkdownToHtml(const std::string& md) {
    HtmlBuilder builder;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = kMd4cFlags;
    parser.enter_block = HtmlEnterBlock;
    parser.leave_block = HtmlLeaveBlock;
    parser.enter_span = HtmlEnterSpan;
    parser.leave_span = HtmlLeaveSpan;
    parser.text = HtmlTextCallback;
    parser.debug_log = nullptr;
    parser.syntax = nullptr;

    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &builder);
    return builder.out.str();
}

std::string HtmlSubsetToMarkdown(const std::string& html, bool* outFellBack) {
    return HtmlToMarkdown(html, outFellBack);
}

} // namespace MarkdownConvert
