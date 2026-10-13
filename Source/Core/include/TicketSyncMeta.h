#pragma once

// TicketSyncMeta — what the delta sync remembers about a cached ticket row: the tracker's
// "updated" stamp it was fetched at, the field ids it was fetched with, and when (docs/plans/
// active/delta-sync.md § Phase 2). A rank-0 leaf (includes only CachedTicketTypes.h) so the cache
// layer, the sync layer and the test fakes share ONE definition of the pure rules below without
// Persistence reaching up into Sync. Header-only, C++14, no platform headers (builds in the
// standalone and DX12 targets and in the doctest rig).
//
// The field list is stored as the list itself, not a hash: panes with different columns share rows,
// and "does this row carry what MY view needs" is a subset question a hash cannot answer - a hash
// would make two panes refetch each other's rows forever.

#include "CachedTicketTypes.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct TicketSyncMeta {
    /// The tracker's "updated" stamp for the row, verbatim. Empty = unknown (the row is refetched).
    std::string RemoteUpdated;
    /// Sorted, unique ids of the fields the row carries values for.
    std::vector<std::string> Fields;
    /// Wall clock (epoch seconds) of the save; the delta sync's max-row-age bound reads it.
    std::int64_t SyncedAtEpochSec = 0;
};

namespace smatchet {
namespace sync {

/// Sorted unique copy of `fields` (the canonical stored form).
inline std::vector<std::string> NormalizeFieldList(std::vector<std::string> fields) {
    std::sort(fields.begin(), fields.end());
    fields.erase(std::unique(fields.begin(), fields.end()), fields.end());
    return fields;
}

/// Union of two canonical field lists.
inline std::vector<std::string> UnionFields(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::vector<std::string> merged;
    merged.reserve(a.size() + b.size());
    merged.insert(merged.end(), a.begin(), a.end());
    merged.insert(merged.end(), b.begin(), b.end());
    return NormalizeFieldList(std::move(merged));
}

/// True when `rowFields` (canonical, sorted) contains every id in `required`. Empty ids in
/// `required` are ignored; an empty `required` is always satisfied.
inline bool FieldsSatisfy(const std::vector<std::string>& rowFields, const std::vector<std::string>& required) {
    for (const std::string& id : required) {
        if (id.empty()) {
            continue;
        }
        if (!std::binary_search(rowFields.begin(), rowFields.end(), id)) {
            return false;
        }
    }
    return true;
}

/// Ids of the fields a fetched row carries a value slot for (the keys of `fieldValues`: the backend
/// mappers set a slot, blank when absent, for every field the fetch asked for).
inline std::vector<std::string> FieldsOfRow(const CachedTicket& ticket) {
    std::vector<std::string> fields;
    fields.reserve(ticket.fieldValues.size());
    for (const auto& kv : ticket.fieldValues) {
        fields.push_back(kv.first);
    }
    return NormalizeFieldList(std::move(fields));
}

/// The meta to store with a freshly fetched row. `RemoteUpdated` comes from the row's "updated"
/// field; a row without one gets an empty stamp (callers skip storing meta for those).
inline TicketSyncMeta BuildSyncMeta(const CachedTicket& ticket, std::int64_t nowEpochSec) {
    TicketSyncMeta meta;
    meta.RemoteUpdated = ticket.GetFieldValue("updated");
    meta.Fields = FieldsOfRow(ticket);
    meta.SyncedAtEpochSec = nowEpochSec;
    return meta;
}

/// Saving `incoming` over a cached row that was fetched at the SAME tracker version: the two differ
/// only in which fields each fetch asked for (two panes with different columns), so keep both - the
/// incoming values win where it has them, the cached row's other fields stay. Without this each pane's
/// save would drop the other pane's columns and the two would refetch each other's rows indefinitely.
inline CachedTicket MergeSameVersionRow(const CachedTicket& existing, const CachedTicket& incoming) {
    CachedTicket merged = incoming;
    for (const auto& kv : existing.fieldValues) {
        merged.fieldValues.insert(kv); // insert never overwrites: incoming wins
    }
    for (const auto& kv : existing.fieldRichValues) {
        merged.fieldRichValues.insert(kv);
    }
    return merged;
}

/// True when a row saved with `incoming` meta over a cached row with `existing` meta is the same
/// tracker version (both stamps known and equal), i.e. MergeSameVersionRow applies.
inline bool IsSameVersion(const TicketSyncMeta& existing, const TicketSyncMeta& incoming) {
    return !existing.RemoteUpdated.empty() && existing.RemoteUpdated == incoming.RemoteUpdated;
}

} // namespace sync
} // namespace smatchet
