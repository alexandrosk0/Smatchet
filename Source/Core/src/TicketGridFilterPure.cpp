// Pure grid text-filter predicate — see TicketGridFilterPure.h for the contract.

#include "TicketGridFilterPure.h"

#include <cstddef>

namespace {

// A raw JSON payload only reaches fieldValues as nlohmann's compact dump (the attachment list
// the grid parses itself, or the unrecognized-object fallback), and a dump of a non-empty
// object or array always opens with one of these byte pairs. Display text never does.
bool IsRawJsonDump(const std::string& s) {
    if (s.size() < 2 || (s[0] != '{' && s[0] != '[')) {
        return false;
    }
    const char second = s[1];
    return second == '"' || second == '{' || second == '}' || second == ']';
}

unsigned char FoldAscii(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
}

std::string FoldAsciiCopy(const std::string& s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(FoldAscii(static_cast<unsigned char>(c)));
    }
    return out;
}

// This runs per keystroke over every field of every cached ticket, so the fold must stay a
// compare-and-subtract: std::search with a std::tolower comparator pays two locale calls per byte.
bool ContainsFolded(const std::string& haystack, const std::string& needleLower) {
    const std::size_t n = needleLower.size();
    const std::size_t h = haystack.size();
    if (n == 0) {
        return true;
    }
    if (h < n) {
        return false;
    }
    const unsigned char first = static_cast<unsigned char>(needleLower[0]);
    for (std::size_t i = 0; i + n <= h; ++i) {
        if (FoldAscii(static_cast<unsigned char>(haystack[i])) != first) {
            continue;
        }
        std::size_t k = 1;
        while (k < n &&
               FoldAscii(static_cast<unsigned char>(haystack[i + k])) == static_cast<unsigned char>(needleLower[k])) {
            ++k;
        }
        if (k == n) {
            return true;
        }
    }
    return false;
}

} // namespace

bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    const std::string needle = FoldAsciiCopy(filter);
    if (ContainsFolded(ticket.id, needle)) {
        return true;
    }
    for (const auto& fieldEntry : ticket.fieldValues) {
        if (IsRawJsonDump(fieldEntry.second)) {
            continue;
        }
        if (ContainsFolded(fieldEntry.second, needle)) {
            return true;
        }
    }
    return false;
}
