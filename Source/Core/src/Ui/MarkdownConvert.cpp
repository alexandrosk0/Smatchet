// Public-API spine for the Markdown <-> ADF / HTML conversion pipeline. The engines were split
// into cohesive companion TUs (see the god-file-splits plan):
//   * MarkdownToAdf.cpp  — md4c -> ADF
//   * MarkdownToHtml.cpp — md4c -> HTML
//   * AdfToMarkdown.cpp  — ADF -> Markdown walker + HTML-subset -> Markdown state machine
// This file owns only the public entry points, the shared md4c parser-flag source of truth, and
// the md4c debug-log shim; the engine state + callbacks live under MarkdownConvert::md_detail
// (declared in MarkdownConvert_Internal.h). Body moves are behavior-identical.

#include "MarkdownConvert.h"
#include "MarkdownConvert_Internal.h"

#include "Logger.h"

extern "C" {
#include "md4c.h"
}

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

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

} // namespace

// Public API

nlohmann::json MarkdownToAdf(const std::string& md) {
    md_detail::AdfBuilder builder;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = Md4cParserFlags();
    parser.enter_block = md_detail::AdfEnterBlock;
    parser.leave_block = md_detail::AdfLeaveBlock;
    parser.enter_span = md_detail::AdfEnterSpan;
    parser.leave_span = md_detail::AdfLeaveSpan;
    parser.text = md_detail::AdfTextCallback;
    parser.debug_log = Md4cDebugLogShim;
    parser.syntax = nullptr;

    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &builder);
    return builder.doc;
}

std::string AdfToMarkdown(const nlohmann::json& adf, std::vector<std::string>* outDroppedNodeTypes) {
    md_detail::AdfWalkState s;
    if (adf.is_object()) {
        md_detail::EmitAdfBlock(adf, s);
    } else if (adf.is_array()) {
        for (const auto& child : adf)
            md_detail::EmitAdfBlock(child, s);
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
    md_detail::HtmlBuilder builder;

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = Md4cParserFlags();
    parser.enter_block = md_detail::HtmlEnterBlock;
    parser.leave_block = md_detail::HtmlLeaveBlock;
    parser.enter_span = md_detail::HtmlEnterSpan;
    parser.leave_span = md_detail::HtmlLeaveSpan;
    parser.text = md_detail::HtmlTextCallback;
    parser.debug_log = Md4cDebugLogShim;
    parser.syntax = nullptr;

    md_parse(md.data(), static_cast<MD_SIZE>(md.size()), &parser, &builder);
    return builder.out.str();
}

std::string HtmlSubsetToMarkdown(const std::string& html, bool* outFellBack) {
    return md_detail::HtmlToMarkdown(html, outFellBack);
}

} // namespace MarkdownConvert
