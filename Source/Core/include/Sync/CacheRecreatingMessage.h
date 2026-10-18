#pragma once

// The one user-facing message every offline enqueue path (field edit, pending action, new-issue create, the Lua and
// CLI creates) fails with while the local cache is being recreated. The queue lives in the database file the
// recreate is about to delete, so a write in that window would be silently wiped; each path checks
// IOfflineQueueDeps::IsCacheRecreating() BEFORE it takes the cache and fails loudly with this text instead.
// Localized (key offline.cache_recreating), so no surface carries its own copy of the wording.

#include "SmatchetLocalization.h"

#include <string>

namespace smatchet {
namespace sync {

inline std::string CacheRecreatingEnqueueMessage() {
    return SmatchetLocalization::T("offline.cache_recreating",
                                   "The local database is being recreated, so this cannot be queued offline right "
                                   "now. Retry in a moment.");
}

} // namespace sync
} // namespace smatchet
