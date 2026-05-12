#include "Commands/FuzzyMatch.h"

#include <cctype>

namespace smatchet {
namespace cmd {

namespace {

inline char AsciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline bool IsSeparator(char c) {
    return c == '.' || c == '_' || c == '-' || c == ' ' || c == '/' || c == '\\';
}

}  // namespace

bool FuzzyHasSubsequence(const std::string& query, const std::string& candidate) {
    if (query.empty()) return true;
    size_t qi = 0;
    for (size_t ci = 0; ci < candidate.size() && qi < query.size(); ++ci) {
        if (AsciiLower(candidate[ci]) == AsciiLower(query[qi])) {
            ++qi;
        }
    }
    return qi == query.size();
}

int FuzzyScore(const std::string& query, const std::string& candidate) {
    if (query.empty()) return 1;
    if (candidate.empty()) return 0;

    // First pass — make sure the subsequence exists. Cheap reject.
    if (!FuzzyHasSubsequence(query, candidate)) {
        return 0;
    }

    // Bonus weights — tuned to match user expectations for command-palette UX.
    constexpr int kBaseMatch = 1;
    constexpr int kBonusConsecutive = 5;
    constexpr int kBonusStart = 6;
    constexpr int kBonusAfterSeparator = 4;
    constexpr int kBonusExactCase = 1;

    int score = 0;
    size_t qi = 0;
    bool prevMatched = false;

    for (size_t ci = 0; ci < candidate.size() && qi < query.size(); ++ci) {
        const char cChar = candidate[ci];
        const char qChar = query[qi];
        if (AsciiLower(cChar) != AsciiLower(qChar)) {
            prevMatched = false;
            continue;
        }
        score += kBaseMatch;
        if (cChar == qChar) {
            score += kBonusExactCase;
        }
        if (ci == 0) {
            score += kBonusStart;
        } else if (IsSeparator(candidate[ci - 1])) {
            score += kBonusAfterSeparator;
        }
        if (prevMatched) {
            score += kBonusConsecutive;
        }
        prevMatched = true;
        ++qi;
    }

    // Modest penalty for unmatched suffix length (prefer shorter candidates).
    if (candidate.size() > query.size()) {
        const int trail = static_cast<int>(candidate.size() - query.size());
        score -= trail / 8;
    }

    return score > 0 ? score : 1;
}

}  // namespace cmd
}  // namespace smatchet
