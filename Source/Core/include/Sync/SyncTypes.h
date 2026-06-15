#ifndef SMATCHET_SYNC_SYNC_TYPES_H
#define SMATCHET_SYNC_SYNC_TYPES_H

// Leaf home for the two tracker-sync result structs shared by Sync/TicketSyncService.h
// and AppController.h. Relocated out of AppController.h so the Sync layer can depend on
// these types WITHOUT including AppController.h — severing the Sync -> AppController
// include back-edge (core-include-dag Phase 3).
//
// Both types are in the GLOBAL namespace, exactly where they lived in AppController.h
// (between the closed `smatchet::lua` block and `class AppController`), so the relocation
// is byte-identical with no namespace/ADL shift; AppController.h includes this header in
// their place. Dependency-light: CachedTicketTypes.h (for CachedTicket) + <string>/<vector>.

#include "CachedTicketTypes.h"

#include <string>
#include <vector>

/** Single consolidated tracker degraded/offline banner for main windows (replaces stacked warnings). */
struct TrackerConnectivityBannerForUi {
    enum class Level { None, Warning, Error };
    Level Kind = Level::None;
    std::string Message;
};

/** Raw tracker issue fetch result; apply on the UI thread via AppController::ApplyIssueFetchPack. */
struct TrackerIssueFetchPack {
    std::vector<CachedTicket> Tickets;
    bool FullSyncCompleted = false;
    std::string FetchError;
    /// Soft caveat (e.g. pagination cap). See TrackerIssueFetchSummary::Warning.
    std::string Warning;
};

#endif // SMATCHET_SYNC_SYNC_TYPES_H
