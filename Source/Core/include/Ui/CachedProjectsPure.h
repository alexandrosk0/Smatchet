#pragma once

// Pure rule for the cached-projects list of the Preferences Tracker tab and the JQL project pill
// (UiDrawSession::cachedProjectsSnapshot): the list is read on a worker, and a "Forget" click removes an entry from
// the snapshot at once but from the file only on the next read. Header-only, no I/O; generic over the entry type
// (FieldCatalogCache::CachedProjectEntry in production), which needs `projectKey`, `backend` and `endpoint`.

#include <cstddef>
#include <vector>

namespace smatchet {
namespace ui {

/// Drops from `rows` every entry that `forgotten` names (same project key, backend and endpoint). A read that was
/// started before a Forget lands with the entry still in it: without this the entry reappears in the list until the
/// next read, as if the Forget had not worked. Returns how many entries were dropped.
template <class Entry>
std::size_t RemoveForgottenProjects(std::vector<Entry>& rows, const std::vector<Entry>& forgotten) {
    if (forgotten.empty()) {
        return 0;
    }
    std::size_t dropped = 0;
    for (auto it = rows.begin(); it != rows.end();) {
        bool isForgotten = false;
        for (const Entry& gone : forgotten) {
            if (it->projectKey == gone.projectKey && it->backend == gone.backend && it->endpoint == gone.endpoint) {
                isForgotten = true;
                break;
            }
        }
        if (isForgotten) {
            it = rows.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    return dropped;
}

} // namespace ui
} // namespace smatchet
