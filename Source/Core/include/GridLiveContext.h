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
struct ViewDefinition;

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
    /// Transport-shaped LastTrackerFieldCatalogError (N12 item 13b) — classified where the
    /// catalog fetch's TrackerError is flattened; set/cleared with the string.
    bool LastTrackerFieldCatalogErrorTransient = false;
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

/// Per-context lazy in-memory group roster (user-info-window Slice 3, plan item 15;
/// "group roster" per Tracker CONTEXT.md — NOT "group catalog", which collides with the
/// field catalog). Mutex-guarded like GridContextFieldCatalog and per-context for the same
/// reason: rosters are backend-scoped, so two live different-backend panes must not share
/// one copy. Filled lazily by the AppController group delegators; never persisted.
struct GridContextGroupRoster {
    /// Guards both members below.
    mutable std::mutex rosterMutex_;
    /// FetchGroupMembers results keyed by group name (active users, deduped by AccountId).
    std::unordered_map<std::string, std::vector<TrackerUser>> MembersByGroup;
    std::string LastGroupRosterError;
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

    /// Backend-generation token (issue #1081): bumped by GridContextDepsAdapter::SetBackend
    /// (after the atomic_exchange) and by AppController::retireExpiredHiddenContexts_. In-flight
    /// workers capture the value at work-capture time and re-check it immediately before
    /// applying results into this context — a mismatch means the backend was swapped/retired
    /// mid-flight and the stale apply is dropped (capture-then-check; cancel/await was rejected:
    /// it would block the UI thread up to an HTTP timeout, Pillar 2).
    std::atomic<std::uint64_t> backendGeneration_{0};

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

    /// Per-context lazy group roster (user-info-window Slice 3 — see struct doc above).
    GridContextGroupRoster groupRoster;

    // --- Visibility lifecycle (multi-grid Slice 3, plan item 17) -----------------------
    // UI-thread-only bookkeeping driven by AppController::NotifyPaneVisible /
    // TickAllContexts: a pane that stops being drawn keeps its snapshot but no new syncs
    // are kicked for it; after the hidden-grace window its context is retired (backend to
    // the ADR-0012 graveyard) — EXCEPT the default pane's context, which is permanent
    // (offlineQueue_ holds a reference chain through its deps adapter).
    std::chrono::steady_clock::time_point lastVisibleAt{};
    bool everVisible = false;
    /// Monotonic LRU recency stamp (multi-grid Slice 5a, plan item 21): assigned from
    /// AppController::paneVisibilityClock_ each frame the pane is visible (UI thread only, in
    /// EnsurePaneContextLive). Smaller == less recently visible == evicted first by the
    /// hidden-pane memory cap. A deterministic counter rather than a wall-clock so the
    /// eviction order is reproducible in tests (mirrors the ActiveTicketsRevision pattern).
    std::uint64_t lastVisibleOrder = 0;
    /// Frame index (AppController::paneFrameClock_, bumped once per TickAllContexts frame) of
    /// the last frame this pane was drawn. The hidden-pane cap classifies "visible" by frame
    /// recency (drawn this/last frame) rather than wall-clock elapsed, so a genuinely-visible
    /// non-focused pane is never misclassified as hidden under a low frame rate (vsync/power
    /// throttle) — which would evict its snapshot and flicker/re-sync a pane the user is viewing.
    std::uint64_t lastVisibleFrame = 0;
    /// One-shot latch for AppController::EnsurePaneLiveSyncStarted (UI thread only):
    /// a non-focused visible pane's first sync is kicked exactly once per context
    /// generation (a retired-then-reshown pane gets a fresh context → fresh kick).
    /// Re-armed (set back to false) by GridContextDepsAdapter::OnStreamingSyncSessionFinished
    /// when the session ends with a fetch error, so the next focus switch / visibility kick
    /// retries instead of suppressing the re-sync forever (review MEDIUM-1).
    bool initialSyncKicked = false;
    /// Failure backoff for the initial-sync kick (issue #1081 sync-storm fix). Set to
    /// now + 30 s by GridContextDepsAdapter::OnStreamingSyncSessionFinished(fetchOk=false)
    /// alongside the latch re-arm above; AppController::EnsurePaneLiveSyncStarted bails while
    /// now < syncRetryAfter (see PaneSyncKickPolicy.h). Without it a fast-failing backend
    /// re-kicked a full sync EVERY FRAME (re-arm each failed session + per-frame kick site).
    /// UI thread only — same single-thread discipline as initialSyncKicked. Mirrors the
    /// projectComponentsRetryAfter_ 30 s pattern above.
    std::chrono::steady_clock::time_point syncRetryAfter{};
    /// JQL the most-recent kicked sync for this context used (UI thread only — written by
    /// the EnsurePaneLiveSyncStarted main-thread post and AppController::RecordPaneSyncKick,
    /// cleared by the session-end deps hook on fetch error; same single-thread discipline
    /// as initialSyncKicked). Compared against the adopted view's saved JQL on a pane focus
    /// switch: a drift (view edited after the context synced) re-kicks the sync instead of
    /// rendering stale rows (review MEDIUM-2).
    std::string lastSyncedJql;

    /// The pane's OWN ViewDefinition, resolved from ITS backend's views bucket by the
    /// first-sync worker (multi-grid Slice 4, cold-start frozen-capture hole). Published on
    /// the UI thread via the EnsurePaneLiveSyncStarted main-thread hop; read on the UI
    /// thread (same single-thread discipline as initialSyncKicked). A cross-backend pane
    /// whose view the focused ViewState bucket can't see builds its OWN columns from this
    /// instead of leaking the focused view's column set on cold start. Null until the
    /// pane's first sync resolves it. shared_ptr<const> so the type stays forward-declared
    /// here (full ViewDefinition is heavy — see ConfigManager.h).
    std::shared_ptr<const ViewDefinition> resolvedOwnView;

    // --- Ticket-change monitor anchors (ticket-change-monitor plan, S1c-2, item 8) --------
    // UI-thread-only bookkeeping for the per-pane change probe, same single-thread discipline
    // as syncRetryAfter / lastSyncedJql above (written/read only on the UI thread by
    // AppController::TickChangeMonitors and its main-thread apply hop).
    /// Earliest steady-clock instant the next change probe may be dispatched for this pane.
    /// Stamped to now + interval at dispatch (the in-flight guard — a probe in flight keeps
    /// this in the future so the per-frame tick does not re-dispatch) and re-advanced on apply.
    /// Consulted by smatchet::ShouldPollForChanges (PaneSyncKickPolicy.h).
    std::chrono::steady_clock::time_point nextChangePollAt{};
    /// Wall-clock "changed since" anchor for the backend query. Advanced to now after each
    /// successful poll; the probe asks for issues whose salient fields changed since roughly
    /// one interval before it (window margin absorbs minute-granularity / clock skew).
    std::chrono::system_clock::time_point changeSinceAnchor{};
    /// False until the first poll establishes the silent baseline (seeds changeSinceAnchor
    /// without fetching or toasting), so enabling the monitor on an already-populated pane
    /// does not replay the whole view as "changes".
    bool changeBaselineEstablished = false;

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
