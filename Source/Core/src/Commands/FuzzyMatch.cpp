#include "Commands/FuzzyMatch.h"

#include "TextMatchPure.h"

namespace smatchet {
namespace cmd {

namespace {

// Above every Score the matcher returns, so an all-words-as-written match outranks any letters-in-order one.
constexpr int kExactTierBoost = textmatch::kMaxScore + 1;
static_assert(kExactTierBoost <= (1 << 30), "the boosted score must stay well inside int");

} // namespace

int FuzzyScore(const std::string& query, const std::string& candidate) {
    const textmatch::Result r = textmatch::Match(textmatch::MakeQuery(query), candidate, textmatch::Mode::Fuzzy);
    if (!r.Matched()) {
        return 0;
    }
    return r.Substring ? r.Score + kExactTierBoost : r.Score;
}

bool FuzzyHasSubsequence(const std::string& query, const std::string& candidate) {
    return textmatch::Match(textmatch::MakeQuery(query), candidate, textmatch::Mode::Fuzzy).Matched();
}

} // namespace cmd
} // namespace smatchet
