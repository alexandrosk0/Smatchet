#ifndef SMATCHET_SYNC_SYNC_TYPES_H
#define SMATCHET_SYNC_SYNC_TYPES_H

// Leaf home for the two tracker-sync result structs shared by Sync/TicketSyncService.h
// and AppController.h. Relocated out of AppController.h so the Sync layer can depend on
// these types without including AppController.h, severing the Sync to AppController
// include back-edge (core-include-dag Phase 3). Both types stay in the global namespace,
// exactly where they lived in AppController.h, so the relocation is byte-identical with no
// namespace or ADL shift; AppController.h includes this header in their place.
// Dependency-light: CachedTicketTypes.h plus the standard string and vector headers.

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
    /// Transport-shaped FetchError (offline / DNS / timeout / 5xx-style) — classified ONCE where
    /// the pack is composed; consumers branch on this flag, never re-classify the text
    /// (retire-transport-error-text slice 1, backlog N12).
    bool FetchErrorTransient = false;
    /// Soft caveat (e.g. pagination cap). See TrackerIssueFetchSummary::Warning.
    std::string Warning;
};

#endif // SMATCHET_SYNC_SYNC_TYPES_H
