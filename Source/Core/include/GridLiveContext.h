#pragma once

// GridLiveContext — the per-pane live engine bundle (multi-grid foundation, ADR-0018;
// docs/plans/multi-grid-tabs.md Slice 1a). Holds everything one grid pane needs to be
// "live": its tracker backend, its streaming-sync service, and its in-memory active-ticket
// cache + published snapshot. Members moved VERBATIM from AppController (which now owns a
// `map<paneId, unique_ptr<GridLiveContext>>` — a single entry in Slice 1, so behaviour is
// identical to the old singleton members).
//
// Ownership / lifetime: owned by AppController via `std::unique_ptr` (the context holds
// atomics + a mutex, so it is non-movable — heap allocation keeps `&Backend` address-stable
// for the `std::atomic_load`/`atomic_store` discipline below). The retired-backend graveyard
// (ADR-0012) intentionally stays on AppController, shared by all contexts.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "CachedTicketTypes.h"

class ITrackerBackend;
class TicketSyncService;

struct GridLiveContext {
    // Ctor/dtor are out-of-line (GridLiveContext.cpp) so the `unique_ptr<TicketSyncService>`
    // member only needs the forward declaration here — AppController.h includes this header
    // for its context map, and pulling the full TicketSyncService.h into AppController.h's
    // ~105 includers would regress their compile cost for nothing.
    GridLiveContext();
    ~GridLiveContext();
    GridLiveContext(const GridLiveContext&) = delete;
    GridLiveContext& operator=(const GridLiveContext&) = delete;

    /// Shared (not unique) so off-thread workers can capture a strong handle that
    /// survives a live tracker swap freeing this slot. All reads go through
    /// `std::atomic_load`, all writes through `std::atomic_store`/`atomic_exchange`
    /// (a shared_ptr instance is not concurrently copy/assign-safe in C++14). See ADR 0012.
    std::shared_ptr<ITrackerBackend> Backend;

    std::vector<CachedTicket> ActiveTickets;
    mutable std::shared_ptr<const std::vector<CachedTicket>> activeTicketsPublished_;
    std::atomic<std::uint64_t> ActiveTicketsRevision{0};
    /// Guards ActiveTickets + activeTicketsPublished_ (same contract as the old
    /// AppController::activeTicketsMutex_).
    mutable std::mutex activeTicketsMutex_;

    /// Cache/queue namespacing key (= ConfigManager::NormalizeViewsBackendKey output).
    /// Declared in Slice 1a; populated + consumed by Slice 1b (tickets_v2 namespacing)
    /// and Slice 1c (offline-queue BackendKey). Empty until then.
    std::string backendKey;
    /// Resolved FieldCatalogCache key for this context (backend|endpoint|project).
    /// Declared in Slice 1a; the in-memory field-catalog block moves per-context in
    /// Slice 3 (design addendum § 3.1). Empty until then.
    std::string catalogKey;

    /// Owns the streaming-sync FSM (worker thread, batch queue, supersede/cancel
    /// transitions) and applies fetched batches to the cache. One sync FSM per context.
    /// Declared LAST so it is destroyed FIRST: its teardown joins the sync worker while
    /// Backend and the active-ticket state above are still alive.
    std::unique_ptr<TicketSyncService> ticketSync_;
};
