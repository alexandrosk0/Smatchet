#include "Tracker/AnnotateContextCommentPure.h"

#include "Tracker/CommentBlobFormatPure.h"

#include <algorithm>
#include <cstddef>

namespace smatchet {
namespace tracker {

namespace {

std::size_t LongestBacktickRun(const std::string& text) {
    std::size_t longest = 0;
    std::size_t run = 0;
    for (const char c : text) {
        run = c == '`' ? run + 1 : 0;
        longest = (std::max)(longest, run);
    }
    return longest;
}

} // namespace

std::string BuildAnnotateContextCommentMarkdown(const AnnotateContextFields& fields) {
    const std::string line = std::to_string(fields.LineNumber);
    std::string md =
        EscapeMarkdownText("Annotate \xE2\x80\x94 " + fields.P4User + " | " + fields.FunctionName + " | " +
                           fields.FilePath + ":" + line + " | CL " + fields.Changelist + " | " + fields.Date);
    md += "\n\n";
    if (fields.Approximated) {
        md += EscapeMarkdownText("Note: annotate is approximated (exact line not found in annotate).");
        md += "\n\n";
    }
    const std::string code =
        "L" + line + "  CL:" + fields.Changelist + "  " + fields.P4User + "\n" + fields.CodeSnippet;
    const std::string fence((std::max)(std::size_t(3), LongestBacktickRun(code) + 1), '`');
    md += fence + "cpp\n" + code;
    if (!code.empty() && code.back() != '\n') {
        md += '\n';
    }
    md += fence + "\n";
    return md;
}

} // namespace tracker
} // namespace smatchet
