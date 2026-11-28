#pragma once

// The user-facing message every offline enqueue path (field edit, pending action, new-issue create, the Lua and
// CLI creates) fails with while the durable cache cannot take a write. While the cache is being recreated the queue
// lives in the database file the recreate is about to delete, so a write in that window would be silently wiped.
// While it is opening there is no file yet, and in a session-only run the cache is memory-only and would lose the
// write at exit. Each path checks IOfflineQueueDeps::EnqueueBlock() BEFORE it takes the cache and fails loudly with the
// matching text instead. Localized, so no surface carries its own copy of the wording.

#include "Persistence/LocalCacheStatusPure.h"
#include "SmatchetLocalization.h"

#include <string>

namespace smatchet {
namespace sync {

inline std::string CacheRecreatingEnqueueMessage() {
    return SmatchetLocalization::T("offline.cache_recreating",
                                   "The local database is being recreated, so this cannot be queued offline right "
                                   "now. Retry in a moment.");
}

/// The refusal text for `block`; empty for CacheEnqueueBlock::None.
inline std::string CacheEnqueueBlockedMessage(persistence::CacheEnqueueBlock block) {
    switch (block) {
    case persistence::CacheEnqueueBlock::Recreating:
        return CacheRecreatingEnqueueMessage();
    case persistence::CacheEnqueueBlock::Opening:
        return SmatchetLocalization::T("offline.cache_opening",
                                       "The local database is still opening, so this cannot be queued offline right "
                                       "now. Retry in a moment.");
    case persistence::CacheEnqueueBlock::SessionOnly:
        return SmatchetLocalization::T("offline.cache_session_only",
                                       "The local database could not be opened, so this cannot be queued offline. "
                                       "Restart Smatchet or check Settings -> Preferences -> Local data.");
    case persistence::CacheEnqueueBlock::None:
        break;
    }
    return std::string();
}

} // namespace sync
} // namespace smatchet
