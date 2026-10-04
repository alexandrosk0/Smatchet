#ifndef SMATCHET_MARKDOWN_CONVERT_INTERNAL_H
#define SMATCHET_MARKDOWN_CONVERT_INTERNAL_H

// Implementation-detail header shared by the four MarkdownConvert TUs:
//   * MarkdownConvert.cpp     — public-API spine (parser setup + entry points)
//   * MarkdownToAdf.cpp       — md4c -> ADF engine
//   * MarkdownToHtml.cpp      — md4c -> HTML engine
//   * AdfToMarkdown.cpp       — ADF -> Markdown walker + HTML-subset -> Markdown state machine
// Not part of the public surface — never include from Source/Core/include/ headers or from
// any non-MarkdownConvert .cpp. All shared symbols live under `MarkdownConvert::md_detail`
// so the linker names stay segregated from anything the project may grow elsewhere.

extern "C" {
#include "md4c.h"
}

#include "Logger.h"

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>
#include <vector>

namespace MarkdownConvert {
namespace md_detail {

using nlohmann::json;

// Shared by the ADF and HTML md4c engines (moved as inline so both TUs see one definition).
inline std::string MdAttrToString(const MD_ATTRIBUTE& attr) {
    if (attr.text == nullptr || attr.size == 0)
        return std::string();
    return std::string(attr.text, attr.size);
}

/// The href of an MD_SPAN_A, from the `detail` md4c passes to an enter-span callback.
inline std::string MdLinkHref(const void* detail) {
    const auto* d = static_cast<const MD_SPAN_A_DETAIL*>(detail);
    return d ? MdAttrToString(d->href) : std::string();
}

/// md4c reports an image's alt text as ordinary text events between the image span's enter and
/// leave callbacks, so the ADF and HTML engines collect it here instead of emitting it.
struct MdImageSpan {
    int depth = 0;
    std::string alt;
    std::vector<std::string> srcStack;
};

/// MD_SPAN_IMG enter, given the `detail` md4c passes: remember the src and start a fresh alt text.
inline void EnterImageSpan(MdImageSpan& img, const void* detail) {
    const auto* d = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
    img.srcStack.push_back(d ? MdAttrToString(d->src) : std::string());
    ++img.depth;
    img.alt.clear();
}

/// MD_SPAN_IMG leave, first half: move the innermost image's src into `outSrc`. False when md4c sent
/// no matching enter, in which case there is nothing to emit.
inline bool PopImageSrc(MdImageSpan& img, std::string& outSrc) {
    if (img.srcStack.empty()) {
        return false;
    }
    outSrc = std::move(img.srcStack.back());
    img.srcStack.pop_back();
    return true;
}

/// MD_SPAN_IMG leave, second half, after the engine has emitted the image with `img.alt`.
inline void LeaveImageSpan(MdImageSpan& img) {
    img.alt.clear();
    if (img.depth > 0) {
        --img.depth;
    }
}

/// Text inside an image span (and outside a code block) is alt text: collect it and return true so
/// the engine emits nothing for it. Line breaks become one space.
inline bool AbsorbImageAltText(MdImageSpan& img, int codeBlockDepth, MD_TEXTTYPE type, const MD_CHAR* text,
                               MD_SIZE size) {
    if (img.depth <= 0 || codeBlockDepth != 0) {
        return false;
    }
    if (type == MD_TEXT_NORMAL || type == MD_TEXT_ENTITY || type == MD_TEXT_CODE) {
        img.alt.append(text, size);
    } else if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
        img.alt += ' ';
    }
    return true;
}

/// Text events both engines drop: NUL characters, and raw HTML, which md4c should not emit under
/// MD_FLAG_NOHTML (ignored defensively, for ABI or dialect drift). Debug builds log the first raw-HTML
/// chunk of a parse once; `engine` names the path in that line.
inline bool SkipIgnoredMdText(MD_TEXTTYPE type, MD_SIZE size, bool& loggedRawHtml, const char* engine) {
    if (type == MD_TEXT_NULLCHAR) {
        return true;
    }
    if (type != MD_TEXT_HTML) {
        return false;
    }
#ifndef NDEBUG
    if (!loggedRawHtml) {
        loggedRawHtml = true;
        LOG_DEBUG("md4c: unexpected MD_TEXT_HTML under NOHTML (%s path, first chunk size=%u)", engine,
                  static_cast<unsigned>(size));
    }
#else
    (void)size;
    (void)loggedRawHtml;
    (void)engine;
#endif
    return true;
}

/// State both md4c engines keep for the part of the text pipeline they share.
struct MdEngineState {
    int codeBlockDepth = 0;
    MdImageSpan img;
    /// At most one LOG_DEBUG per md_parse if md4c emits MD_TEXT_HTML despite NOHTML.
    bool debugLoggedMdTextHtml = false;
};

/// True when a text event produces no output of its own: an ignored event (SkipIgnoredMdText) or
/// image alt text (AbsorbImageAltText). `engine` names the path in the debug log.
inline bool ConsumeNonEmittedMdText(MdEngineState& st, const char* engine, MD_TEXTTYPE type, const MD_CHAR* text,
                                    MD_SIZE size) {
    return SkipIgnoredMdText(type, size, st.debugLoggedMdTextHtml, engine) ||
           AbsorbImageAltText(st.img, st.codeBlockDepth, type, text, size);
}

// ---- Markdown -> ADF (Atlassian Document Format JSON) ----
struct AdfBuilder : MdEngineState {
    json doc;
    /// Stack of pointers into `doc` — each entry is the `content` array of the currently-open
    /// container block. Top of stack is where new child nodes get pushed.
    std::vector<json*> contentStack;
    /// Inline marks currently active for emitted text nodes (innermost first).
    std::vector<json> markStack;
    /// Blockquote nesting depth. Jira ADF blockquote content allows only paragraph / bulletList /
    /// orderedList — nested blockquote is a schema violation. Flatten nested quotes into the outer
    /// blockquote by suppressing inner wrappers; depth tracks pairing so leave-block pops correctly.
    int blockquoteDepth = 0;

    AdfBuilder() : doc({{"type", "doc"}, {"version", 1}, {"content", json::array()}}) {
        contentStack.push_back(&doc["content"]);
    }

    json* topContent() { return contentStack.back(); }
};

// ---- Markdown -> HTML (Plane subset) ----
struct HtmlBuilder : MdEngineState {
    std::ostringstream out;
};

// ---- ADF -> Markdown (recursive walker) ----
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
    /// ADF block-nesting depth — bounded in EmitAdfBlock so server-supplied,
    /// deeply-nested ADF can't stack-overflow the mutually-recursive walk.
    int depth = 0;
};

// Engine entry points referenced by the public-API spine in MarkdownConvert.cpp.
// Markdown -> ADF engine (defined in MarkdownToAdf.cpp):
int AdfEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata);
int AdfLeaveBlock(MD_BLOCKTYPE type, void* detail, void* userdata);
int AdfEnterSpan(MD_SPANTYPE type, void* detail, void* userdata);
int AdfLeaveSpan(MD_SPANTYPE type, void* detail, void* userdata);
int AdfTextCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata);

// Markdown -> HTML engine (defined in MarkdownToHtml.cpp):
int HtmlEnterBlock(MD_BLOCKTYPE type, void* detail, void* userdata);
int HtmlLeaveBlock(MD_BLOCKTYPE type, void* detail, void* userdata);
int HtmlEnterSpan(MD_SPANTYPE type, void* detail, void* userdata);
int HtmlLeaveSpan(MD_SPANTYPE type, void* detail, void* userdata);
int HtmlTextCallback(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata);

// ADF -> Markdown / HTML-subset -> Markdown engine (defined in AdfToMarkdown.cpp):
void EmitAdfBlock(const json& node, AdfWalkState& s);
std::string HtmlToMarkdown(const std::string& html, bool* outFellBack);

} // namespace md_detail
} // namespace MarkdownConvert

#endif // SMATCHET_MARKDOWN_CONVERT_INTERNAL_H
