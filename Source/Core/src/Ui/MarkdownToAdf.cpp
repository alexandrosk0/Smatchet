// Markdown -> ADF (Atlassian Document Format JSON) engine, split out of the MarkdownConvert god
// file for the god-file-splits decomposition. Behavior-identical body move. Shared state plus the
// engine entry-point declarations live in the MarkdownConvert_Internal header.

#include "MarkdownConvert.h"
#include "MarkdownConvert_Internal.h"

#include "Logger.h"

extern "C" {
#include "md4c.h"
}

#include <nlohmann/json.hpp>

// clang-format off
// SMATCHET_DEVIATION(rule=duplication; reason=the shared engine-TU include block + namespace-open boilerplate is grandfathered across the MarkdownConvert split siblings (MarkdownToAdf / MarkdownToHtml / AdfToMarkdown) — a behavior-preserving god-file partition has no shared prologue header to factor into without worse coupling, and the DRY gate doc endorses an exemption over cross-context abstraction; owner=orchestrator; revisit=when a shared MarkdownConvert TU prologue header is introduced)
// clang-format on

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

// Markdown -> ADF (Atlassian Document Format JSON)

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

// Push a typed container node onto the current parent and open a child-content frame for it.
// Shared by the table cell/row/table cases. Mirrors the inline push pattern verbatim.
void AdfPushContainerWithChildren(AdfBuilder& b, json node) {
    auto* parent = b.topContent();
    parent->push_back(std::move(node));
    b.contentStack.push_back(&parent->back()["content"]);
}

// Table block group (table, header/body wrappers, rows, header/data cells). Each pushes its own
// container (header/body wrappers are transparent) and returns true to signal the type is handled,
// so the main switch in AdfEnterBlock can early-return. Returns false for non-table block types.
bool AdfTryEnterTableBlock(AdfBuilder& b, MD_BLOCKTYPE type) {
    switch (type) {
    case MD_BLOCK_TABLE:
        AdfPushContainerWithChildren(b, {{"type", "table"},
                                         {"attrs", {{"layout", "default"}, {"isNumberColumnEnabled", false}}},
                                         {"content", json::array()}});
        return true;
    case MD_BLOCK_THEAD:
    case MD_BLOCK_TBODY:
        return true;
    case MD_BLOCK_TR:
        AdfPushContainerWithChildren(b, {{"type", "tableRow"}, {"content", json::array()}});
        return true;
    case MD_BLOCK_TH:
        AdfPushContainerWithChildren(b, {{"type", "tableHeader"}, {"content", json::array()}});
        return true;
    case MD_BLOCK_TD:
        AdfPushContainerWithChildren(b, {{"type", "tableCell"}, {"content", json::array()}});
        return true;
    default:
        return false;
    }
}

int AdfEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& b = *static_cast<AdfBuilder*>(userdata);

    if (AdfTryEnterTableBlock(b, type)) {
        return 0;
    }

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

} // namespace md_detail
} // namespace MarkdownConvert
