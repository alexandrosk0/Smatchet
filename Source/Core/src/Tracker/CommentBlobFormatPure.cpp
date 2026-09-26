#include "Tracker/CommentBlobFormatPure.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>

// See header. This TU is deliberately Logger.h-free / I/O-free so the doctest
// rig links it bare (the GitHubIssueSearchMapping purity precedent).

namespace smatchet {
namespace tracker {

namespace {

// Unix epoch seconds → "YYYY-MM-DD" (UTC calendar day). Empty on a non-positive
// epoch (unknown/unparsed timestamp) or a gmtime/strftime failure — the tooltip
// header degrades to "[Author] " rather than showing a bogus 1970 date.
std::string EpochSecToUtcDate(std::int64_t epochSec) {
    if (epochSec <= 0) {
        return std::string();
    }
    const std::time_t t = static_cast<std::time_t>(epochSec);
    // clang-format off
    // SMATCHET_DEVIATION(rule=duplication; reason=the portable gmtime_s/gmtime_r + strftime idiom also appears in SmatchetUserInfoUi.cpp (a Ui TU) — factoring a shared helper would couple this pure Tracker TU to Ui or grow a new cross-subsystem header for 12 lines of libc boilerplate, the exact coupling the DRY pillar marks CRITICAL; owner=orchestrator; revisit=when a shared pure time-format TU exists)
    // clang-format on
    std::tm tmv = {};
#if defined(_WIN32)
    if (gmtime_s(&tmv, &t) != 0) {
        return std::string();
    }
#else
    if (gmtime_r(&t, &tmv) == nullptr) {
        return std::string();
    }
#endif
    char buf[16] = {};
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv) == 0) {
        return std::string();
    }
    return std::string(buf);
}

// A CommonMark fence line: <=3 spaces of indent, then a run of >=3 '`' or '~'. A backtick
// run whose remainder contains a backtick is inline code, not a fence. On a match, reports
// the indent, fence char, run length, and whether only whitespace follows the run (the
// precondition for a CLOSING fence).
bool ParseFenceLine(const std::string& line, size_t& outIndent, char& outChar, size_t& outLen, bool& outBareRun) {
    size_t i = 0;
    while (i < line.size() && i < 3 && line[i] == ' ') {
        ++i;
    }
    if (i >= line.size() || (line[i] != '`' && line[i] != '~')) {
        return false;
    }
    const char c = line[i];
    size_t runEnd = i;
    while (runEnd < line.size() && line[runEnd] == c) {
        ++runEnd;
    }
    if (runEnd - i < 3 || (c == '`' && line.find('`', runEnd) != std::string::npos)) {
        return false;
    }
    outIndent = i;
    outChar = c;
    outLen = runEnd - i;
    outBareRun = line.find_first_not_of(" \t\r", runEnd) == std::string::npos;
    return true;
}

bool IsAsciiPunct(char c) {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

bool IsIsoCalendarDate(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') {
        return false;
    }
    for (size_t i = 0; i < s.size(); ++i) {
        if (i != 4 && i != 7 && !std::isdigit(static_cast<unsigned char>(s[i]))) {
            return false;
        }
    }
    return true;
}

// A History entry header as ParseChangelog writes it: "[Author] <date>", where the date is
// empty or YYYY-MM-DD. The strict date keeps a bracketed line inside a multi-line value
// ("[WIP] fix", "[1] 2 repro steps") that follows a blank line from being taken for an entry.
bool SplitActivityHeaderLine(const std::string& line, std::string& outAuthor, std::string& outDate) {
    if (line.size() < 3 || line[0] != '[') {
        return false;
    }
    const size_t close = line.rfind("] ");
    if (close == std::string::npos || close < 1) {
        return false;
    }
    const std::string date = line.substr(close + 2);
    if (!date.empty() && !IsIsoCalendarDate(date)) {
        return false;
    }
    outAuthor = line.substr(1, close - 1);
    outDate = date;
    return true;
}

} // namespace

std::string EscapeMarkdownText(const std::string& text) {
    std::string out;
    out.reserve(text.size() + text.size() / 4);
    for (const char c : text) {
        if (c == '\r' || c == '\n') {
            out.push_back(' ');
            continue;
        }
        if (IsAsciiPunct(c)) {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string CloseOpenCodeFence(const std::string& md) {
    size_t openIndent = 0;
    char openChar = '\0';
    size_t openLen = 0;
    size_t start = 0;
    while (start <= md.size()) {
        size_t end = md.find('\n', start);
        if (end == std::string::npos) {
            end = md.size();
        }
        size_t indent = 0;
        char c = '\0';
        size_t len = 0;
        bool bareRun = false;
        if (ParseFenceLine(md.substr(start, end - start), indent, c, len, bareRun)) {
            if (openLen == 0) {
                openIndent = indent;
                openChar = c;
                openLen = len;
            } else if (c == openChar && len >= openLen && bareRun) {
                openLen = 0;
            }
        }
        start = end + 1;
    }
    if (openLen == 0) {
        return md;
    }
    // Same indent as the opener, so a fence opened inside a list item closes inside it.
    std::string out = md;
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    out.append(openIndent, ' ');
    out.append(openLen, openChar);
    return out;
}

std::string ActivityEntryHeader(const std::string& author, const std::string& date) {
    // Trimmed, because "**Name **" is not a closed bold span. Only characters that can restyle
    // a name are escaped: emphasis, code, links, autolinks or HTML, strikethrough and entities.
    // Dots, dashes and digits stay as-is so the stored blob remains searchable by most names.
    const size_t first = author.find_first_not_of(" \t\r\n");
    const std::string name = first == std::string::npos
                                 ? std::string("Unknown")
                                 : author.substr(first, author.find_last_not_of(" \t\r\n") - first + 1);
    std::string header = "**";
    for (const char c : name) {
        if (c != '\0' && std::strchr("\\*_`<[]~&", c) != nullptr) {
            header.push_back('\\');
        }
        header.push_back(c == '\n' || c == '\r' ? ' ' : c);
    }
    header += "**";
    if (!date.empty()) {
        header += " " + date;
    }
    return header;
}

std::string PreserveLineBreaks(const std::string& md) {
    std::string out;
    out.reserve(md.size() + md.size() / 16);
    char fenceChar = '\0';
    size_t fenceLen = 0;
    size_t start = 0;
    while (start < md.size()) {
        size_t end = md.find('\n', start);
        const bool lastLine = end == std::string::npos;
        if (lastLine) {
            end = md.size();
        }
        std::string line = md.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back(); // CRLF → LF, so the hard-break spaces land at the true line end
        }
        size_t indent = 0;
        char c = '\0';
        size_t len = 0;
        bool bareRun = false;
        const bool isFence = ParseFenceLine(line, indent, c, len, bareRun);
        if (fenceLen == 0 && isFence) {
            fenceChar = c;
            fenceLen = len;
        } else if (fenceLen != 0 && isFence && c == fenceChar && len >= fenceLen && bareRun) {
            fenceLen = 0;
        }
        out += line;
        if (!lastLine) {
            const size_t next = end + 1;
            const size_t nextEnd = md.find('\n', next);
            const std::string nextLine =
                md.substr(next, nextEnd == std::string::npos ? std::string::npos : nextEnd - next);
            const bool lineBlank = line.find_first_not_of(" \t\r") == std::string::npos;
            const bool nextBlank = nextLine.find_first_not_of(" \t\r") == std::string::npos;
            // Two trailing spaces = CommonMark hard break. Not inside (or on) a fence, where
            // they would become literal code text.
            if (fenceLen == 0 && !isFence && !lineBlank && !nextBlank) {
                out += "  ";
            }
            out.push_back('\n');
        }
        start = end + 1;
    }
    return out;
}

std::string CommentBodyDisplayMarkdown(const std::string& body) {
    return PreserveLineBreaks(CloseOpenCodeFence(CleanCommentOutputAscii(body)));
}

std::string PlainActivityBlobToMarkdown(const std::string& blob) {
    std::string out;
    bool afterBlank = true;
    bool inParagraph = false;
    size_t start = 0;
    while (start < blob.size()) {
        size_t end = blob.find('\n', start);
        if (end == std::string::npos) {
            end = blob.size();
        }
        std::string line = blob.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t firstVisible = line.find_first_not_of(" \t");
        if (firstVisible == std::string::npos) {
            afterBlank = true;
            continue;
        }
        std::string author;
        std::string date;
        if (afterBlank && SplitActivityHeaderLine(line, author, date)) {
            if (!out.empty()) {
                out += "\n";
                out += kActivityEntrySeparator;
            }
            out += ActivityEntryHeader(author, date) + "\n\n";
            inParagraph = false;
        } else {
            if (inParagraph) {
                // A blank line keeps its paragraph break; a single newline becomes a hard break
                // (backslash-newline) instead of Markdown's soft break, which would join lines.
                out += afterBlank ? "\n\n" : "\\\n";
            }
            out += EscapeMarkdownText(line.substr(firstVisible));
            inParagraph = true;
        }
        afterBlank = false;
    }
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    return out;
}

std::string CleanCommentOutputAscii(const std::string& input) {
    std::string cleaned;
    cleaned.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        // 0xE2 0x80 0xA2 == U+2022 BULLET — rewrite to "* " so list markers
        // survive fonts without the glyph.
        if (i + 2 < input.size() && c == 0xE2 && static_cast<unsigned char>(input[i + 1]) == 0x80 &&
            static_cast<unsigned char>(input[i + 2]) == 0xA2) {
            cleaned += "* ";
            i += 2;
        } else {
            cleaned.push_back(static_cast<char>(c));
        }
    }
    return cleaned;
}

std::string FormatCommentBlob(const std::vector<TrackerIssueComment>& comments) {
    const size_t kMaxComments = 20;
    const size_t kMaxTotalLength = 12000;

    if (comments.empty()) {
        return std::string();
    }

    // Newest first. Seed the working order with the input REVERSED (backends
    // deliver oldest-first, so reversed = newest-first already), then
    // stable_sort by CreatedAtSec descending — entries sharing a timestamp
    // keep the reversed order, matching the original ParseComments rbegin()
    // iteration exactly.
    std::vector<const TrackerIssueComment*> ordered;
    ordered.reserve(comments.size());
    for (std::vector<TrackerIssueComment>::const_reverse_iterator it = comments.rbegin(); it != comments.rend(); ++it) {
        ordered.push_back(&(*it));
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const TrackerIssueComment* a, const TrackerIssueComment* b) {
        return a->CreatedAtSec > b->CreatedAtSec;
    });

    std::string result;
    size_t commentCount = 0;
    for (const TrackerIssueComment* comment : ordered) {
        if (commentCount >= kMaxComments) {
            break;
        }
        const std::string body = CommentBodyDisplayMarkdown(comment->Body);
        if (body.empty()) {
            continue;
        }
        const std::string entry =
            ActivityEntryHeader(comment->Author, EpochSecToUtcDate(comment->CreatedAtSec)) + "\n\n" + body + "\n";
        // Count the separator too so the cap holds for what is actually appended.
        const size_t separatorLen = commentCount > 0 ? std::strlen(kActivityEntrySeparator) : 0;
        const size_t appended = entry.size() + separatorLen;
        if (result.size() + appended > kMaxTotalLength) {
            break;
        }
        if (commentCount > 0) {
            result += kActivityEntrySeparator;
        }
        result += entry;
        commentCount++;
    }
    return result;
}

} // namespace tracker
} // namespace smatchet
