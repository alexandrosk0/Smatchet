#ifndef SMATCHET_COMMANDS_FUZZY_MATCH_H
#define SMATCHET_COMMANDS_FUZZY_MATCH_H

// The command palette's historical entry points onto the shared matcher (TextMatchPure.h,
// docs/plans/input-ux-w2-one-matcher.md): the query is split into case-folded words that may occur in any
// order, a word that does not occur as written may match with its letters in order, and a candidate where
// every word occurs as written always scores above one that needed letters in order.
// New code calls smatchet::textmatch directly (it also returns the matched byte ranges).

#include <cstdint> // kept: includers have relied on it transitively
#include <string>

namespace smatchet {
namespace cmd {

/// 0 = no match. Positive ints are comparable for one query; only the relative order matters.
int FuzzyScore(const std::string& query, const std::string& candidate);

/// True iff every word of `query` matches `candidate`, as written or with its letters in order.
bool FuzzyHasSubsequence(const std::string& query, const std::string& candidate);

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_FUZZY_MATCH_H
