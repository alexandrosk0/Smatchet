#pragma once

// ViewListingDiffPure — which issues left a view, from two listings of what the view's own query returned. Pure:
// no I/O. Keyed by view, not pane: pane ids are recycled, and two panes on one view must report a departure once.
// See docs/plans/left-view-history-grid.md.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace smatchet {

/// A view's last complete listing, as saved in `lookup_cache`.
struct SavedViewListing {
    bool known = false;
    std::string query;
    std::vector<std::string> ids;
    std::int64_t savedAtUnixSec = 0;
};

/// True when two queries are the same apart from surrounding whitespace.
bool SameViewQuery(const std::string& a, const std::string& b);

/// Keys in `previous` the new listing no longer names. Empty unless the two listings answer the same query: a
/// changed query says nothing about the issues. An empty new listing yields nothing too, the same suspect case
/// the sync's own wipe guard refuses to act on. Keys the pane still holds through the hierarchy walk
/// (`stillHeld`) stay in the grid, so they have not left it. Order follows `previous`.
std::vector<std::string> DepartedFromListing(const SavedViewListing& previous, const std::vector<std::string>& listed,
                                             const std::string& query,
                                             const std::unordered_set<std::string>& stillHeld);

/// The `keys` a saved listing still names: a departure another pane on the view already recorded is not in it.
std::vector<std::string> KeysStillListed(const SavedViewListing& listing, const std::vector<std::string>& keys);

/// `listing` without `keys`.
std::vector<std::string> ListingWithout(const SavedViewListing& listing, const std::vector<std::string>& keys);

std::string ViewListingToJson(const std::string& query, const std::vector<std::string>& ids);

/// A listing from ViewListingToJson; malformed input leaves it unknown, so the next listing is only a baseline.
SavedViewListing ViewListingFromJson(const std::string& json, std::int64_t savedAtUnixSec);

} // namespace smatchet
