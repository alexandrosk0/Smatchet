#include "Sync/PendingActionQueueService.h"

#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "IOfflineQueueDeps.h"
#include "ISyncCache.h"
#include "ITrackerCollaboration.h"
#include "Logger.h"
#include "OfflineFirstPure.h"
#include "OfflineQueueReplayPolicy.h"
#include "PendingActionPolicyPure.h"
#include "ScopeExit.h"
#include "Tracker/TrackerError.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <utility>

namespace {

using smatchet::ScopeExit;
namespace pendingaction = smatchet::pendingaction;

// The field-edit queue's cadence: the next pass 5 s after one that made progress (or had nothing
// left to try), 30 s after one where every attempt failed or that could not run at all.
constexpr std::chrono::seconds kReplayDelay{5};
constexpr std::chrono::seconds kReplayBackoff{30};

bool IsReplayable(const PendingActionRecord& row, const std::string& backendKey) {
    return row.BackendKey == backendKey && row.State != PendingActionState::kNeedsReview;
}

nlohmann::json AuditExtra(const PendingActionRecord& row) {
    return nlohmann::json{{"pending_action_id", row.Id}, {"kind", row.Kind}};
}

// The one place an action reaches the tracker, for both live sends and replay.
TrackerError Dispatch(ITrackerCollaboration& collab, const TrackerConfig& cfg, PendingActionKind kind,
                      const std::string& issueKey, const std::string& payloadJson) {
    switch (kind) {
    case PendingActionKind::CommentAdd: {
        std::string body;
        std::int64_t queuedAt = 0;
        if (!pendingaction::ParseCommentActionPayload(payloadJson, body, queuedAt)) {
            return TrackerErrorInvalidRequest("The saved comment could not be read.");
        }
        return collab.AddIssueCommentPlain(cfg, issueKey, body);
    }
    case PendingActionKind::WorklogAdd: {
        pendingaction::WorklogActionPayload worklog;
        if (!pendingaction::ParseWorklogActionPayload(payloadJson, worklog)) {
            return TrackerErrorInvalidRequest("The saved worklog could not be read.");
        }
        return collab.AddWorklog(cfg, issueKey, worklog.TimeSpent, worklog.TimeRemaining, worklog.AdjustEstimate,
                                 worklog.Description, worklog.Started);
    }
    case PendingActionKind::WatchAdd:
        return collab.AddIssueWatcher(cfg, issueKey);
    }
    // Every kind returns above; this only satisfies compilers that cannot prove the switch exhaustive.
    return TrackerErrorInvalidRequest("Unknown action kind.");
}

} // namespace

PendingActionQueueService::PendingActionQueueService(IOfflineQueueDeps& deps)
    : deps_(deps), snapshot_(std::make_shared<const PendingActionsSnapshot>()) {}

std::shared_ptr<const PendingActionsSnapshot> PendingActionQueueService::Snapshot() const {
    return std::atomic_load(&snapshot_);
}

PendingActionQueueService::SubmitOutcome
PendingActionQueueService::SubmitOrQueue(PendingActionKind kind, const std::string& issueKey,
                                         const std::string& payloadJson, TrackerConnectivityState connectivityAtKick) {
    SubmitOutcome out;
    if (issueKey.empty() || payloadJson.empty()) {
        out.Error = "Nothing to send (the action is incomplete).";
        return out;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    switch (smatchet::offline::RouteWrite(connectivityAtKick, /*queueSupported=*/true, cfg.ReadOnlyMode)) {
    case smatchet::offline::WriteRoute::Reject:
        out.Error = "Read-only mode is enabled in Preferences.";
        return out;
    case smatchet::offline::WriteRoute::QueueImmediately:
        return Enqueue(kind, issueKey, payloadJson, PendingActionState::kPending);
    case smatchet::offline::WriteRoute::NetworkFirst:
        break;
    }
    const std::shared_ptr<ITrackerCollaboration> collab = deps_.CollaborationShared();
    if (!collab) {
        out.Error = "Tracker backend does not support collaboration features.";
        return out;
    }
    const TrackerError err = Dispatch(*collab, cfg, kind, issueKey, payloadJson);
    if (err.IsOk()) {
        out.K = SubmitOutcome::Kind::Sent;
        deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
        return out;
    }
    const char* state = pendingaction::StateAfterFailedSend(kind, err);
    if (state[0] == '\0') {
        LOG_WARN("PendingActionQueueService: %s on %s rejected: %s", PendingActionKindWire(kind), issueKey.c_str(),
                 err.Detail.c_str());
        out.Error = err.Detail.empty() ? std::string("The tracker rejected the action.") : err.Detail;
        return out;
    }
    out = Enqueue(kind, issueKey, payloadJson, state);
    out.QueuedAfterNetworkFailure = out.K == SubmitOutcome::Kind::Queued;
    if (out.K == SubmitOutcome::Kind::Failed) {
        out.Error = err.Detail + " " + out.Error;
    }
    return out;
}

PendingActionQueueService::SubmitOutcome PendingActionQueueService::Enqueue(PendingActionKind kind,
                                                                            const std::string& issueKey,
                                                                            const std::string& payloadJson,
                                                                            const char* state) {
    SubmitOutcome out;
    // Latched once: the UI thread may swap the cache (RecreateLocalCacheDatabase) mid-enqueue.
    const std::shared_ptr<ISyncCache> cache = deps_.CacheShared();
    if (!cache) {
        out.Error = "The local cache is unavailable, so this could not be saved offline.";
        return out;
    }
    const char* wire = PendingActionKindWire(kind);
    try {
        out.QueueId = cache->EnqueuePendingAction(deps_.CacheBackendKey(), wire, issueKey, payloadJson, state);
        out.K = SubmitOutcome::Kind::Queued;
    } catch (const std::exception& ex) {
        out.Error = "Saving it to the offline queue failed (local database error).";
        LOG_ERROR("PendingActionQueueService: enqueue %s on %s failed: %s", wire, issueKey.c_str(), ex.what());
        BackendAuditTrail::AppendResult("offline_queue_action", "ui", issueKey,
                                        BackendAuditTrail::MakeOperationId("offline-action-queue"), false, ex.what(),
                                        nlohmann::json{{"kind", wire}});
        return out;
    }
    LOG_INFO("PendingActionQueueService: queued %s id=%lld issue=%s state=%s", wire,
             static_cast<long long>(out.QueueId), issueKey.c_str(), state);
    BackendAuditTrail::AppendResult(
        "offline_queue_action", "ui", issueKey, std::to_string(out.QueueId), true, std::string(),
        nlohmann::json{{"pending_action_id", out.QueueId}, {"kind", wire}, {"state", state}});
    PublishSnapshot(*cache);
    return out;
}

void PendingActionQueueService::Tick() {
    const std::shared_ptr<ISyncCache> cache = deps_.CacheShared();
    if (!cache) {
        return;
    }
    if (!snapshotLoaded_.load()) {
        LoadSnapshotAsync(cache);
        if (!snapshotLoaded_.load()) {
            return;
        }
    }
    // Pillar 2: decide from the in-memory snapshot — no worker, no SQLite, while nothing is queued.
    const std::string backendKey = deps_.CacheBackendKey();
    const std::shared_ptr<const PendingActionsSnapshot> snap = Snapshot();
    if (backendKey.empty() ||
        std::none_of(snap->Pending.begin(), snap->Pending.end(),
                     [&backendKey](const PendingActionRecord& row) { return IsReplayable(row, backendKey); })) {
        return;
    }
    if (ConfigManager::Load().ReadOnlyMode) {
        return;
    }
    const std::shared_ptr<ITrackerCollaboration> collab = deps_.CollaborationShared();
    if (!collab) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(scheduleMutex_);
        if (replayInFlight_ || std::chrono::steady_clock::now() < nextReplayAt_) {
            return;
        }
        replayInFlight_ = true;
    }
    try {
        deps_.LaunchBackgroundTask([this, cache, collab, backendKey]() {
            std::chrono::seconds nextDelay = kReplayBackoff;
            // Clears the in-flight latch on every exit, a throw included, so replay can never stop.
            ScopeExit endPass([this, &nextDelay]() {
                const auto nextAt = std::chrono::steady_clock::now() + nextDelay;
                std::lock_guard<std::mutex> lock(scheduleMutex_);
                nextReplayAt_ = nextAt;
                replayInFlight_ = false;
            });
            nextDelay = RunReplayPass(*cache, *collab, backendKey);
        });
    } catch (const std::exception& ex) {
        LOG_WARN("PendingActionQueueService: could not start a replay pass: %s", ex.what());
        std::lock_guard<std::mutex> lock(scheduleMutex_);
        replayInFlight_ = false;
        nextReplayAt_ = std::chrono::steady_clock::now() + kReplayBackoff;
    }
}

void PendingActionQueueService::LoadSnapshotAsync(const std::shared_ptr<ISyncCache>& cache) {
    if (snapshotLoadInFlight_.exchange(true)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(scheduleMutex_);
        const auto now = std::chrono::steady_clock::now();
        if (now < nextSnapshotLoadAt_) {
            snapshotLoadInFlight_.store(false);
            return;
        }
        nextSnapshotLoadAt_ = now + kReplayBackoff; // a failed load is retried after the backoff
    }
    try {
        deps_.LaunchBackgroundTask([this, cache]() {
            ScopeExit done([this]() { snapshotLoadInFlight_.store(false); });
            PublishSnapshot(*cache);
        });
    } catch (const std::exception& ex) {
        LOG_WARN("PendingActionQueueService: could not start loading the queue: %s", ex.what());
        snapshotLoadInFlight_.store(false);
    }
}

void PendingActionQueueService::RequestSnapshotRefresh() {
    {
        std::lock_guard<std::mutex> lock(scheduleMutex_);
        nextSnapshotLoadAt_ = std::chrono::steady_clock::time_point();
    }
    snapshotLoaded_.store(false);
}

void PendingActionQueueService::PushReplayTimersForward(std::chrono::steady_clock::time_point pushTo) {
    std::lock_guard<std::mutex> lock(scheduleMutex_);
    nextReplayAt_ = (std::max)(nextReplayAt_, pushTo);
}

void PendingActionQueueService::RestartReplayTimersNow(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(scheduleMutex_);
    nextReplayAt_ = now;
}

std::chrono::seconds PendingActionQueueService::RunReplayPass(ISyncCache& cache, ITrackerCollaboration& collab,
                                                              const std::string& backendKey) {
    std::vector<PendingActionRecord> rows;
    try {
        rows = cache.LoadPendingActions();
    } catch (const std::exception& ex) {
        LOG_ERROR("PendingActionQueueService: loading the queue for replay failed: %s", ex.what());
        return kReplayBackoff;
    }
    const TrackerConfig cfg = ConfigManager::Load();
    ReplayTally tally;
    for (const PendingActionRecord& row : rows) {
        if (!IsReplayable(row, backendKey)) {
            continue;
        }
        ReplayOne(cache, collab, cfg, row, tally);
        if (tally.TransportDown) {
            break; // the tracker is unreachable: leave the rest untouched instead of spending their attempts
        }
    }
    PublishSnapshot(cache);
    if (tally.Successes > 0) {
        deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    }
    if (tally.Successes > 0 || tally.Failures > 0) {
        LOG_INFO("PendingActionQueueService: replay finished successes=%d failures=%d", tally.Successes,
                 tally.Failures);
    }
    return tally.Failures > 0 && tally.Successes == 0 ? kReplayBackoff : kReplayDelay;
}

void PendingActionQueueService::ReplayOne(ISyncCache& cache, ITrackerCollaboration& collab, const TrackerConfig& cfg,
                                          const PendingActionRecord& row, ReplayTally& tally) {
    PendingActionKind kind;
    if (!ParsePendingActionKind(row.Kind, kind)) {
        Archive(cache, row, "unknown_kind", "Unknown action kind '" + row.Kind + "'.");
        ++tally.Failures;
        return;
    }
    if (OfflineQueueReplayPolicy::ShouldArchive(row.Attempts)) {
        Archive(cache, row, "max_attempts", row.LastError);
        ++tally.Failures;
        return;
    }
    // `sending` at the start of a pass means an earlier pass sent it but could not record the outcome.
    const bool maybeSent = row.State == PendingActionState::kAmbiguous || row.State == PendingActionState::kSending;
    if (maybeSent && kind == PendingActionKind::WorklogAdd) {
        // A worklog cannot be matched reliably on the tracker, so it is never resent blind.
        try {
            cache.UpdatePendingAction(row.Id, PendingActionState::kNeedsReview, row.Attempts,
                                      "Sent, but the response was lost. Check the issue before sending it again.");
        } catch (const std::exception& ex) {
            LOG_ERROR("PendingActionQueueService: marking id=%lld for review failed: %s",
                      static_cast<long long>(row.Id), ex.what());
            ++tally.Failures;
        }
        return;
    }
    if (maybeSent && kind == PendingActionKind::CommentAdd && ResolveAmbiguousComment(cache, collab, row, tally)) {
        return;
    }
    // Claim the stored row before sending. The compare-and-set skips a row discarded or changed since
    // this pass loaded its copy, and persists the in-flight marker: if the process dies mid-send, the
    // next start turns the row ambiguous and checks the tracker instead of sending it twice.
    bool claimed = false;
    try {
        claimed =
            cache.TransitionPendingAction(row.Id, row.State, PendingActionState::kSending, row.Attempts, row.LastError);
    } catch (const std::exception& ex) {
        LOG_ERROR("PendingActionQueueService: marking id=%lld as sending failed: %s", static_cast<long long>(row.Id),
                  ex.what());
        ++tally.Failures;
        return;
    }
    if (!claimed) {
        LOG_INFO("PendingActionQueueService: id=%lld was discarded or changed during the pass; not sent",
                 static_cast<long long>(row.Id));
        return;
    }
    const TrackerError err = Dispatch(collab, cfg, kind, row.IssueKey, row.PayloadJson);
    BackendAuditTrail::AppendResult("offline_replay_action", "offline_action_replay", row.IssueKey,
                                    std::to_string(row.Id), err.IsOk(), err.Detail, AuditExtra(row));
    if (!err.IsOk()) {
        RecordFailure(cache, row, err, pendingaction::StateAfterFailedSend(kind, err), tally);
        return;
    }
    ++tally.Successes;
    try {
        cache.DeletePendingAction(row.Id);
    } catch (const std::exception& ex) {
        // The row stays `sending`, so the next pass checks the tracker rather than sending again.
        LOG_ERROR("PendingActionQueueService: removing sent id=%lld failed: %s", static_cast<long long>(row.Id),
                  ex.what());
    }
}

bool PendingActionQueueService::ResolveAmbiguousComment(ISyncCache& cache, ITrackerCollaboration& collab,
                                                        const PendingActionRecord& row, ReplayTally& tally) {
    std::string body;
    std::int64_t queuedAt = 0;
    if (!pendingaction::ParseCommentActionPayload(row.PayloadJson, body, queuedAt)) {
        return false; // the send path archives an unreadable payload
    }
    Result<std::vector<TrackerIssueComment>, TrackerError> fetched = collab.FetchIssueComments(row.IssueKey);
    if (!fetched.has_value()) {
        // Unknown whether it landed, so it is not resent now; a retryable failure keeps it ambiguous.
        const TrackerError& err = fetched.error();
        RecordFailure(cache, row, err, err.IsRetryable() ? PendingActionState::kAmbiguous : "", tally);
        return true;
    }
    if (!pendingaction::CommentAlreadyPosted(fetched.value(), body, queuedAt)) {
        return false;
    }
    try {
        cache.DeletePendingAction(row.Id);
    } catch (const std::exception& ex) {
        LOG_ERROR("PendingActionQueueService: removing already-posted id=%lld failed: %s",
                  static_cast<long long>(row.Id), ex.what());
        ++tally.Failures;
        return true;
    }
    ++tally.Successes;
    nlohmann::json extra = AuditExtra(row);
    extra["dedup"] = true;
    BackendAuditTrail::AppendResult("offline_replay_action", "offline_action_replay", row.IssueKey,
                                    std::to_string(row.Id), true, std::string(), extra);
    LOG_INFO("PendingActionQueueService: comment id=%lld on %s had already been posted; not sending it again",
             static_cast<long long>(row.Id), row.IssueKey.c_str());
    return true;
}

void PendingActionQueueService::RecordFailure(ISyncCache& cache, const PendingActionRecord& row,
                                              const TrackerError& error, const char* retryState, ReplayTally& tally) {
    ++tally.Failures;
    if (error.IsTransport()) {
        tally.TransportDown = true;
    }
    if (retryState[0] == '\0') {
        Archive(cache, row, "replay_rejected", error.Detail);
        return;
    }
    const int attempts = row.Attempts + 1;
    try {
        cache.UpdatePendingAction(row.Id, retryState, attempts, error.Detail);
    } catch (const std::exception& ex) {
        LOG_ERROR("PendingActionQueueService: recording the failure of id=%lld failed: %s",
                  static_cast<long long>(row.Id), ex.what());
        return;
    }
    if (OfflineQueueReplayPolicy::ShouldArchive(attempts)) {
        Archive(cache, row, "max_attempts", error.Detail);
    }
}

void PendingActionQueueService::Archive(ISyncCache& cache, const PendingActionRecord& row, const char* reason,
                                        const std::string& detail) {
    try {
        cache.ArchivePendingAction(row.Id, reason, detail);
        LOG_WARN("PendingActionQueueService: %s id=%lld on %s moved to failed actions (%s): %s", row.Kind.c_str(),
                 static_cast<long long>(row.Id), row.IssueKey.c_str(), reason, detail.c_str());
    } catch (const std::exception& ex) {
        LOG_ERROR("PendingActionQueueService: archiving id=%lld failed: %s", static_cast<long long>(row.Id), ex.what());
    }
}

void PendingActionQueueService::PublishSnapshot(ISyncCache& cache) {
    // Held across the reload so an older read can never overwrite a newer one. Only workers take it,
    // and the UI thread reads the published pointer without locking.
    std::lock_guard<std::mutex> lock(publishMutex_);
    std::shared_ptr<PendingActionsSnapshot> next = std::make_shared<PendingActionsSnapshot>();
    try {
        next->Pending = cache.LoadPendingActions();
        next->Dead = cache.LoadDeadPendingActions();
    } catch (const std::exception& ex) {
        LOG_WARN("PendingActionQueueService: reloading the queue failed; keeping the previous view: %s", ex.what());
        return;
    }
    if (deps_.CacheShared().get() != &cache) {
        return; // the cache file was replaced meanwhile: its rows must not become the published view
    }
    std::atomic_store(&snapshot_, std::shared_ptr<const PendingActionsSnapshot>(std::move(next)));
    snapshotLoaded_.store(true);
}

void PendingActionQueueService::RunCacheActionAsync(const char* what, std::function<void(ISyncCache&)> action) {
    const std::shared_ptr<ISyncCache> cache = deps_.CacheShared();
    if (!cache) {
        return;
    }
    try {
        deps_.LaunchBackgroundTask([this, cache, what, action]() {
            try {
                action(*cache);
            } catch (const std::exception& ex) {
                LOG_ERROR("PendingActionQueueService::%s failed: %s", what, ex.what());
            }
            PublishSnapshot(*cache);
        });
    } catch (const std::exception& ex) {
        LOG_WARN("PendingActionQueueService: could not start %s: %s", what, ex.what());
    }
}

void PendingActionQueueService::Discard(const std::vector<std::int64_t>& ids) {
    RunCacheActionAsync("Discard", [ids](ISyncCache& cache) {
        for (const std::int64_t id : ids) {
            cache.DeletePendingAction(id);
        }
        LOG_INFO("PendingActionQueueService: discarded %zu queued action(s)", ids.size());
    });
}

void PendingActionQueueService::RestoreDead(const std::vector<std::int64_t>& originalIds) {
    RunCacheActionAsync("RestoreDead", [originalIds](ISyncCache& cache) {
        for (const std::int64_t id : originalIds) {
            (void)cache.RestoreDeadPendingAction(id);
        }
    });
}

void PendingActionQueueService::DeleteDead(const std::vector<std::int64_t>& deadIds) {
    RunCacheActionAsync("DeleteDead", [deadIds](ISyncCache& cache) {
        for (const std::int64_t id : deadIds) {
            cache.DeleteDeadPendingAction(id);
        }
    });
}

void PendingActionQueueService::SendAgain(std::int64_t id) {
    RunCacheActionAsync("SendAgain", [id](ISyncCache& cache) {
        // Only a row that needs review: any other state keeps its own replay path (an ambiguous comment
        // is still looked up on the tracker before it is sent again).
        if (!cache.TransitionPendingAction(id, PendingActionState::kNeedsReview, PendingActionState::kPending, 0,
                                           std::string())) {
            LOG_INFO("PendingActionQueueService: send-again id=%lld skipped; it no longer needs review",
                     static_cast<long long>(id));
        }
    });
    RestartReplayTimersNow(std::chrono::steady_clock::now());
}
