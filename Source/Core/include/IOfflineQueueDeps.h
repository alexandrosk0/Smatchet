#pragma once

// IOfflineQueueDeps — interface bundle exposing every AppController-side dependency that
// OfflineQueueService reaches into during replay, queue mutation, and audit wiring.
// Background: OfflineQueueService was extracted from AppController via `friend` access during
// the item 12 migration (see OfflineQueueService.h header comment). This interface removes the
// `friend` seam: AppController constructs `GridContextDepsAdapter` (which implements this
// interface) and hands it to `OfflineQueueService`. The service no longer holds an
// `AppController&` reference and therefore no longer needs trusted-friendship access.
// Lifetime contract: the implementer (GridContextDepsAdapter) is owned by AppController and
// outlives the OfflineQueueService it backs. Methods are called from both the UI thread and
// background tasks spawned via `LaunchBackgroundTask`; implementations are responsible for
// any synchronisation their backing state requires.
// Test fixtures implement this interface directly (see tests/support/FakeOfflineQueueDeps.h)
// so unit tests can exercise OfflineQueueService without constructing an AppController.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "IssueDraft.h"         // for RequiredFieldSet
#include "TrackerFieldSchema.h" // for TrackerField

class ISyncCache;
class ITrackerIssueReader;
class ITrackerIssueMutations;

class IOfflineQueueDeps {
  public:
    virtual ~IOfflineQueueDeps() = default;

    /// Sync/replay-facing local-cache surface (ADR-0020 — `LocalCacheManager` in production;
    /// `FakeSyncCache` in tests). May be null before `Initialize` has wired the cache,
    /// or after `RecreateLocalCacheDatabase` has torn it down. Callers must null-check.
    virtual ISyncCache* Cache() = 0;

    /// DR6 latched cache snapshot — a strong handle over the same cache the raw Cache() points
    /// at, so an off-thread caller (QueueCreateOffline runs on a worker) keeps the cache alive
    /// across its whole operation while the UI thread swaps it in RecreateLocalCacheDatabase.
    /// Mirrors the ADR-0012 Backend atomic-swap / ReaderShared latched-handle pattern. May be
    /// null (no cache yet). Capture the shared_ptr itself and use it for every deref.
    virtual std::shared_ptr<ISyncCache> CacheShared() = 0;

    /// LATCHED narrow read accessor — a strong handle that keeps the owning backend alive
    /// (production: aliasing shared_ptr onto the atomically-latched backend) so replay
    /// workers that captured it survive a live tracker swap or a Slice-3 pane-context
    /// retirement (debt 2026-06-07 — the raw-subobject-across-async use-after-free class).
    /// May be null (no backend yet). Capture the shared_ptr itself into the worker.
    virtual std::shared_ptr<ITrackerIssueReader> ReaderShared() const = 0;

    /// LATCHED narrow mutation accessor — same lifetime contract as ReaderShared(). Null
    /// when there is no backend or the backend's Mutations() is unsupported.
    virtual std::shared_ptr<ITrackerIssueMutations> MutationsShared() const = 0;

    /// Backend-key namespace for LocalCacheManager ticket reads/writes done by replay
    /// (`IssueCreatePipeline::Run` seeds the cache after a successful create). Returns a copy —
    /// callable from the replay background task. Shared (same override) with `ITicketSyncDeps`.
    /// Queue-row namespacing itself is Slice 1c — this getter only scopes the ticket tables.
    virtual std::string CacheBackendKey() const = 0;

    /// Backend-generation token (issue #1081): bumped on every live backend swap and on
    /// pane-context retirement (see GridLiveContext::backendGeneration_). Replay ticks capture
    /// the value at work-capture time (alongside `CacheBackendKey`) and drop stale applies —
    /// e.g. the post-replay `RefreshLocalData` — when the generation moved mid-flight, so a
    /// completed replay against the OLD backend never wholesale-replaces the NEW backend's
    /// ActiveTickets. Callable from worker threads (atomic load).
    virtual std::uint64_t BackendGeneration() const = 0;

    /// Catalog of tracker fields used by `TickOfflineCreates` to build `IssueCreatePipeline`
    /// input. Returned by const-ref so the caller can snapshot via `std::make_shared<...>`
    /// before spawning the background worker.
    virtual const std::vector<TrackerField>& AvailableFields() const = 0;

    /// Required-field set resolution. Used by `TickOfflineCreates` to validate the queued
    /// draft against per-project createmeta before replaying. Mirrors
    /// `AppController::GetRequiredFieldSet`.
    virtual RequiredFieldSet GetRequiredFieldSet(const std::string& projectKey, const std::string& issueTypeId,
                                                 const std::string& issueTypeName) const = 0;

    /// Spawn a background worker. Mirrors `AppController::LaunchBackgroundTask` — the task
    /// must be joined by the host before its captured state is destroyed.
    virtual void LaunchBackgroundTask(std::function<void()> task) = 0;

    /// Reload `ActiveTickets` from the cache. Called from the replay tick after a successful
    /// create replay so the UI sees the new ticket.
    virtual void RefreshLocalData() = 0;

    /// Generation-checked variant (issue #1081) for the post-replay refresh: the implementer
    /// re-checks `capturedBackendGeneration` against this deps object's OWN context under
    /// `activeTicketsMutex_` immediately before the wholesale ActiveTickets replace, and drops
    /// the replace on mismatch. Closes the TOCTOU left open by a worker-side
    /// `BackendGeneration()` pre-check alone — the slow full-table cache read sits between
    /// that pre-check and the locked replace, and a swap landing mid-read would otherwise
    /// wholesale-replace the NEW backend's ActiveTickets with OLD-key rows. The pre-check
    /// stays useful as a cheap skip of the cache read; this overload is the authoritative gate.
    virtual void RefreshLocalData(std::uint64_t capturedBackendGeneration) = 0;

    /// Set the "post a `Live tracker backend OK` toast on the next UI frame" latch. Called
    /// after every successful replay so the user sees connectivity recovery without polling.
    virtual void RequestDeferredLiveTrackerBackendSuccessNotify() = 0;
};
