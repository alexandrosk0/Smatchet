#include "GridLiveContext.h"

// Full definition required here: ~GridLiveContext destroys the
// unique_ptr<TicketSyncService> member (forward-declared in the header — see the
// header comment for why the include lives in this TU and not there).
#include "TicketSyncService.h"
// Full ViewDefinition required so ~GridLiveContext can destroy the
// shared_ptr<const ViewDefinition> resolvedOwnView member (forward-declared in the header).
#include "ConfigManager.h"

GridLiveContext::GridLiveContext() = default;

GridLiveContext::~GridLiveContext() = default;
