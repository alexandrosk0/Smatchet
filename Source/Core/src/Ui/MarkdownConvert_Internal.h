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

// ---- Markdown -> ADF (Atlassian Document Format JSON) ----
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

// ---- Markdown -> HTML (Plane subset) ----
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
