#pragma once

// Pure (ImGui-free) grid text-filter predicate lifted out of SmatchetActiveProjectGridTable.cpp.

#include "CachedTicketTypes.h"

#include <string>

/// Full-text match: true when `filter` (case-insensitive substring, empty always matches) is
/// found in the ticket id or any field value. User fields reach CachedTicket already flattened
/// to their display name by the tracker field-value parser, so this matches usernames, not ids.
bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter);
