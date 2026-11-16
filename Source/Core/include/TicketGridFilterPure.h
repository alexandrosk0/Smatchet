#pragma once

// Pure (ImGui-free) grid text-filter predicate lifted out of SmatchetActiveProjectGridTable.cpp.

#include "CachedTicketTypes.h"

#include <cstddef>
#include <string>
#include <vector>

/// What the grid shows for a stored field value, for the filter: an option-backed field may store an
/// id (Plane's State and Cycle hold uuids) while the cell shows the option's name.
class IGridFilterDisplay {
  public:
    virtual ~IGridFilterDisplay() = default;
    /// The text shown for `stored` in field `fieldId`; `stored` itself when the field maps no ids.
    virtual const std::string& Shown(const std::string& fieldId, const std::string& stored) const = 0;
};

/// True for a fieldValues key that is bookkeeping, never shown in the grid: Plane's internal `uuid`
/// and the `_smatchet_*` sentinels (e.g. GitHub's `_smatchet_is_pr`).
bool IsInternalTicketFieldKey(const std::string& key);

/// The most words one filter is matched on. Words past this many are ignored, which only
/// makes the match looser; the cap lets the matcher track found words in a fixed-size mask
/// instead of allocating per ticket.
constexpr std::size_t kGridFilterMaxTokens = 32;

/// The words of a filter: split on ASCII whitespace (space, tab, CR, LF — the set
/// TrimCopyAsciiWhitespace trims), ASCII-lowercased, at most kGridFilterMaxTokens of them.
/// Empty for an empty or whitespace-only filter.
std::vector<std::string> SplitGridFilterTokens(const std::string& filter);

/// Full-text match on pre-split words (see SplitGridFilterTokens): true when EVERY word occurs,
/// as an ASCII case-insensitive substring, in the ticket id or a displayable field value. Each
/// word may match a different field, in any order ("crash login" finds "Login page crashes").
/// No words always matches. Field values go through `display` when given, so a stored id matches
/// by the name the grid shows. Raw JSON payloads in fieldValues (attachment lists,
/// unrecognized-object fallbacks) and internal keys are skipped, so hidden ids/emails never match.
/// Split once and call this per ticket: the grid runs it over every cached ticket per keystroke.
bool TicketMatchesGridFilterTokens(const CachedTicket& ticket, const std::vector<std::string>& foldedTokens,
                                   const IGridFilterDisplay* display = nullptr);

/// Convenience form of TicketMatchesGridFilterTokens that splits `filter` on every call.
bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter,
                             const IGridFilterDisplay* display = nullptr);
