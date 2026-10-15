#pragma once

// WalkOwnedKeysPure - which resident rows a pane holds only because of the hierarchy walk. A reload adds the
// parents and the children of the view's own rows; the view's roster never lists them, so a membership check
// against the roster would read each of them as "left the view" (and the grid would drop them with a toast).
// C++14; ParentKeyOf is the only call that links against the parent-hierarchy helpers.

#include "CachedTicketTypes.h"
#include "Tracker/ParentHierarchyPure.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace smatchet {
namespace sync {

/// Ids of the rows in `resident` the membership check must leave alone because the hierarchy walk owns them.
/// - A row that is the parent of another resident row: the walk loads it for that child whether or not the view
///   lists it.
/// - With `viewListed` (the ids the pane's last complete reload got from the view's own query; null = unknown):
///   every resident row that list does not name. Those are the walk's children, and rows the pane gained
///   outside the query (a ticket created here); neither can be shown to have left the view, so they stay.
/// A listed row the roster no longer names is the one case left over: it left the view.
inline std::unordered_set<std::string> WalkOwnedKeys(const std::vector<CachedTicket>& resident,
                                                     const std::unordered_set<std::string>* viewListed) {
    std::unordered_set<std::string> present;
    present.reserve(resident.size());
    for (const CachedTicket& row : resident) {
        present.insert(row.id);
    }
    std::unordered_set<std::string> owned;
    for (const CachedTicket& row : resident) {
        const std::string parentKey = ParentHierarchyPure::ParentKeyOf(row);
        if (!parentKey.empty() && present.find(parentKey) != present.end()) {
            owned.insert(parentKey);
        }
        if (viewListed != nullptr && viewListed->find(row.id) == viewListed->end()) {
            owned.insert(row.id);
        }
    }
    return owned;
}

} // namespace sync
} // namespace smatchet
