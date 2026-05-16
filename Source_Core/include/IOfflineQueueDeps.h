#pragma once

// IOfflineQueueDeps — interface bundle exposing every AppController-side dependency that
// OfflineQueueService reaches into during replay, queue mutation, and audit wiring.
//
// Background: OfflineQueueService was extracted from AppController via `friend` access during
// the item 12 migration (see OfflineQueueService.h header comment). This interface removes the
// `friend` seam: AppController constructs `AppControllerDepsAdapter` (which implements this
// interface) and hands it to `OfflineQueueService`. The service no longer holds an
// `AppController&` reference and therefore no longer needs trusted-friendship access.
//
// Lifetime contract: the implementer (AppControllerDepsAdapter) is owned by AppController and
// outlives the OfflineQueueService it backs. Methods are called from both the UI thread and
// background tasks spawned via `LaunchBackgroundTask`; implementations are responsible for
// any synchronisation their backing state requires.
//
// Test fixtures implement this interface directly (see tests/support/FakeOfflineQueueDeps.h)
// so unit tests can exercise OfflineQueueService without constructing an AppController.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "IssueDraft.h"         // for RequiredFieldSet
#include "TrackerFieldSchema.h" // for TrackerField

class LocalCacheManager;
class ITrackerClient;

class IOfflineQueueDeps {
  public:
    virtual ~IOfflineQueueDeps() = default;

    /// Local SQLite cache. May be null before `Initialize` has wired the cache, or after
    /// `RecreateLocalCacheDatabase` has torn it down. Callers must null-check.
    virtual LocalCacheManager* Cache() = 0;

    /// Active tracker backend (Jira / Plane / fake). May be null in the same windows as
    /// `Cache()`. Replay tick guards on both before issuing live requests.
    virtual ITrackerClient* Backend() = 0;

    /// Catalog of tracker fields used by `TickOfflineCreates` to build `IssueCreatePipeline`
    /// input. Returned by const-ref so the caller can snapshot via `std::make_shared<...>`
    /// before spawning the background worker.
    virtual const std::vector<TrackerField>& AvailableFields() const = 0;

    /// Required-field set resolution. Used by `TickOfflineCreates` to validate the queued
    /// draft against per-project createmeta before replaying. Mirrors
    /// `AppController::GetRequiredFieldSet`.
    virtual RequiredFieldSet GetRequiredFieldSet(const std::string& projectKey,
                                                  const std::string& issueTypeId,
                                                  const std::string& issueTypeName) const = 0;

    /// Spawn a background worker. Mirrors `AppController::LaunchBackgroundTask` — the task
    /// must be joined by the host before its captured state is destroyed.
    virtual void LaunchBackgroundTask(std::function<void()> task) = 0;

    /// Reload `ActiveTickets` from the cache. Called from the replay tick after a successful
    /// create replay so the UI sees the new ticket.
    virtual void RefreshLocalData() = 0;

    /// Set the "post a `Live tracker backend OK` toast on the next UI frame" latch. Called
    /// after every successful replay so the user sees connectivity recovery without polling.
    virtual void RequestDeferredLiveTrackerBackendSuccessNotify() = 0;
};
