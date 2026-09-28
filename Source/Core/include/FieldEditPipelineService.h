#pragma once

// FieldEditPipelineService — owns the field-edit pipeline extracted from `AppController` per the
// AppController god-object decomposition plan (Phase 2), mirroring the Phase-1 EditMetaCacheService
// extraction. The service holds an `IFieldEditDeps&` (typically backed by `GridContextDepsAdapter`)
// for the AppController-side state it reaches (focused-pane fallback, per-pane tickets, optimistic
// update + refresh, deferred-notify and offline-queue hooks), and an `EditMetaCacheService&` directly
// (ctor-injected, not via deps) for the editmeta checks.
// Every field edit — grid cell, command / MCP, Lua, Annotate — commits through CommitOrQueue on a
// worker and is applied with ApplyFieldEditResult on the UI thread. Quality Pillar 6 (offline-first):
// CommitOrQueue is the one commit-or-queue seam, so an edit made while the tracker is unreachable
// reaches the offline queue without a network request.
// Pane binding (#2260): each request's Target names the pane the user acted in. The send, the
// editmeta checks, the queue row and the optimistic apply all use that pane's backend and cache key,
// never whichever pane holds focus when the worker runs.
// Concurrency: the service holds no long-lived mutex and no per-edit state between calls.
// Lifetime contract mirrors EditMetaCacheService: AppController owns the service via
// `std::unique_ptr` and outlives it; the `IFieldEditDeps&` (GridContextDepsAdapter) and the
// `EditMetaCacheService&` both outlive this service (declared after it, destroyed after it).

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "CachedTicketTypes.h"          // CachedTicket (CaptureTicketSnapshots)
#include "PendingActionTypes.h"         // PendingActionTarget (the pane an edit is bound to)
#include "SmatchetResult.h"             // VoidResult (ApplyFieldEditResult)
#include "Tracker/TrackerFieldSchema.h" // TrackerField (FieldEditCommitRequest holds one by value)
#include "Types/FieldEditTypes.h"       // FieldEditResult

class IFieldEditDeps;
class EditMetaCacheService;
class IssueTransitionsCacheService;
class ITrackerBackend;
class ITrackerIssueMutations;

/// One field edit to commit (or queue), captured where the user acted, with everything the worker needs.
struct FieldEditCommitRequest {
    std::string IssueId;
    TrackerField Field;
    std::vector<std::string> Values;
    /// Conflict bases persisted with a queued edit (ADR-0016): rich for ADF/HTML fields, scalar display otherwise.
    std::string OriginalRichValue;
    std::string OriginalValue;
    bool HasOriginalValue = false;
    std::string OriginalEstimateSnapshot;
    std::string RemainingEstimateSnapshot;
    std::string IssueTypeKeySnapshot;
    /// The pane the user edited, latched where they acted: the edit is sent to its backend, checked against
    /// that backend's editmeta and queued under its cache key even if focus moves first. Its Connectivity (the
    /// last probe when latched) decides queue-first vs network-first. An unbound request (empty PaneId: tests
    /// and tools) binds to the focused pane once, when CommitOrQueue starts.
    PendingActionTarget Target;
};

enum class FieldEditCommitKind : unsigned char { Failed, SavedOnline, QueuedOffline };

struct FieldEditCommitOutcome {
    FieldEditCommitKind Kind = FieldEditCommitKind::Failed;
    /// Saved or queued: the display values to apply locally. Failed: the network attempt's result.
    FieldEditResult Apply;
    std::int64_t QueueId = 0;
    /// Queued only after a network attempt failed with a retryable error (the tracker just dropped).
    bool QueuedAfterTransportFailure = false;
    std::string Error;
};

class FieldEditPipelineService {
  public:
    FieldEditPipelineService(IFieldEditDeps& deps, EditMetaCacheService& editMeta,
                             IssueTransitionsCacheService& transitions);

    /** Field families whose edits the offline queue can hold (queued first offline, or after a retryable failure). */
    static bool FieldEditSupportsOfflineQueue(const TrackerField& field);

    /// Fill `req`'s estimate and issue-type snapshots from the edited ticket as its pane shows it and, when
    /// `captureBase`, the conflict base (ADR-0016): the rich value of an ADF / HTML field, else the display.
    static void CaptureTicketSnapshots(const CachedTicket& ticket, bool captureBase, FieldEditCommitRequest& req);

    /// Worker-safe commit-or-queue seam (Quality Pillar 6). While the tracker is known to be offline
    /// (per `req.Target.Connectivity`) a queueable edit goes straight to the offline queue with no
    /// network request. Otherwise it tries the network and queues after a retryable failure. Queueing
    /// never fetches editmeta. The caller applies a Saved/Queued outcome with ApplyFieldEditResult.
    FieldEditCommitOutcome CommitOrQueue(const FieldEditCommitRequest& req);

    /// Apply a saved or queued edit's display values to `target`'s pane: its cache namespace and grid. UI
    /// thread. A no-op when that pane was retired or switched tracker since (its next sync shows the edit);
    /// an unbound target applies to the focused pane.
    VoidResult ApplyFieldEditResult(const PendingActionTarget& target, const std::string& issueId,
                                    const FieldEditResult& result);

  private:
    /// `target`, or the focused pane latched once for an unbound target (empty PaneId); the target's
    /// Connectivity is kept either way.
    PendingActionTarget BindTarget(const PendingActionTarget& target) const;

    /// CommitOrQueue body for an edit already bound to `target` (CommitOrQueue adds the audit pair).
    FieldEditCommitOutcome CommitOrQueueBound(const FieldEditCommitRequest& req, const PendingActionTarget& target);

    /// Network commit on `target`'s backend. The result carries Error + ErrorTransient on failure.
    FieldEditResult SubmitFieldEditNetworkOnly(const FieldEditCommitRequest& req, const PendingActionTarget& target);

    /// Build the payload and optimistic display map of a queued edit without any network request:
    /// no update and no editmeta fetch (the permission check uses whatever editmeta is loaded and is
    /// optimistic otherwise; replay gets the tracker's verdict).
    bool TryPrepareOfflineFieldEdit(const FieldEditCommitRequest& req, const PendingActionTarget& target,
                                    FieldEditResult& outResult, std::string& outFieldsPayloadJson,
                                    std::string& outError);

    /// Build a plain `fields` payload (checked against the editmeta already loaded) + optimistic display map
    /// on `backend`. Makes no network request.
    bool TryBuildFieldEditPayloadForNetwork(const FieldEditCommitRequest& req,
                                            const std::shared_ptr<ITrackerBackend>& backend,
                                            nlohmann::json& outFieldsPayload,
                                            std::unordered_map<std::string, std::string>& outDisplayValues,
                                            std::string& outError);

    /// CommitOrQueue helper — prepare the payload (no network) and persist it to the offline queue under
    /// `target`'s cache key through IFieldEditDeps::EnqueueOfflineFieldEdit.
    FieldEditCommitOutcome QueuePreparedEdit(const FieldEditCommitRequest& req, const PendingActionTarget& target,
                                             bool afterTransportFailure);

    /// SubmitFieldEditNetworkOnly helper — push the built payload, retrying once after a 400 with
    /// a refreshed editmeta + edit-permission re-check. Returns true on a successful update.
    bool ApplyFieldUpdateWithEditMetaRetry(const FieldEditCommitRequest& req, const PendingActionTarget& target,
                                           const nlohmann::json& fieldsPayload, ITrackerIssueMutations& mutations,
                                           FieldEditResult& outResult);

    /// SubmitFieldEditNetworkOnly helper — add the issue to a sprint (agile API) + optimistic display value.
    bool SubmitSprintFieldEditNetworkOnly(const FieldEditCommitRequest& req, const std::vector<std::string>& values,
                                          ITrackerIssueMutations& mutations, FieldEditResult& outResult);

    /// SubmitFieldEditNetworkOnly helper — update one Jira time-tracking estimate.
    bool SubmitTimetrackingFieldEditNetworkOnly(const FieldEditCommitRequest& req,
                                                const std::vector<std::string>& values,
                                                ITrackerIssueMutations& mutations, FieldEditResult& outResult);

    IFieldEditDeps& deps_;
    EditMetaCacheService& editMeta_;
    IssueTransitionsCacheService& transitions_;
};
