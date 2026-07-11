#pragma once

// OfflineQueueService — owns the offline-create / offline-field-edit replay queues and the
// dead-letter management around them. Extracted from `AppController_IssueCreateOffline.cpp`
// per BACKLOG_CODE_REVIEW.md §1.7 / §7 item 12.
// Phase 1A scope (this PR): class skeleton, owned by AppController as a `unique_ptr` member,
// holds the `legacyPendingStartupBanner_` state and a small first batch of read-only
// accessors. AppController's public surface is preserved via thin delegators.
// Future phases migrate the remaining methods cluster-by-cluster:
//   1B — write methods (`QueueCreateOffline`, `QueueFieldEditOffline`, `Restore*`, `Delete*`).
//   1C — `Tick*` replay loops + their internal helpers.
//   1D — field-edit equivalents (`ResolveFieldEditConflict`, `Tick…FieldEdits`).
// Phase 2 (this PR) replaces the `friend class OfflineQueueService;` access with a small
// `IOfflineQueueDeps` interface bundle (see IOfflineQueueDeps.h). AppController hands a
// concrete `GridContextDepsAdapter` to the service; tests substitute `FakeOfflineQueueDeps`
// so they can exercise the service without constructing an AppController.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// The summary structs that several methods below return (`DeadLetterRestoreSummary`,
// `DeadLetterDeleteSummary`, …) live in this leaf header (top-level / global namespace).
// Relocated out of AppController.h so this service no longer drags in the orchestrator
// (core-include-dag Phase 3 — severs the Sync -> AppController include back-edge).
#include "Sync/OfflineQueueTypes.h"

namespace OfflineFieldEditMergeDetail {
/// True when `rich` (after leading whitespace) begins with '{' — i.e. an ADF JSON document
/// rather than HTML/markdown. Pure; unit-tested in OfflineFieldEditMerge.test.cpp.
bool DetectRichKindIsAdf(const std::string& rich);

/// Convert a stored rich value (ADF JSON `doc`, HTML subset, or plain markdown) to markdown.
/// Falls back to returning the input unchanged when it is neither ADF nor HTML. Pure.
std::string RichToMarkdown(const std::string& rich);

/// Convert a queued field-payload value ("mine") to markdown: ADF `doc` objects and HTML/
/// markdown strings are both handled; any other JSON type yields an empty string. Pure.
std::string MineValueToMarkdown(const nlohmann::json& myVal);
} // namespace OfflineFieldEditMergeDetail

class IOfflineQueueDeps;
class ISyncCache;
class ITrackerIssueReader;
class ITrackerIssueMutations;
struct TrackerError; // by-const-ref only (HandleFieldEditUpdateFailure); full def in Tracker/TrackerError.h
namespace TextMerge {
struct MergeResult;
}
struct TrackerField;
struct CachedTicket;
struct IssueDraft; // formerly via AppController.h; used only by const-ref here (core-include-dag Phase 3)
struct PendingCreate;
struct DeadPendingCreate;
struct PendingFieldEditRecord;
struct DeadPendingFieldEdit;

/// Lifetime contract: AppController owns the service via `std::unique_ptr` and outlives it.
/// The constructor stores an `IOfflineQueueDeps&` reference (typically backed by
/// `GridContextDepsAdapter`); methods reach AppController-side state (`Cache`, `Backend`,
/// audit trail, mutexes) through that interface. Tests pass a `FakeOfflineQueueDeps` so the
/// service is exercisable in pure-logic doctest builds.
class OfflineQueueService {
  public:
    explicit OfflineQueueService(IOfflineQueueDeps& deps);

    // --- Phase 1A: trivial read-only accessors -------------------------------------------
    std::size_t GetPendingCreateCount() const;
    std::size_t GetDeadPendingCreateCount() const;
    std::vector<PendingCreate> GetPendingCreates() const;
    std::vector<DeadPendingCreate> GetDeadPendingCreates() const;

    /// One-shot getter: returns and clears the legacy-pending-startup banner string. AppController
    /// sets `legacyPendingStartupBanner_` during `Initialize` when it detects legacy queue rows
    /// from a pre-split schema; the UI reads it once on first frame and shows a toast.
    std::string TakeLegacyPendingStartupBanner();

    // --- Phase 1B: write methods + remaining field-edit read accessors -------------------
    /// Persist an offline create row. Returns the new SQLite row id, or 0 on read-only / failure.
    std::int64_t QueueCreateOffline(const IssueDraft& draft);

    /// Move selected dead-letter rows back to the active offline queue (attempts reset to 0).
    /// Each row keeps its ORIGINAL `backend_key` (multi-grid Slice 1c) — never the focused
    /// context's key — and is re-queued as a fresh create (`ExistingIssueKey` + issuekey/key
    /// field values scrubbed, the dead-letter restore UI contract; the scrub itself runs
    /// inside `LocalCacheManager::RestoreDeadPendingCreate`'s transaction, CR-959).
    ::DeadLetterRestoreSummary RestoreDeadPendingCreates(const std::vector<std::int64_t>& originalIds);

    /// Field-edit twin of `RestoreDeadPendingCreates`: move selected dead-letter field-edit
    /// rows back to the active queue (attempts reset to 0), each keeping its ORIGINAL
    /// `backend_key` and both merge bases.
    ::DeadFieldEditRestoreSummary RestoreDeadPendingFieldEdits(const std::vector<std::int64_t>& originalIds);

    /// Permanently remove dead-letter rows by `pending_creates_dead.dead_id`.
    ::DeadLetterDeleteSummary DeleteDeadPendingCreates(const std::vector<std::int64_t>& deadIds);

    /// Permanently remove active offline-queue rows by `pending_creates.id`.
    ::PendingQueueDeleteSummary DeletePendingCreates(const std::vector<std::int64_t>& pendingIds);

    /// Persist a tracker field payload for later replay when connectivity returns.
    /// `originalRichValue` is the rich (ADF/HTML) base for the 3-way merge; `originalValue` is
    /// the scalar display base for the 2-way `ServerMovedFromBase` compare (ADR-0016). Both
    /// default empty; a row with neither base keeps last-write-wins. `hasOriginalValue` records
    /// whether the scalar base was CAPTURED (presence) independent of its emptiness, so a blank
    /// captured base is still conflict-checked rather than mistaken for a legacy no-base row.
    std::int64_t QueueFieldEditOffline(const std::string& issueKey, const std::string& fieldId,
                                       const std::string& fieldsPayloadJson, std::string& outError,
                                       const std::string& originalRichValue,
                                       const std::string& originalValue = std::string(), bool hasOriginalValue = false);

    std::vector<PendingFieldEditRecord> GetPendingFieldEdits() const;
    std::vector<DeadPendingFieldEdit> GetDeadPendingFieldEdits() const;

    /// Replace the queued payload with a user-resolved version and clear the conflict flag.
    /// The edit will be retried on the next TickOfflineFieldEdits pass. `kind` (text|scalar|
    /// unverified, ADR-0016) selects how the resolution is applied: `text` reconverts
    /// `resolvedValue` Markdown→ADF/HTML via `richKind` into the payload key; `scalar` rebuilds
    /// the payload via the production `BuildFieldPayload` builder so a structured field keeps its
    /// `{"id":...}` / array shape (a bare display string dead-letters with HTTP 400 — #854; string
    /// fields come back bare, verbatim write is the no-schema fallback); `unverified` ("Force
    /// Mine") replays the queued payload unchanged and only clears the conflict state + bases.
    void ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedValue, const std::string& richKind,
                                  const std::string& kind = std::string("text"));

    ::PendingFieldEditDeleteSummary DeletePendingFieldEdits(const std::vector<std::int64_t>& ids);

    ::DeadFieldEditDeleteSummary DeleteDeadPendingFieldEdits(const std::vector<std::int64_t>& deadIds);

    // --- Phase 1C: replay loops + replay-timer accessors ---------------------------------
    /// Replay queued offline creates. Called from the UI tick. Rate-limited internally via
    /// `nextOfflineReplayAt_` and guarded by `offlineReplayInFlight_` so concurrent ticks
    /// (or re-entry from worker callbacks) become no-ops.
    void TickOfflineCreates();

    /// Replay queued offline field edits. Same rate-limit + in-flight guard as
    /// `TickOfflineCreates`; uses its own pair of state variables.
    void TickOfflineFieldEdits();

    /// Push both replay timers (creates + field-edits) forward to at least `pushTo`. Called
    /// from `AppController::PushOfflineReplayTimersDuringTransportOutage` when a tracker
    /// connectivity probe reports the transport is down — there's no point retrying replay
    /// against an unreachable server. Existing schedule is preserved via `max(current, pushTo)`.
    void PushReplayTimersForward(std::chrono::steady_clock::time_point pushTo);

    /// Reset both replay timers to `now`, scheduling immediate replay on the next tick.
    /// Called from `AppController::ConsumeTrackerConnectivityRecovery` when transport
    /// reachability is regained.
    void RestartReplayTimersNow(std::chrono::steady_clock::time_point now);

    /// Set by AppController during legacy migration on first launch with a pre-split cache schema.
    /// Reset to empty by `TakeLegacyPendingStartupBanner`. Read/write only on the UI thread.
    std::string legacyPendingStartupBanner_;

    /// remove-global-project-key.md: one-shot startup sweep that fills in
    /// `IssueDraft::ProjectKey` on legacy `pending_creates` rows whose draft was authored
    /// against the now-removed global project. Recovery order per row:
    ///   1. parent key prefix from `draft.ExistingIssueKey` (Jira `PROJ-123` -> `PROJ`)
    ///   2. legacy global captured during config load (Jira or Plane per `trackerType`)
    ///   3. dead-letter with terminal_reason `legacy_missing_project`
    /// Guarded by the `cache_meta` flag `legacy_project_swept_v1`; subsequent calls return 0
    /// without scanning. Logged at INFO with a per-bucket summary.
    void RunLegacyProjectSweep(const std::string& legacyJiraProjectKey, const std::string& legacyPlaneProjectId,
                               const std::string& trackerType);

  private:
    // --- TickOfflineFieldEdits replay helpers ---------------------------------------------
    // TickOfflineFieldEdits dispatches a background task whose body is a thin loop over these
    // per-row helpers. The helpers run off the UI thread (inside the launched task) and operate
    // on stack-local state passed by reference — no extra heap allocation or container copy is
    // introduced versus the original inline loop.

    /// Running counters for one replay pass. Stack-allocated in the background task; threaded
    /// through the per-row helpers by reference.
    struct FieldEditReplayTally {
        int Successes = 0;
        int Failures = 0;
        int Archived = 0;
        int CacheOpFailures = 0;
        bool RanUpdate = false;
    };

    /// Run a cache mutation under a uniform try/catch, counting failures into `tally`. Returns
    /// true on success. Forwarding-by-value of the callable is avoided: it is taken by const ref.
    bool RunFieldEditCacheMutation(const char* action, std::int64_t id, const std::function<void()>& fn,
                                   FieldEditReplayTally& tally);

    /// Outcome of the pre-replay conflict gate (ADR-0016). `Proceed` = no divergence (or no base
    /// captured) → replay the queued payload as usual. `Suspend` = a conflict (rich merge, scalar
    /// divergence, or unverifiable re-fetch) was recorded and the row must be skipped this pass.
    /// `RetryTransient` = a transient re-fetch failure for a based row that has not yet hit the
    /// attempt cap → bump attempts and try again next tick (never a silent replay).
    enum class FieldEditConflictOutcome { Proceed, Suspend, RetryTransient };

    /// Pre-replay conflict gate. Dispatches rich (3-way text merge) vs scalar (2-way display
    /// compare) based on which base the row captured, performing the single server re-fetch and
    /// applying the decision-(c) fetch-failure routing uniformly for both kinds.
    FieldEditConflictOutcome EvaluateFieldEditConflict(const PendingFieldEditRecord& row, nlohmann::json& fieldsPayload,
                                                       ISyncCache* cache, ITrackerIssueReader* reader,
                                                       FieldEditReplayTally& tally);

    /// Look up a tracker field by id in the live catalog (`deps_.AvailableFields()`). Returns null
    /// when the field is unknown (legacy/retired field or empty catalog). Used by scalar
    /// conflict-resolution (#854) to rebuild the structured payload via the production builder.
    const TrackerField* FindCatalogField(const std::string& fieldId) const;

    /// Record a `kind:"unverified"` conflict (server value couldn't be read) and suspend the row.
    /// Context: `{kind:"unverified", mine, fieldId}`. Always returns Suspend.
    FieldEditConflictOutcome RecordUnverifiedFieldEditConflict(const PendingFieldEditRecord& row,
                                                               const nlohmann::json& fieldsPayload, ISyncCache* cache,
                                                               FieldEditReplayTally& tally);

    /// Scalar 2-way conflict gate: compare the captured display base against the re-fetched
    /// display value via `ServerMovedFromBase`. On divergence records a `kind:"scalar"` conflict
    /// and returns Suspend; otherwise Proceed.
    FieldEditConflictOutcome ResolveFieldEditScalarConflict(const PendingFieldEditRecord& row,
                                                            const nlohmann::json& fieldsPayload,
                                                            const CachedTicket& fresh, ISyncCache* cache,
                                                            FieldEditReplayTally& tally);

    /// 3-way merge gate: merge the captured rich base against the re-fetched server document.
    /// Mutates `fieldsPayload` in place on a clean merge. Returns Suspend when a conflict was
    /// recorded, Proceed otherwise.
    FieldEditConflictOutcome ResolveFieldEditThreeWayMerge(const PendingFieldEditRecord& row,
                                                           nlohmann::json& fieldsPayload, const CachedTicket& fresh,
                                                           ISyncCache* cache, FieldEditReplayTally& tally);

    /// Apply a computed 3-way merge result: on a clean merge, rewrite `fieldsPayload[payloadKey]`
    /// to the merged ADF/HTML; on conflict, record the conflict context to the cache and return
    /// true so the caller suspends replay of this row.
    bool ApplyOrRecordMergeResult(const PendingFieldEditRecord& row, nlohmann::json& fieldsPayload,
                                  const std::string& payloadKey, bool isAdf, const TextMerge::MergeResult& merged,
                                  const std::string& baseMd, const std::string& mineMd, const std::string& theirsMd,
                                  ISyncCache* cache, FieldEditReplayTally& tally);

    /// Replay a single queued field edit: attempt-cap gate, payload parse, 3-way merge, the
    /// tracker mutation, and the success / transport-retry / rejection / archive bookkeeping.
    void ReplayOneFieldEdit(const PendingFieldEditRecord& row, ISyncCache* cache, ITrackerIssueReader* reader,
                            ITrackerIssueMutations* mutations, IOfflineQueueDeps& depsRef, FieldEditReplayTally& tally);

    /// Handle a failed `UpdateIssueFields` for a replayed field edit: transport errors bump the
    /// attempt count (archiving at the cap), non-transport errors archive as `replay_rejected`.
    void HandleFieldEditUpdateFailure(const PendingFieldEditRecord& row, ISyncCache* cache,
                                      const TrackerError& updateErr, FieldEditReplayTally& tally);

    // --- TickOfflineCreates replay helpers ------------------------------------------------
    // TickOfflineCreates dispatches a background task whose body is a thin loop over a per-row
    // helper. The helper runs off the UI thread (inside the launched task) and operates on
    // stack-local state passed by reference — no extra heap allocation or container copy is
    // introduced versus the original inline loop. Mirrors the TickOfflineFieldEdits shape.

    /// Running counters for one create-replay pass. Stack-allocated in the background task;
    /// threaded through the per-row helper by reference.
    struct CreateReplayTally {
        int Successes = 0;
        int Failures = 0;
        int Archived = 0;
        int CacheOpFailures = 0;
        bool RanCreate = false;
    };

    /// Run a cache mutation under a uniform try/catch, counting failures into `tally`. Returns
    /// true on success. The callable is taken by const ref (no forwarding-by-value copy).
    bool RunCreateCacheMutation(const char* action, std::int64_t id, const std::function<void()>& fn,
                                CreateReplayTally& tally);

    /// Replay a single queued offline create: attempt-cap gate, payload parse, the create
    /// pipeline run, and the success / retry / archive bookkeeping. Runs off the UI thread.
    /// `backendKey` is the cache namespace CAPTURED at work-capture time (issue #1081) — the
    /// key the rows were queued + filtered against, never a write-time deps re-read.
    void ReplayOneCreate(const PendingCreate& pc, ISyncCache* cache, ITrackerIssueMutations* mutations,
                         const std::vector<TrackerField>& catalog, const std::string& backendKey,
                         IOfflineQueueDeps& depsRef, CreateReplayTally& tally);

    // --- RunLegacyProjectSweep per-row helper ---------------------------------------------
    /// Running counters for one legacy-project sweep pass.
    struct LegacyProjectSweepTally {
        int Recovered = 0;
        int DeadLettered = 0;
        int Untouched = 0;
    };

    /// Sweep a single pending-create row: recover its project key (parent prefix or legacy
    /// fallback), or dead-letter it. Counts the outcome into `tally`.
    void SweepOneLegacyPendingCreate(const PendingCreate& pc, const std::string& legacyForBackend,
                                     LegacyProjectSweepTally& tally);

    IOfflineQueueDeps& deps_;

    // Offline-replay throttle + in-flight guards. Moved here from AppController in Phase 1C.
    // All accesses go through `offlineReplayScheduleMutex_`. UI thread sets the schedule
    // (via the public methods above); the background worker spawned by `TickOffline*` reads
    // the in-flight flags + updates `next*ReplayAt_` when it finishes.
    mutable std::mutex offlineReplayScheduleMutex_;
    std::chrono::steady_clock::time_point nextOfflineReplayAt_ = std::chrono::steady_clock::now();
    bool offlineReplayInFlight_ = false;
    std::chrono::steady_clock::time_point nextOfflineFieldEditReplayAt_ = std::chrono::steady_clock::now();
    bool offlineFieldEditReplayInFlight_ = false;
};
