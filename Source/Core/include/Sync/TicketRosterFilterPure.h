#pragma once

// TicketRosterFilterPure — pure keep-set filtering over a ticket roster. Extracted because the
// same erase-remove-if appears on both sides of the multi-pane cache scoping: the sync service
// converges a pane's in-memory roster onto the ids its OWN query returned, and the local-data
// refresh narrows the namespace-wide `GetAllTickets` read down to the same recorded set (the
// SQLite cache is one backend-keyed namespace shared by every pane, ADR-0018 decision 4).
// Header-only + dependency-free so both the Sync TU and the AppController TU can use it.

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <vector>

#include "CachedTicketTypes.h"

namespace smatchet {

/// Erase every row whose `id` is absent from `keep`, preserving the order of the survivors.
/// Returns the number of rows removed (0 = the roster already matched the keep set).
inline std::size_t RetainTicketsInKeepSet(std::vector<CachedTicket>& rows,
                                          const std::unordered_set<std::string>& keep) {
    const std::size_t before = rows.size();
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&keep](const CachedTicket& t) { return keep.find(t.id) == keep.end(); }),
               rows.end());
    return before - rows.size();
}

} // namespace smatchet
