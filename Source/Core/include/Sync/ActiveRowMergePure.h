#pragma once

// ActiveRowMergePure - merging one frame's rows into a pane's in-memory roster (docs/plans/active/delta-sync.md
// § Phase 3). The roster is looked up through an id -> index map; the map lives for a whole sync session instead
// of being rebuilt for every slice of the apply drain, and is rebuilt only when the roster changed behind it.
// Header-only, C++14, no platform headers; the caller holds the roster's lock.

#include "CachedTicketTypes.h"
#include "Sync/RemoteVersionPure.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace smatchet {
namespace sync {

/// id -> position in the roster, valid while the roster's revision and row count match what the map was built
/// (or last updated) for. A hit is re-checked against the row it points at, so a map that went stale in a way the
/// revision did not announce rebuilds instead of misplacing a row.
struct ActiveRowIndex {
    std::unordered_map<std::string, std::size_t> Positions;
    std::uint64_t Revision = 0;
    std::size_t Rows = 0;
    bool Valid = false;
    std::size_t Builds = 0; ///< times the map was built from scratch (cumulative; diagnostics and tests)

    /// Drops the map (and its memory); the next merge rebuilds it.
    void Release() {
        std::unordered_map<std::string, std::size_t>().swap(Positions);
        Valid = false;
    }
};

/// True when `now` is a different tracker version of the row `was` stands for: both stamps known, naming
/// different instants.
inline bool RowVersionMoved(const CachedTicket& was, const CachedTicket& now) {
    const std::string& before = was.GetFieldValueRef("updated");
    const std::string& after = now.GetFieldValueRef("updated");
    return !before.empty() && !after.empty() && !IsSameRemoteVersion(before, after);
}

namespace active_row_merge_detail {

inline void Rebuild(const std::vector<CachedTicket>& active, ActiveRowIndex& index) {
    index.Positions.clear();
    index.Positions.reserve(active.size());
    for (std::size_t i = 0; i < active.size(); ++i) {
        index.Positions.emplace(active[i].id, i); // the first of a duplicated id wins
    }
    ++index.Builds;
}

/// Position of `id` in `active`, or false when it is not there. Rebuilds the map once if a hit points at the
/// wrong row.
inline bool Find(const std::vector<CachedTicket>& active, ActiveRowIndex& index, const std::string& id,
                 std::size_t& position) {
    auto it = index.Positions.find(id);
    if (it == index.Positions.end()) {
        return false;
    }
    if (it->second >= active.size() || active[it->second].id != id) {
        Rebuild(active, index);
        it = index.Positions.find(id);
        if (it == index.Positions.end()) {
            return false;
        }
    }
    position = it->second;
    return true;
}

} // namespace active_row_merge_detail

/// Merges one frame's rows into `active`. Freshly fetched rows always replace; cache-served rows replace only when
/// the row is missing or differs (and is not provably older than the resident copy), so a reload that changed
/// nothing republishes nothing. A resident row whose
/// tracker version moved (by either kind of row) is listed in `movedIds`: what hangs off the old version is stale.
/// `revision` is the roster's revision counter read under the same lock as `active`; after bumping it for a change
/// the caller records the new value in `index.Revision`. Returns whether anything changed.
inline bool MergeRowsIntoActive(std::vector<CachedTicket>& active, std::uint64_t revision, ActiveRowIndex& index,
                                const std::vector<CachedTicket>& fetched, const std::vector<CachedTicket>& unchanged,
                                std::vector<std::string>& movedIds) {
    if (!index.Valid || index.Revision != revision || index.Rows != active.size()) {
        active_row_merge_detail::Rebuild(active, index);
    }
    bool changed = false;
    const auto place = [&](const CachedTicket& row, bool onlyIfDifferent) {
        std::size_t position = 0;
        if (!active_row_merge_detail::Find(active, index, row.id, position)) {
            index.Positions.emplace(row.id, active.size());
            active.push_back(row);
            changed = true;
            return;
        }
        CachedTicket& current = active[position];
        if (onlyIfDifferent &&
            ((current.fieldValues == row.fieldValues && current.fieldRichValues == row.fieldRichValues) ||
             IsIncomingRowStale(row.GetFieldValueRef("updated"), current.GetFieldValueRef("updated")))) {
            return; // nothing new - or the pane holds a provably newer copy than the cache does (a write in flight)
        }
        if (RowVersionMoved(current, row)) {
            movedIds.push_back(row.id);
        }
        current = row;
        changed = true;
    };
    for (const CachedTicket& row : fetched) {
        place(row, false);
    }
    for (const CachedTicket& row : unchanged) {
        place(row, true);
    }
    index.Rows = active.size();
    index.Revision = revision;
    index.Valid = true;
    return changed;
}

} // namespace sync
} // namespace smatchet
