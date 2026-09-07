#pragma once

// Pure (ImGui-free) grid text-filter predicate lifted out of SmatchetActiveProjectGridTable.cpp.

#include "CachedTicketTypes.h"

#include <string>

/// Full-text match: true when `filter` (ASCII case-insensitive substring; empty always matches)
/// occurs in the ticket id or any displayable field value. Raw JSON payloads in fieldValues
/// (attachment lists, unrecognized-object fallbacks) are skipped, so hidden ids/emails never match.
bool TicketMatchesGridFilter(const CachedTicket& ticket, const std::string& filter);
