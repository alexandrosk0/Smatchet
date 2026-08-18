#include "Tracker/CommentBlobFormatPure.h"

#include <algorithm>
#include <cstdint>
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

} // namespace

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
        const std::string body = CleanCommentOutputAscii(comment->Body);
        if (body.empty()) {
            continue;
        }
        const std::string author = comment->Author.empty() ? std::string("Unknown") : comment->Author;
        const std::string entry = "[" + author + "] " + EpochSecToUtcDate(comment->CreatedAtSec) + "\n" + body + "\n";
        // +1 for the blank-line separator so the cap holds for what is actually appended.
        const size_t appended = entry.size() + (commentCount > 0 ? 1 : 0);
        if (result.size() + appended > kMaxTotalLength) {
            break;
        }
        if (commentCount > 0) {
            result += "\n";
        }
        result += entry;
        commentCount++;
    }
    return result;
}

} // namespace tracker
} // namespace smatchet
