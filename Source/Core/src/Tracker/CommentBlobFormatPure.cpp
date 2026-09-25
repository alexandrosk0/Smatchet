#include "Tracker/CommentBlobFormatPure.h"

#include <algorithm>
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

// A CommonMark fence line: <=3 spaces of indent, then a run of >=3 '`' or '~'.
// On a match, reports the fence char, the run length, and whether only
// whitespace follows the run (the precondition for a CLOSING fence).
bool ParseFenceLine(const std::string& line, char& outChar, size_t& outLen, bool& outBareRun) {
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
    if (runEnd - i < 3) {
        return false;
    }
    outChar = c;
    outLen = runEnd - i;
    outBareRun = line.find_first_not_of(" \t\r", runEnd) == std::string::npos;
    return true;
}

} // namespace

std::string EscapeMarkdownInline(const std::string& text) {
    static const char kSpecial[] = "\\`*_[]<>#|~!&$";
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char c : text) {
        if (c == '\r' || c == '\n') {
            out.push_back(' ');
        } else {
            if (std::strchr(kSpecial, c) != nullptr) {
                out.push_back('\\');
            }
            out.push_back(c);
        }
    }
    return out;
}

std::string CloseOpenCodeFence(const std::string& md) {
    char openChar = '\0';
    size_t openLen = 0;
    size_t start = 0;
    while (start <= md.size()) {
        size_t end = md.find('\n', start);
        if (end == std::string::npos) {
            end = md.size();
        }
        char c = '\0';
        size_t len = 0;
        bool bareRun = false;
        if (ParseFenceLine(md.substr(start, end - start), c, len, bareRun)) {
            if (openLen == 0) {
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
    std::string out = md;
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    out.append(openLen, openChar);
    return out;
}

std::string ActivityEntryHeader(const std::string& author, const std::string& date) {
    std::string header = "**" + EscapeMarkdownInline(author.empty() ? std::string("Unknown") : author) + "**";
    if (!date.empty()) {
        header += " " + date;
    }
    return header;
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
        const std::string body = CloseOpenCodeFence(CleanCommentOutputAscii(comment->Body));
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
