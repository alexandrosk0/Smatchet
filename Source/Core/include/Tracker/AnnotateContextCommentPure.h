#pragma once

// AnnotateContextCommentPure — the Annotate-context comment ("who last touched this line") as
// Markdown, so it posts through the ordinary comment path and the offline queue (Quality Pillar 6):
// every backend converts the Markdown to its own format (Jira: paragraphs + a `cpp` codeBlock, the
// shape the Jira-only ADF builder used to post). Pure: no I/O, no Logger.h.

#include <string>

namespace smatchet {
namespace tracker {

struct AnnotateContextFields {
    std::string P4User;
    std::string FunctionName;
    std::string FilePath;
    int LineNumber = 0;
    std::string Changelist;
    std::string Date;
    bool Approximated = false; ///< the annotate line was matched approximately
    std::string CodeSnippet;
};

/// A header line (user | function | file:line | CL | date), a note when the line was approximated,
/// then the snippet in a ```cpp fence. Header and note are Markdown-escaped; the fence is longer than
/// any backtick run in the snippet, so the snippet can never close it early.
std::string BuildAnnotateContextCommentMarkdown(const AnnotateContextFields& fields);

} // namespace tracker
} // namespace smatchet
