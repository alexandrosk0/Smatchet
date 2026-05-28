#include "MarkdownConvert.h"

#include "Logger.h"

extern "C" {
#include "md4c.h"
}

#include <algorithm>
#include <cctype>
#include <cstring>
#include <numeric>
#include <sstream>
#include <unordered_set>

using nlohmann::json;

namespace MarkdownConvert {

unsigned Md4cParserFlags() noexcept {
    return MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS | MD_FLAG_NOHTML |
           MD_FLAG_NOINDENTEDCODEBLOCKS;
}

namespace {

extern "C" void Md4cDebugLogShim(const char* msg, void* /*userdata*/) {
    if (msg && msg[0] != '\0') {
        LOG_DEBUG("md4c: %s", msg);
    }
}

std::string MdAttrToString(const MD_ATTRIBUTE& attr) {
    if (attr.text == nullptr || attr.size == 0)
        return std::string();
    return std::string(attr.text, attr.size);
}

/// Wrap contiguous runs of inline nodes (text / hardBreak / mention / emoji / mediaInline /
/// inlineCard) into paragraph blocks, leaving block nodes (lists, code, tables, ...) as siblings
/// in `arr`. Jira ADF requires list-item and table-cell content to be `block*` — and `paragraph`'s
/// own content is strictly `inline*`, so we cannot just wrap the whole array in one paragraph
/// when block siblings exist (that would put a bulletList inside paragraph and HTTP-400 the PUT).
void AdfWrapTopLevelInlineInParagraph(json* arr) {
    if (!arr || !arr->is_array() || arr->empty())
        return;
    auto isInline = [](const json& n) {
        if (!n.is_object())
            return false;
        const std::string t = n.value("type", std::string());
        return t == "text" || t == "hardBreak" || t == "mention" || t == "emoji" || t == "mediaInline" ||
               t == "inlineCard";
    };
    bool needsSplit = false;
    bool sawInline = false;
    bool sawBlock = false;
    for (const auto& child : *arr) {
        if (isInline(child))
            sawInline = true;
        else
            sawBlock = true;
        if (sawInline && sawBlock) {
            needsSplit = true;
            break;
        }
    }
    if (!sawInline)
        return;
    if (!needsSplit) {
        // All-inline → single paragraph wrap (the original fast path).
        json para = {{"type", "paragraph"}, {"content", *arr}};
        *arr = json::array({std::move(para)});
        return;
    }
    json result = json::array();
    json currentPara = json::array();
    auto flush = [&]() {
        if (!currentPara.empty()) {
            result.push_back(json{{"type", "paragraph"}, {"content", std::move(currentPara)}});
            currentPara = json::array();
        }
    };
    for (auto& child : *arr) {
        if (isInline(child)) {
            currentPara.push_back(child);
        } else {
            flush();
            result.push_back(child);
        }
    }
    flush();
    *arr = std::move(result);
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
    int imgSpanDepth = 0;
    /// Blockquote nesting depth. Jira ADF blockquote content allows only paragraph / bulletList /
    /// orderedList — nested blockquote is a schema violation. Flatten nested quotes into the outer
    /// blockquote by suppressing inner wrappers; depth tracks pairing so leave-block pops correctly.
    int blockquoteDepth = 0;
    std::string imgAltAccum;
    std::vector<std::string> imgSrcStack;
#ifndef NDEBUG
    /// At most one LOG_DEBUG per md_parse if md4c emits MD_TEXT_HTML despite NOHTML.
    bool debugLoggedMdTextHtml = false;
#endif

    AdfBuilder() : doc({{"type", "doc"}, {"version", 1}, {"content", json::array()}}) {
        contentStack.push_back(&doc["content"]);
    }

    json* topContent() { return contentStack.back(); }
};

void AdfEmitText(AdfBuilder& b, const std::string& text) {
    if (text.empty())
        return;
    // Coalesce consecutive text emissions that share the same marks. md4c can emit several
    // text events for what the user types as a single run (each whitespace boundary, soft
    // breaks rendered as spaces, etc.); without coalescing the ADF ends up with one node
    // per token, which is structurally noisy and confuses downstream renderers.
    json* parent = b.topContent();
    if (parent && !parent->empty()) {
        json& prev = parent->back();
        if (prev.is_object() && prev.value("type", std::string()) == "text") {
            const bool prevHasMarks = prev.contains("marks");
            const bool curHasMarks = !b.markStack.empty();
            const bool sameMarks = (prevHasMarks == curHasMarks) && (!prevHasMarks || prev["marks"] == b.markStack);
            if (sameMarks) {
                prev["text"] = prev["text"].get<std::string>() + text;
                return;
            }
        }
    }
    json node = {{"type", "text"}, {"text", text}};
    if (!b.markStack.empty()) {
        node["marks"] = b.markStack;
    }
    b.topContent()->push_back(std::move(node));
}

int AdfEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);

    json node;
    bool pushChildContent = true;
    switch (type) {
    case MD_BLOCK_DOC:
        return 0;
    case MD_BLOCK_QUOTE:
        // Suppress nested blockquote wrappers — Jira ADF rejects blockquote-inside-blockquote.
        // Inner content (paragraphs, lists) lands directly in the outer blockquote.
        if (b.blockquoteDepth > 0) {
            ++b.blockquoteDepth;
            return 0;
        }
        ++b.blockquoteDepth;
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
    case MD_BLOCK_LI: {
        node = {{"type", "listItem"}, {"content", json::array()}};
        auto* parent = b.topContent();
        parent->push_back(std::move(node));
        b.contentStack.push_back(&parent->back()["content"]);
        auto* lid = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        if (lid && lid->is_task) {
            const char tm = lid->task_mark;
            const bool done = (tm == 'x' || tm == 'X');
            AdfEmitText(b, done ? std::string("[x] ") : std::string("[ ] "));
        }
        return 0;
    }
    case MD_BLOCK_HR:
        node = {{"type", "rule"}};
        pushChildContent = false;
        break;
    case MD_BLOCK_H: {
        const auto* d = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
        const int level = d ? static_cast<int>(d->level) : 1;
        node = {{"type", "heading"}, {"attrs", {{"level", level}}}, {"content", json::array()}};
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
    case MD_BLOCK_TABLE: {
        node = {{"type", "table"},
                {"attrs", {{"layout", "default"}, {"isNumberColumnEnabled", false}}},
                {"content", json::array()}};
        auto* parent = b.topContent();
        parent->push_back(std::move(node));
        b.contentStack.push_back(&parent->back()["content"]);
        return 0;
    }
    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
        return 0;
    case MD_BLOCK_TR: {
        json row = {{"type", "tableRow"}, {"content", json::array()}};
        auto* parent = b.topContent();
        parent->push_back(std::move(row));
        b.contentStack.push_back(&parent->back()["content"]);
        return 0;
    }
    case MD_BLOCK_TH: {
        json cell = {{"type", "tableHeader"}, {"content", json::array()}};
        auto* parent = b.topContent();
        parent->push_back(std::move(cell));
        b.contentStack.push_back(&parent->back()["content"]);
        return 0;
    }
    case MD_BLOCK_TD: {
        json cell = {{"type", "tableCell"}, {"content", json::array()}};
        auto* parent = b.topContent();
        parent->push_back(std::move(cell));
        b.contentStack.push_back(&parent->back()["content"]);
        return 0;
    }
    case MD_BLOCK_HTML:
        return 0;
    default:
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

    switch (type) {
    case MD_BLOCK_DOC:
    case MD_BLOCK_HR:
    case MD_BLOCK_HTML:
    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
        return 0;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        if (b.contentStack.size() > 1) {
            AdfWrapTopLevelInlineInParagraph(b.contentStack.back());
            b.contentStack.pop_back();
        }
        return 0;
    case MD_BLOCK_TR:
        if (b.contentStack.size() > 1)
            b.contentStack.pop_back();
        return 0;
    case MD_BLOCK_TABLE:
        if (b.contentStack.size() > 1)
            b.contentStack.pop_back();
        return 0;
    case MD_BLOCK_CODE:
        --b.codeBlockDepth;
        if (b.contentStack.size() > 1)
            b.contentStack.pop_back();
        return 0;
    case MD_BLOCK_QUOTE:
        // Pair with the enter-block depth bookkeeping; only pop the content stack for the
        // outermost blockquote (inner wrappers were suppressed and never pushed).
        if (b.blockquoteDepth > 0) {
            --b.blockquoteDepth;
            if (b.blockquoteDepth == 0 && b.contentStack.size() > 1)
                b.contentStack.pop_back();
        }
        return 0;
    case MD_BLOCK_LI: {
        if (b.contentStack.size() > 1) {
            AdfWrapTopLevelInlineInParagraph(b.contentStack.back());
            b.contentStack.pop_back();
        }
        return 0;
    }
    default:
        if (b.contentStack.size() > 1)
            b.contentStack.pop_back();
        return 0;
    }
}

int AdfEnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);
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
    case MD_SPAN_IMG: {
        auto* d = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        b.imgSrcStack.push_back(d ? MdAttrToString(d->src) : std::string());
        ++b.imgSpanDepth;
        b.imgAltAccum.clear();
        break;
    }
    default:
        break;
    }
    return 0;
}

int AdfLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);
    switch (type) {
    case MD_SPAN_EM:
    case MD_SPAN_STRONG:
    case MD_SPAN_DEL:
    case MD_SPAN_CODE:
    case MD_SPAN_A:
        if (!b.markStack.empty())
            b.markStack.pop_back();
        break;
    case MD_SPAN_IMG: {
        if (!b.imgSrcStack.empty()) {
            std::string src = std::move(b.imgSrcStack.back());
            b.imgSrcStack.pop_back();
            static const std::string kAttachmentPrefix = "attachment:";
            if (src.compare(0, kAttachmentPrefix.size(), kAttachmentPrefix) == 0) {
                json attrs = json::object();
                attrs["type"] = "file";
                attrs["id"] = src.substr(kAttachmentPrefix.size());
                if (!b.imgAltAccum.empty()) {
                    attrs["alt"] = b.imgAltAccum;
                }
                b.topContent()->push_back(json{{"type", "mediaInline"}, {"attrs", std::move(attrs)}});
            } else {
                // External image URLs are not valid in Jira ADF `mediaInline` (which requires a
                // file-store `id`). Fall back to a text link so the URL is preserved and the
                // payload validates. The image-as-image is lost; the image-as-link is kept.
                const std::string display = b.imgAltAccum.empty() ? src : b.imgAltAccum;
                json mark = {{"type", "link"}, {"attrs", {{"href", src}}}};
                json textNode = {{"type", "text"}, {"text", display}, {"marks", json::array({std::move(mark)})}};
                b.topContent()->push_back(std::move(textNode));
            }
            b.imgAltAccum.clear();
        }
        if (b.imgSpanDepth > 0)
            --b.imgSpanDepth;
        break;
    }
    default:
        break;
    }
    return 0;
}

int AdfTextCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);
    if (type == MD_TEXT_NULLCHAR)
        return 0;
    // With MD_FLAG_NOHTML, md4c should not emit raw HTML here; ignore defensively (ABI / dialect).
    if (type == MD_TEXT_HTML) {
#ifndef NDEBUG
        if (!b.debugLoggedMdTextHtml) {
            b.debugLoggedMdTextHtml = true;
            LOG_DEBUG("md4c: unexpected MD_TEXT_HTML under NOHTML (ADF path, first chunk size=%u)",
                      static_cast<unsigned>(size));
        }
#endif
        return 0;
    }

    std::string txt(text, size);
    if (b.imgSpanDepth > 0 && b.codeBlockDepth == 0) {
        if (type == MD_TEXT_NORMAL || type == MD_TEXT_ENTITY || type == MD_TEXT_CODE) {
            b.imgAltAccum += txt;
            return 0;
        }
        if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
            b.imgAltAccum += ' ';
            return 0;
        }
        return 0;
    }

    if (type == MD_TEXT_BR) {
        b.topContent()->push_back({{"type", "hardBreak"}});
        return 0;
    }

    if (type == MD_TEXT_SOFTBR) {
        // Standard CommonMark renders soft breaks as spaces, but for a Jira description
        // editor that's surprising: the user pressed Enter and expected a visual line break.
        // Emit hardBreak so single-Enter behaves like a line break in the rendered ticket.
        // Double-Enter still produces a paragraph break (md4c emits MD_BLOCK_P open/close).
        b.topContent()->push_back({{"type", "hardBreak"}});
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
    int imgSpanDepth = 0;
    std::string imgAltBuf;
    std::vector<std::string> imgSrcStack;
#ifndef NDEBUG
    bool debugLoggedMdTextHtml = false;
#endif
};

void HtmlEscape(std::ostringstream& out, const std::string& text) {
    for (char c : text) {
        switch (c) {
        case '&':
            out << "&amp;";
            break;
        case '<':
            out << "&lt;";
            break;
        case '>':
            out << "&gt;";
            break;
        case '"':
            out << "&quot;";
            break;
        case '\'':
            out << "&#39;";
            break;
        default:
            out << c;
        }
    }
}

void HtmlEscapeAttr(std::ostringstream& out, const std::string& text) {
    for (char c : text) {
        switch (c) {
        case '&':
            out << "&amp;";
            break;
        case '"':
            out << "&quot;";
            break;
        case '<':
            out << "&lt;";
            break;
        case '>':
            out << "&gt;";
            break;
        default:
            out << c;
        }
    }
}

int HtmlEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    switch (type) {
    case MD_BLOCK_DOC:
        break;
    case MD_BLOCK_QUOTE:
        b.out << "<blockquote>";
        break;
    case MD_BLOCK_UL:
        b.out << "<ul>";
        break;
    case MD_BLOCK_OL: {
        auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        if (d && d->start != 1)
            b.out << "<ol start=\"" << d->start << "\">";
        else
            b.out << "<ol>";
        break;
    }
    case MD_BLOCK_LI:
        b.out << "<li>";
        break;
    case MD_BLOCK_HR:
        b.out << "<hr/>";
        break;
    case MD_BLOCK_H: {
        const auto* d = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
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
    case MD_BLOCK_P:
        b.out << "<p>";
        break;
    case MD_BLOCK_TABLE:
        b.out << "<table>";
        break;
    case MD_BLOCK_THEAD:
        b.out << "<thead>";
        break;
    case MD_BLOCK_TBODY:
        b.out << "<tbody>";
        break;
    case MD_BLOCK_TR:
        b.out << "<tr>";
        break;
    case MD_BLOCK_TH:
        b.out << "<th>";
        break;
    case MD_BLOCK_TD:
        b.out << "<td>";
        break;
    default:
        break;
    }
    return 0;
}

int HtmlLeaveBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    switch (type) {
    case MD_BLOCK_DOC:
    case MD_BLOCK_HR:
        break;
    case MD_BLOCK_QUOTE:
        b.out << "</blockquote>";
        break;
    case MD_BLOCK_UL:
        b.out << "</ul>";
        break;
    case MD_BLOCK_OL:
        b.out << "</ol>";
        break;
    case MD_BLOCK_LI:
        b.out << "</li>";
        break;
    case MD_BLOCK_H: {
        const auto* d = static_cast<const MD_BLOCK_H_DETAIL*>(detail);
        const int level = d ? static_cast<int>(d->level) : 1;
        b.out << "</h" << level << ">";
        break;
    }
    case MD_BLOCK_CODE:
        --b.codeBlockDepth;
        b.out << "</code></pre>";
        break;
    case MD_BLOCK_P:
        b.out << "</p>";
        break;
    case MD_BLOCK_TABLE:
        b.out << "</table>";
        break;
    case MD_BLOCK_THEAD:
        b.out << "</thead>";
        break;
    case MD_BLOCK_TBODY:
        b.out << "</tbody>";
        break;
    case MD_BLOCK_TR:
        b.out << "</tr>";
        break;
    case MD_BLOCK_TH:
        b.out << "</th>";
        break;
    case MD_BLOCK_TD:
        b.out << "</td>";
        break;
    default:
        break;
    }
    return 0;
}

int HtmlEnterSpan(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    switch (type) {
    case MD_SPAN_EM:
        b.out << "<em>";
        break;
    case MD_SPAN_STRONG:
        b.out << "<strong>";
        break;
    case MD_SPAN_DEL:
        b.out << "<s>";
        break;
    case MD_SPAN_CODE:
        b.out << "<code>";
        break;
    case MD_SPAN_A: {
        auto* d = static_cast<MD_SPAN_A_DETAIL*>(detail);
        const std::string href = d ? MdAttrToString(d->href) : std::string();
        b.out << "<a href=\"";
        HtmlEscapeAttr(b.out, href);
        b.out << "\">";
        break;
    }
    case MD_SPAN_IMG: {
        auto* d = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        b.imgSrcStack.push_back(d ? MdAttrToString(d->src) : std::string());
        ++b.imgSpanDepth;
        b.imgAltBuf.clear();
        break;
    }
    default:
        break;
    }
    return 0;
}

int HtmlLeaveSpan(MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    switch (type) {
    case MD_SPAN_EM:
        b.out << "</em>";
        break;
    case MD_SPAN_STRONG:
        b.out << "</strong>";
        break;
    case MD_SPAN_DEL:
        b.out << "</s>";
        break;
    case MD_SPAN_CODE:
        b.out << "</code>";
        break;
    case MD_SPAN_A:
        b.out << "</a>";
        break;
    case MD_SPAN_IMG: {
        if (!b.imgSrcStack.empty()) {
            const std::string src = std::move(b.imgSrcStack.back());
            b.imgSrcStack.pop_back();
            b.out << "<img src=\"";
            HtmlEscapeAttr(b.out, src);
            b.out << "\" alt=\"";
            HtmlEscapeAttr(b.out, b.imgAltBuf);
            b.out << "\"/>";
            b.imgAltBuf.clear();
        }
        if (b.imgSpanDepth > 0)
            --b.imgSpanDepth;
        break;
    }
    default:
        break;
    }
    return 0;
}

int HtmlTextCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& b = *static_cast<HtmlBuilder*>(userdata);
    if (type == MD_TEXT_NULLCHAR)
        return 0;
    if (type == MD_TEXT_HTML) {
#ifndef NDEBUG
        if (!b.debugLoggedMdTextHtml) {
            b.debugLoggedMdTextHtml = true;
            LOG_DEBUG("md4c: unexpected MD_TEXT_HTML under NOHTML (HTML path, first chunk size=%u)",
                      static_cast<unsigned>(size));
        }
#endif
        return 0;
    }
    const std::string txt(text, size);
    if (b.imgSpanDepth > 0 && b.codeBlockDepth == 0) {
        if (type == MD_TEXT_NORMAL || type == MD_TEXT_ENTITY || type == MD_TEXT_CODE) {
            b.imgAltBuf += txt;
            return 0;
        }
        if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
            b.imgAltBuf += ' ';
            return 0;
        }
        return 0;
    }
    if (type == MD_TEXT_BR) {
        b.out << "<br/>";
        return 0;
    }
    if (type == MD_TEXT_SOFTBR) {
        // See note in AdfTextCallback — emit a real line break so single-Enter behaves
        // intuitively in the editor rather than collapsing to a space.
        b.out << "<br/>";
        return 0;
    }
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
    for (int i = 0; i < spaces; ++i)
        out << ' ';
}

void EmitInlineText(const json& node, std::ostringstream& out) {
    if (!node.is_object())
        return;
    const std::string type = node.value("type", std::string());
    if (type == "text") {
        std::string text = node.value("text", std::string());
        // Apply marks innermost-first when emitting; ADF stores marks innermost-last in its array.
        // Scratch buffers are thread_local + const char* (no std::string heap churn on the hot
        // text-node path); capacity persists across calls within a thread.
        static thread_local std::vector<const char*> openWrap;
        static thread_local std::vector<const char*> closeWrap;
        openWrap.clear();
        closeWrap.clear();
        std::string href;
        bool isCode = false;
        if (node.contains("marks") && node["marks"].is_array()) {
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
            if (mtxt.empty())
                mtxt = std::string("@") + node["attrs"].value("id", std::string());
            out << mtxt;
        }
    } else if (type == "mediaInline" || type == "media") {
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
}

static bool MatchStoredTaskPrefix(const json& paraContent, bool* doneOut) {
    if (!paraContent.is_array() || paraContent.empty())
        return false;
    if (paraContent[0].value("type", std::string()) != "text")
        return false;
    const std::string& t = paraContent[0].at("text").get_ref<const std::string&>();
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

static std::string MarkdownCellPlainInner(const json& cell) {
    std::ostringstream o;
    if (cell.contains("content") && cell["content"].is_array()) {
        for (const auto& blk : cell["content"]) {
            if (blk.value("type", std::string()) == "paragraph") {
                EmitInlineRun(blk.value("content", json::array()), o);
            }
        }
    }
    std::string s = o.str();
    std::replace_if(s.begin(), s.end(), [](char ch) { return ch == '\n' || ch == '\r'; }, ' ');
    return s;
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

void EmitAdfBlock(const json& node, AdfWalkState& s) {
    if (!node.is_object()) {
        return;
    }
    const std::string type = node.value("type", std::string());

    if (type == "paragraph") {
        AppendIndent(s.out, s.listIndent);
        if (s.insideBlockquote)
            s.out << "> ";
        EmitInlineRun(node.value("content", json::array()), s.out);
        s.out << "\n\n";
    } else if (type == "heading") {
        const int level = (node.contains("attrs") && node["attrs"].is_object()) ? node["attrs"].value("level", 1) : 1;
        const int clamped = (std::max)(1, (std::min)(6, level));
        for (int i = 0; i < clamped; ++i)
            s.out << '#';
        s.out << ' ';
        EmitInlineRun(node.value("content", json::array()), s.out);
        s.out << "\n\n";
    } else if (type == "bulletList") {
        s.listMarkerStack.push_back('-');
        s.orderedCounters.push_back(0);
        EmitAdfChildren(node, s);
        s.listMarkerStack.pop_back();
        s.orderedCounters.pop_back();
        if (s.listIndent == 0)
            s.out << "\n";
    } else if (type == "orderedList") {
        const int start = (node.contains("attrs") && node["attrs"].is_object()) ? node["attrs"].value("order", 1) : 1;
        s.listMarkerStack.push_back('1');
        s.orderedCounters.push_back(start - 1);
        EmitAdfChildren(node, s);
        s.listMarkerStack.pop_back();
        s.orderedCounters.pop_back();
        if (s.listIndent == 0)
            s.out << "\n";
    } else if (type == "listItem") {
        AppendIndent(s.out, s.listIndent);
        const char marker = s.listMarkerStack.empty() ? '-' : s.listMarkerStack.back();
        bool taskItem = false;
        bool taskDone = false;
        if (node.contains("content") && node["content"].is_array() && !node["content"].empty()) {
            const auto& fc = node["content"][0];
            if (fc.is_object() && fc.value("type", std::string()) == "paragraph") {
                taskItem = MatchStoredTaskPrefix(fc.value("content", json::array()), &taskDone);
            }
        }
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
        // Render listItem children inline-style: the first paragraph stays on the marker line,
        // subsequent blocks indent.
        if (node.contains("content") && node["content"].is_array()) {
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
        } else {
            s.out << "\n";
        }
    } else if (type == "mediaSingle") {
        AppendIndent(s.out, s.listIndent);
        if (node.contains("content") && node["content"].is_array()) {
            for (const auto& ch : node["content"]) {
                if (ch.value("type", std::string()) == "media") {
                    EmitInlineText(ch, s.out);
                }
            }
        }
        s.out << "\n\n";
    } else if (type == "table") {
        EmitMarkdownTable(node, s);
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
            if (!current.empty() && current.back() != '\n')
                s.out << '\n';
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
        while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) && html[pos] != '=' &&
               html[pos] != '>' && html[pos] != '/') {
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
                if (pos < html.size())
                    ++pos;
            } else {
                while (pos < html.size() && !std::isspace(static_cast<unsigned char>(html[pos])) && html[pos] != '>' &&
                       html[pos] != '/') {
                    attrValue += html[pos++];
                }
            }
        }
        if (attrName == "href" && outTok.name == "a") {
            outTok.href = DecodeHtmlEntities(attrValue);
        }
        if (attrName == "src" && outTok.name == "img") {
            outTok.src = DecodeHtmlEntities(attrValue);
        }
        if (attrName == "alt" && outTok.name == "img") {
            outTok.alt = DecodeHtmlEntities(attrValue);
        }
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

std::string HtmlToMarkdown(const std::string& html, bool* outFellBack) {
    std::ostringstream out;
    bool fellBack = false;
    const auto& allowed = HtmlAllowedTags();

    std::string buffer;
    std::vector<std::string*> outPtrStack;
    outPtrStack.push_back(&buffer);
    auto tail = [&]() -> std::string& { return *outPtrStack.back(); };

    auto flushBuffer = [&](bool addBlankLine) {
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
    };

    std::vector<std::vector<std::string>> tableRows;
    int tableNest = 0;
    std::string cellAcc;

    struct Frame {
        std::string tag;
        int olCounter = 0; // for <ol> only
        int listIndent = 0;
    };
    std::vector<Frame> stack;
    // Href stack for <a> tags. Previously static thread_local — mid-parse exceptions left stale
    // state that corrupted the next call. Now a plain local: reset on every call, shared across
    // the open- and close-tag handlers below (they index into it via Frame::olCounter).
    std::vector<std::string> linkHrefs;

    auto isInlineMark = [](const std::string& t) {
        return t == "strong" || t == "b" || t == "em" || t == "i" || t == "s" || t == "del" || t == "u" || t == "code";
    };

    auto inlineOpenMd = [](const std::string& t) -> std::string {
        if (t == "strong" || t == "b")
            return "**";
        if (t == "em" || t == "i")
            return "*";
        if (t == "s" || t == "del")
            return "~~";
        if (t == "code")
            return "`";
        if (t == "u")
            return ""; // Markdown has no underline; pass through plain.
        return "";
    };

    auto appendPipeTable = [&](const std::vector<std::vector<std::string>>& rows) {
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
    };

    size_t pos = 0;
    while (pos < html.size()) {
        if (html[pos] == '<') {
            HtmlTagToken tok;
            const size_t before = pos;
            if (!ParseHtmlTag(html, pos, tok, fellBack)) {
                tail() += html[before];
                pos = before + 1;
                continue;
            }
            if (tok.name == "!ignored")
                continue;

            // Unknown tag → trip the fallback flag and suppress markup (text inside still emits).
            if (allowed.find(tok.name) == allowed.end()) {
                fellBack = true;
                continue;
            }

            const std::string& t = tok.name;
            const bool isVoid = (t == "br" || t == "hr" || t == "img");
            if (tok.isClose) {
                if (t == "td" || t == "th") {
                    if (outPtrStack.size() > 1 && outPtrStack.back() == &cellAcc) {
                        std::string finished = std::move(cellAcc);
                        cellAcc.clear();
                        outPtrStack.pop_back();
                        if (!tableRows.empty()) {
                            tableRows.back().push_back(std::move(finished));
                        }
                    }
                }
                if (t == "table") {
                    appendPipeTable(tableRows);
                    tableRows.clear();
                    if (tableNest > 0)
                        --tableNest;
                }

                std::string poppedLinkHref;
                bool poppedAny = false;
                const auto itClose =
                    std::find_if(stack.rbegin(), stack.rend(), [&](const Frame& fr) { return fr.tag == t; });
                if (itClose != stack.rend()) {
                    auto it = itClose;
                    if (t == "a" && it->olCounter > 0) {
                        const size_t idx = static_cast<size_t>(it->olCounter) - 1;
                        if (idx < linkHrefs.size()) {
                            poppedLinkHref = linkHrefs[idx];
                        }
                        if (idx + 1 == linkHrefs.size())
                            linkHrefs.pop_back();
                    }
                    const size_t targetIdx = std::distance(it, stack.rend()) - 1;
                    while (stack.size() > targetIdx + 1)
                        stack.pop_back();
                    stack.pop_back();
                    poppedAny = true;
                }
                (void)poppedAny;
                if (t == "p" || t == "div") {
                    tail() += "\n\n";
                } else if (t == "h1" || t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6") {
                    tail() += "\n\n";
                } else if (t == "li") {
                    tail() += "\n";
                } else if (t == "ul" || t == "ol") {
                    flushBuffer(true);
                } else if (t == "blockquote") {
                    tail() += "\n\n";
                } else if (t == "pre") {
                    tail() += "\n```\n\n";
                } else if (isInlineMark(t)) {
                    tail() += inlineOpenMd(t);
                } else if (t == "a") {
                    tail() += "](";
                    tail() += poppedLinkHref;
                    tail() += ")";
                }
                continue;
            }

            if (isVoid) {
                if (t == "br")
                    tail() += "  \n";
                else if (t == "hr")
                    tail() += "\n---\n\n";
                else if (t == "img") {
                    std::string escAlt;
                    for (char ch : tok.alt) {
                        if (ch == ']' || ch == '\\')
                            escAlt += '\\';
                        escAlt += ch;
                    }
                    tail() += "![" + escAlt + "](" + tok.src + ")";
                }
                continue;
            }

            if (t == "table") {
                if (tableNest > 0) {
                    fellBack = true;
                    continue;
                }
                ++tableNest;
                flushBuffer(true);
                tableRows.clear();
                stack.push_back({"table", 0, 0});
                continue;
            }
            if (t == "thead" || t == "tbody") {
                stack.push_back({t, 0, 0});
                continue;
            }
            if (t == "tr") {
                tableRows.emplace_back();
                stack.push_back({"tr", 0, 0});
                continue;
            }
            if (t == "td" || t == "th") {
                cellAcc.clear();
                outPtrStack.push_back(&cellAcc);
                stack.push_back({t, 0, 0});
                continue;
            }

            if (t == "p" || t == "div") {
                stack.push_back({t, 0, 0});
                continue;
            }
            if (t == "h1" || t == "h2" || t == "h3" || t == "h4" || t == "h5" || t == "h6") {
                stack.push_back({t, 0, 0});
                const int level = t[1] - '0';
                for (int i = 0; i < level; ++i)
                    tail() += '#';
                tail() += ' ';
                continue;
            }
            if (t == "ul" || t == "ol") {
                Frame f{t, 0, 0};
                if (!stack.empty())
                    f.listIndent = stack.back().listIndent + 2;
                stack.push_back(f);
                tail() += "\n";
                continue;
            }
            if (t == "li") {
                Frame f{"li", 0, 0};
                if (!stack.empty())
                    f.listIndent = stack.back().listIndent;
                stack.push_back(f);
                for (int i = 0; i < f.listIndent; ++i)
                    tail() += ' ';
                bool ordered = false;
                for (auto it = stack.rbegin() + 1; it != stack.rend(); ++it) {
                    if (it->tag == "ol") {
                        ++it->olCounter;
                        tail() += std::to_string(it->olCounter) + ". ";
                        ordered = true;
                        break;
                    }
                    if (it->tag == "ul") {
                        tail() += "- ";
                        break;
                    }
                }
                if (!ordered && stack.size() == 1)
                    tail() += "- ";
                continue;
            }
            if (t == "blockquote") {
                stack.push_back({t, 0, 0});
                tail() += "> ";
                continue;
            }
            if (t == "pre") {
                stack.push_back({t, 0, 0});
                tail() += "\n```\n";
                continue;
            }
            if (isInlineMark(t)) {
                stack.push_back({t, 0, 0});
                tail() += inlineOpenMd(t);
                continue;
            }
            if (t == "a") {
                stack.push_back({"a", 0, 0});
                tail() += "[";
                linkHrefs.push_back(tok.href);
                stack.back().olCounter = static_cast<int>(linkHrefs.size());
                continue;
            }
            if (t == "span") {
                stack.push_back({t, 0, 0});
                continue;
            }
            continue;
        }

        const size_t lt = html.find('<', pos);
        const size_t end = (lt == std::string::npos) ? html.size() : lt;
        const std::string raw = html.substr(pos, end - pos);
        tail() += DecodeHtmlEntities(raw);
        pos = end;
    }

    // Malformed HTML (e.g. unclosed <td>) can leave a nested text sink active — merge back and
    // flag fallback so callers can prefer raw-mode.
    while (outPtrStack.size() > 1) {
        fellBack = true;
        std::string orphan = std::move(*outPtrStack.back());
        outPtrStack.pop_back();
        tail() += orphan;
    }
    // If the document ended mid-table (missing </table>), appendPipeTable never fired on the
    // </table> close path and the accumulated rows would be lost. Flush them here so the
    // partial table at least renders as best-effort pipe-table output before the fallback flag
    // routes the caller to raw mode.
    if (!tableRows.empty()) {
        fellBack = true;
        appendPipeTable(tableRows);
        tableRows.clear();
    }

    flushBuffer(false);
    if (outFellBack)
        *outFellBack = fellBack;
    std::string result = out.str();
    // Collapse 3+ consecutive newlines into 2 (paragraph break).
    std::string collapsed;
    collapsed.reserve(result.size());
    int newlineRun = 0;
    for (char c : result) {
        if (c == '\n') {
            ++newlineRun;
            if (newlineRun <= 2)
                collapsed += c;
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
    parser.flags = Md4cParserFlags();
    parser.enter_block = AdfEnterBlock;
    parser.leave_block = AdfLeaveBlock;
    parser.enter_span = AdfEnterSpan;
    parser.leave_span = AdfLeaveSpan;
    parser.text = AdfTextCallback;
    parser.debug_log = Md4cDebugLogShim;
    parser.syntax = nullptr;

    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &builder);
    return builder.doc;
}

std::string AdfToMarkdown(const nlohmann::json& adf, std::vector<std::string>* outDroppedNodeTypes) {
    AdfWalkState s;
    if (adf.is_object()) {
        EmitAdfBlock(adf, s);
    } else if (adf.is_array()) {
        for (const auto& child : adf)
            EmitAdfBlock(child, s);
    }
    if (outDroppedNodeTypes)
        *outDroppedNodeTypes = std::move(s.dropped);

    // Trim trailing whitespace — most blocks emit "\n\n" suffix.
    std::string out = s.out.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

std::string MarkdownToHtml(const std::string& md) {
    HtmlBuilder builder;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = Md4cParserFlags();
    parser.enter_block = HtmlEnterBlock;
    parser.leave_block = HtmlLeaveBlock;
    parser.enter_span = HtmlEnterSpan;
    parser.leave_span = HtmlLeaveSpan;
    parser.text = HtmlTextCallback;
    parser.debug_log = Md4cDebugLogShim;
    parser.syntax = nullptr;

    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &builder);
    return builder.out.str();
}

std::string HtmlSubsetToMarkdown(const std::string& html, bool* outFellBack) {
    return HtmlToMarkdown(html, outFellBack);
}

} // namespace MarkdownConvert
