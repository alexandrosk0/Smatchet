// Markdown -> HTML (Plane subset) engine. Split out of MarkdownConvert.cpp
// (see the god-file-splits plan). Behavior-identical body move; state + entry-point
// declarations live in MarkdownConvert_Internal.h.

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

} // namespace md_detail
} // namespace MarkdownConvert
