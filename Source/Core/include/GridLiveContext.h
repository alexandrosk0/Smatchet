#pragma once

// GridLiveContext is the per-pane live engine bundle of the multi-grid foundation
// (ADR-0018, plan multi-grid-tabs Slice 1a): one grid pane's tracker backend, streaming-sync
// service, and in-memory active-ticket cache plus published snapshot, moved verbatim from
// AppController, which now owns a map from pane id to heap-owned context (one entry in
// Slice 1, so behaviour is unchanged). The struct holds atomics and a mutex, so it is
// non-movable; heap ownership keeps the Backend slot address-stable for the atomic
// load/store discipline below. The ADR-0012 retired-backend graveyard stays on AppController.

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
    // Ctor/dtor are out-of-line (GridLiveContext.cpp) so the sync-service member only needs
    // a forward declaration here; pulling the full TicketSyncService header into
    // AppController.h would regress the compile cost of its roughly 105 includers.
    GridLiveContext();
    ~GridLiveContext();
    GridLiveContext(const GridLiveContext&) = delete;
    GridLiveContext& operator=(const GridLiveContext&) = delete;

    /// Shared (not unique) so off-thread workers can capture a strong handle that survives
    /// a live tracker swap freeing this slot. All reads latch via atomic load, all writes go
    /// through atomic store/exchange — shared_ptr is not concurrently copy-safe. See ADR 0012.
    std::shared_ptr<ITrackerBackend> Backend;

    std::vector<CachedTicket> ActiveTickets;
    mutable std::shared_ptr<const std::vector<CachedTicket>> activeTicketsPublished_;
    std::atomic<std::uint64_t> ActiveTicketsRevision{0};
    /// Guards ActiveTickets + activeTicketsPublished_ (same contract as the old
    /// AppController::activeTicketsMutex_).
    mutable std::mutex activeTicketsMutex_;

    /// Cache/queue namespacing key (NormalizeViewsBackendKey output). Declared in Slice 1a;
    /// populated and consumed by Slices 1b and 1c. Empty until then.
    std::string backendKey;
    /// Resolved field-catalog cache key for this context (backend, endpoint, project).
    /// Declared in Slice 1a; consumed when the catalog moves per-context in Slice 3.
    std::string catalogKey;

    /// Owns the streaming-sync FSM (worker thread, batch queue, supersede/cancel) and applies
    /// fetched batches to the cache. Declared LAST so it is destroyed FIRST: teardown joins
    /// the sync worker while Backend and the active-ticket state above are still alive.
    std::unique_ptr<TicketSyncService> ticketSync_;
};
