#pragma once

// GridLiveContext is the per-pane live engine bundle of the multi-grid foundation
// (ADR-0018, plan multi-grid-tabs Slice 1a): one grid pane's tracker backend, streaming-sync
// service, and in-memory active-ticket cache plus published snapshot, moved verbatim from
// AppController, which now owns a map from pane id to heap-owned context (one entry in
// Slice 1, so behaviour is unchanged). The struct holds atomics and a mutex, so it is
// non-movable; heap ownership keeps the Backend slot address-stable for the atomic
// load/store discipline below. The ADR-0012 retired-backend graveyard stays on AppController.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CachedTicketTypes.h"
#include "TrackerFieldSchema.h"

class ITrackerBackend;
class TicketSyncService;

/// Per-context in-memory field-catalog block (multi-grid Slice 3, plan item 17 +
/// slice1-design § 3.1). Moved verbatim out of AppController: the block is mutex-guarded but
/// SEMANTICALLY single-backend — two live different-backend panes sharing one copy would
/// overwrite each other's catalog. Each GridLiveContext now owns its own. Member names are
/// unchanged so the AppController call sites stay greppable against their history.
struct GridContextFieldCatalog {
    /// Bumped when the field catalog changes (fetch, error clear, etc.); UI sort caches key on it.
    std::atomic<std::uint64_t> TrackerFieldCatalogRevision{0};
    /// Guards every member below (same contract as the former AppController::availableFieldsMutex_).
    mutable std::mutex availableFieldsMutex_;
    std::vector<TrackerField> AvailableFields;
    std::vector<TrackerComponent> AvailableComponents;
    std::vector<TrackerIssueTypeCreateMeta> AvailableIssueTypeMeta;
    /// Last-fetched user catalog (see AppController::GetAvailableUsers for semantics).
    std::vector<TrackerUser> AvailableUsers;
    std::string LastTrackerFieldCatalogError;
    std::string LastTrackerFieldCatalogWarning;
    bool fieldCatalogEverLoaded_ = false;
    /// Project key for the most-recent in-flight catalog fetch (see SetCurrentCatalogProject).
    std::string currentCatalogProjectKey_;
    /// Per-project component option lists for cross-project grid views, keyed by project key.
    std::unordered_map<std::string, std::vector<TrackerFieldOption>> projectComponentOptions_;
    /// Project keys with an in-flight lazy component fetch (EnsureProjectComponentsLoaded).
    std::unordered_set<std::string> projectComponentsInFlight_;
    /// Per-project backoff after a FAILED component fetch (30 s relaunch guard).
    mutable std::unordered_map<std::string, std::chrono::steady_clock::time_point> projectComponentsRetryAfter_;
};

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

    /// Cache/queue namespacing key (NormalizeViewsBackendKey output). Wired in Slice 1b:
    /// set from the resolved tracker type at backend init and on every backend-kind swap;
    /// every LocalCacheManager ticket call scopes its rows with it. Reads cross threads
    /// (Lua automation worker, streaming-sync worker) while the UI thread may rewrite it on
    /// a tracker swap — access ONLY via the mutex-guarded accessors below.
    std::string CacheBackendKeyCopy() const {
        std::lock_guard<std::mutex> lk(backendKeyMutex_);
        return backendKey;
    }
    void SetCacheBackendKey(const std::string& key) {
        std::lock_guard<std::mutex> lk(backendKeyMutex_);
        backendKey = key;
    }
    /// Resolved field-catalog cache key for this context (backend, endpoint, project).
    /// Declared in Slice 1a; consumed when the catalog moves per-context in Slice 3.
    std::string catalogKey;

    /// Per-context in-memory field catalog (multi-grid Slice 3 — see struct doc above).
    GridContextFieldCatalog fieldCatalog;

    // --- Visibility lifecycle (multi-grid Slice 3, plan item 17) -----------------------
    // UI-thread-only bookkeeping driven by AppController::NotifyPaneVisible /
    // TickAllContexts: a pane that stops being drawn keeps its snapshot but no new syncs
    // are kicked for it; after the hidden-grace window its context is retired (backend to
    // the ADR-0012 graveyard) — EXCEPT the default pane's context, which is permanent
    // (offlineQueue_ holds a reference chain through its deps adapter).
    std::chrono::steady_clock::time_point lastVisibleAt{};
    bool everVisible = false;
    /// One-shot latch for AppController::EnsurePaneLiveSyncStarted (UI thread only):
    /// a non-focused visible pane's first sync is kicked exactly once per context
    /// generation (a retired-then-reshown pane gets a fresh context → fresh kick).
    bool initialSyncKicked = false;

  private:
    /// Guarded by backendKeyMutex_ — see CacheBackendKeyCopy/SetCacheBackendKey above.
    std::string backendKey;
    mutable std::mutex backendKeyMutex_;

  public:
    /// Owns the streaming-sync FSM (worker thread, batch queue, supersede/cancel) and applies
    /// fetched batches to the cache. Declared LAST so it is destroyed FIRST: teardown joins
    /// the sync worker while Backend and the active-ticket state above are still alive.
    std::unique_ptr<TicketSyncService> ticketSync_;
};
