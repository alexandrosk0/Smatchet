#pragma once

// RemoteChangeInvalidationPure - what a remote change to a resident ticket drops (docs/plans/active/delta-sync.md
// § Phase 7): its transitions and its edit permissions were learned at the old tracker version. They are cached
// per tracker namespace, so the drop must name the namespace of the pane that saw the change, not whichever pane is
// focused. Header-only, no I/O.

#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace sync {

/// Calls `invalidateTransitions(backendKey, id)` and `invalidateEditMeta(backendKey, id)` for each of `ids`.
template <typename InvalidateTransitions, typename InvalidateEditMeta>
inline void InvalidateRemotelyChangedTickets(const std::string& backendKey, const std::vector<std::string>& ids,
                                             InvalidateTransitions&& invalidateTransitions,
                                             InvalidateEditMeta&& invalidateEditMeta) {
    for (const std::string& id : ids) {
        invalidateTransitions(backendKey, id);
        invalidateEditMeta(backendKey, id);
    }
}

} // namespace sync
} // namespace smatchet
