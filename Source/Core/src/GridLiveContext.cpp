#include "GridLiveContext.h"

// Full definition required here: ~GridLiveContext destroys the
// unique_ptr<TicketSyncService> member (forward-declared in the header — see the
// header comment for why the include lives in this TU and not there).
#include "TicketSyncService.h"
// Full ViewDefinition required so ~GridLiveContext can destroy the
// shared_ptr<const ViewDefinition> resolvedOwnView member (forward-declared in the header).
#include "ConfigManager.h"

#include <atomic>

// One disjoint 2^32-wide band per context. The low half stays available for this context's own
// SetBackend / retirement bumps (a pane would need 4 billion backend swaps in one process lifetime
// to reach the next band), so no two live contexts ever share a generation value.
std::uint64_t NextBackendGenerationSeed() {
    static std::atomic<std::uint64_t> s_nextBand(1);
    return s_nextBand.fetch_add(1) << 32;
}

GridLiveContext::GridLiveContext() = default;

GridLiveContext::~GridLiveContext() = default;
