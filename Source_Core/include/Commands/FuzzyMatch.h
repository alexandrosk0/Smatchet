#ifndef SMATCHET_COMMANDS_FUZZY_MATCH_H
#define SMATCHET_COMMANDS_FUZZY_MATCH_H

// Sublime-style subsequence fuzzy matcher for command palette + did-you-mean.
//
// Scoring rewards (descending importance):
//   - All query chars are matched as an in-order subsequence of candidate
//   - Consecutive matches (camelCase / dotted run)
//   - Matches at start of candidate / after a separator (`.`, `_`, ` `)
//   - Case-exact matches
//
// Returns 0 when there is no subsequence match. Higher is better.

#include <cstdint>
#include <string>

namespace smatchet {
namespace cmd {

/// Sublime-style score. 0 = no match. Positive ints are comparable; only the
/// relative order matters.
int FuzzyScore(const std::string& query, const std::string& candidate);

/// True iff every char of `query` (lowercased) appears in `candidate`
/// (lowercased) as an in-order subsequence.
bool FuzzyHasSubsequence(const std::string& query, const std::string& candidate);

}  // namespace cmd
}  // namespace smatchet

#endif  // SMATCHET_COMMANDS_FUZZY_MATCH_H
