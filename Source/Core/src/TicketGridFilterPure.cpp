// Pure grid text-filter predicate — see TicketGridFilterPure.h for the contract.

#include "TicketGridFilterPure.h"

#include "TextMatchPure.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

// A raw JSON payload only reaches fieldValues as nlohmann's compact dump (the attachment list
// the grid parses itself, or the unrecognized-object fallback). A dump of a non-empty object
// or array opens with a quote-or-brace pair AND closes with the matching bracket as its last
// byte; an empty one is exactly two bytes. Checking both ends keeps bracketed prose such as
// "[]. release notes" or "[Bug] crash" searchable.
bool IsRawJsonDump(const std::string& s) {
    const std::size_t n = s.size();
    if (n < 2) {
        return false;
    }
    const char first = s[0];
    const char second = s[1];
    const char last = s[n - 1];
    if (n == 2) {
        return (first == '{' && second == '}') || (first == '[' && second == ']');
    }
    if (first == '{') {
        return second == '"' && last == '}';
    }
    if (first == '[') {
        return (second == '{' || second == '"') && last == ']';
    }
    return false;
}

// The separators between filter words: the same four bytes TrimCopyAsciiWhitespace trims, so
// the trimmed text the grid passes in and the words split from it agree on what a space is.
bool IsFilterWordSeparator(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// This runs per keystroke over every field of every cached ticket, so the compare allocates nothing:
// FindFolded decodes and folds both sides as it goes (an ASCII byte pair is a compare-and-add).
bool ContainsFolded(const std::string& haystack, const std::string& needleFolded) {
    return smatchet::textmatch::FindFolded(haystack, needleFolded) != std::string::npos;
}

// Clears the bit of every still-missing word that occurs in `text`.
void MarkWordsFoundIn(const std::string& text, const std::vector<std::string>& foldedTokens, std::size_t count,
                      std::uint32_t& missing) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t bit = std::uint32_t(1) << i;
        if ((missing & bit) != 0U && ContainsFolded(text, foldedTokens[i])) {
            missing &= ~bit;
        }
    }
}

} // namespace

bool IsInternalTicketFieldKey(const std::string& key) { return key == "uuid" || (!key.empty() && key[0] == '_'); }

std::vector<std::string> SplitGridFilterTokens(const std::string& filter) {
    std::vector<std::string> tokens;
    std::string word;
    for (const char c : filter) {
        if (!IsFilterWordSeparator(c)) {
            word.push_back(c);
            continue;
        }
        if (!word.empty()) {
            tokens.push_back(smatchet::textmatch::FoldCopy(word));
            word.clear();
        }
    }
    if (!word.empty()) {
        tokens.push_back(smatchet::textmatch::FoldCopy(word));
    }
    if (tokens.size() > kGridFilterMaxTokens) {
        tokens.resize(kGridFilterMaxTokens);
    }
    return tokens;
}

bool TicketMatchesGridFilterTokens(const CachedTicket& ticket, const std::vector<std::string>& foldedTokens,
                                   const IGridFilterDisplay* display) {
    const std::size_t count = (std::min)(foldedTokens.size(), kGridFilterMaxTokens);
    if (count == 0) {
        return true;
    }
    // One bit per word still to find; a word found in the id or any field stays found.
    static_assert(kGridFilterMaxTokens <= 32, "the found-word mask is 32 bits wide");
    std::uint32_t missing = count >= 32 ? ~std::uint32_t(0) : ((std::uint32_t(1) << count) - 1U);
    MarkWordsFoundIn(ticket.id, foldedTokens, count, missing);
    if (missing == 0U) {
        return true;
    }
    for (const auto& fieldEntry : ticket.fieldValues) {
        if (IsInternalTicketFieldKey(fieldEntry.first) || IsRawJsonDump(fieldEntry.second)) {
            continue;
        }
        const std::string& shown =
            display != nullptr ? display->Shown(fieldEntry.first, fieldEntry.second) : fieldEntry.second;
        MarkWordsFoundIn(shown, foldedTokens, count, missing);
        if (missing == 0U) {
            return true;
        }
    }
    return false;
}

bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter, const IGridFilterDisplay* display) {
    return TicketMatchesGridFilterTokens(ticket, SplitGridFilterTokens(filter), display);
}

bool TicketTitleMatchesGridFilterFuzzy(const CachedTicket& ticket, const smatchet::textmatch::Query& query,
                                       smatchet::textmatch::Scratch& scratch) {
    if (query.Empty()) {
        return true;
    }
    const std::string& title = ticket.GetFieldValueRef(kGridFilterTitleField);
    return smatchet::textmatch::Match(query, title.data(), title.size(), smatchet::textmatch::Mode::Fuzzy, scratch)
        .Matched();
}
