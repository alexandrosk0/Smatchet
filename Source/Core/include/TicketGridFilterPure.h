#pragma once

// Pure (ImGui-free) grid text-filter predicate lifted out of SmatchetActiveProjectGridTable.cpp.

#include "CachedTicketTypes.h"

#include <string>

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

/// Full-text match: true when `filter` (ASCII case-insensitive substring; empty always matches)
/// occurs in the ticket id or any displayable field value — through `display` when given, so a stored
/// id matches by the name the grid shows. Raw JSON payloads in fieldValues (attachment lists,
/// unrecognized-object fallbacks) and internal keys are skipped, so hidden ids/emails never match.
bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter,
                             const IGridFilterDisplay* display = nullptr);
