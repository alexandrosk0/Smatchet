#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "FieldEditPipelineService.h" // FieldEditCommitRequest / FieldEditCommitOutcome (CommitOrQueueFieldEdit)
#include "Logger.h"
#include "MainThreadDispatcher.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "TrackerHttpUtils.h"
#include "UiPerfMonitor.h"

#include "imgui.h"
#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

// Transitional shim: AppController::ApplyFieldEditResult flipped to VoidResult (#21 AppController
// public-API flip). This grid pipeline still branches on bool + an error string for its toast /
// cell-feedback, so adapt the VoidResult back here. Converting these two call sites to consume
// VoidResult directly is a follow-up. The result lands in the pane the edit was made in.
bool ApplyFieldEditResultBool(AppController& app, const PendingFieldEdit& edit, const FieldEditResult& result,
                              std::string& outError) {
    const VoidResult r = app.ApplyFieldEditResult(edit.Target, edit.IssueId, result);
    if (!r.has_value()) {
        outError = r.error();
    }
    return r.has_value();
}

// Apply the final commit result on the UI thread. Runs from the
// MainThreadDispatcher::Drain at the top of the next frame, which mutates
// `d` (the singleton UiDrawSession via `g_ui`) safely from the UI thread.
//
// This function is intentionally light — every cost here is what the user
// pays after the HTTP roundtrip has already completed off-thread, so the
// post-back cost is "did the commit succeed?" + cache write + toast.
void ApplyCommitResultOnUiThread(AppController& app, UiDrawSession& d, const PendingFieldEdit& edit,
                                 FieldEditCommitResult result) {
    const std::string editKey = BuildCellKey(edit.IssueId, edit.Field.Id);
    std::string applyError;

    if (result.CommitKind == FieldEditCommitResult::Kind::QueuedOffline) {
        // The worker already persisted the edit to the offline queue (CommitOrQueue); only the
        // optimistic local apply is left.
        if (!ApplyFieldEditResultBool(app, edit, result.ApplyResult, applyError)) {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.apply_error", "Apply Error"),
                applyError.empty() ? std::string(SmatchetLocalization::T("toast.apply_queued_failed",
                                                                         "Failed to apply queued field edit."))
                                   : applyError,
                ToastType::Error);
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Error;
            feedback.Message = applyError;
            feedback.FramesRemaining = 0;
            d.cellFeedbackByKey[editKey] = feedback;
        } else {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.queued_offline", "Queued Offline"),
                SmatchetLocalization::T("toast.field_edit_will_sync",
                                        "Field edit will sync when Tracker is reachable."),
                ToastType::Info);
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Success;
            feedback.Message = "Queued";
            feedback.FramesRemaining = 240;
            d.cellFeedbackByKey[editKey] = feedback;
        }
        if (result.QueuedAfterTransportFailure) {
            // The tracker just failed a live request: probe now so the next edit queues first
            // instead of waiting out another retry window before the regular probe notices.
            app.RequestTrackerProbeNow();
        }
    } else if (result.CommitKind == FieldEditCommitResult::Kind::SavedOnline) {
        const bool applied = ApplyFieldEditResultBool(app, edit, result.ApplyResult, applyError);
        if (!applied) {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.save_error", "Save Error"),
                applyError.empty() ? std::string(SmatchetLocalization::T("toast.apply_saved_failed",
                                                                         "Failed to apply saved field update."))
                                   : applyError,
                ToastType::Error);
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Error;
            feedback.Message = applyError;
            feedback.FramesRemaining = 0;
            d.cellFeedbackByKey[editKey] = feedback;
        } else {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.success", "Success"),
                SmatchetLocalization::T("toast.field_update_saved", "Field update saved to Tracker."),
                ToastType::Success);
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Success;
            feedback.Message = "Saved";
            feedback.FramesRemaining = 180;
            d.cellFeedbackByKey[editKey] = feedback;
        }
    } else {
        d.gridEditError = result.Error.empty() ? std::string(SmatchetLocalization::T(
                                                     "toast.save_field_failed", "Failed to save Tracker field update."))
                                               : result.Error;
        d.gridEditSuccess.clear();
        CellWriteFeedback feedback;
        feedback.State = CellWriteState::Error;
        feedback.Message = d.gridEditError;
        feedback.FramesRemaining = 0;
        d.cellFeedbackByKey[editKey] = feedback;
    }

    // Clear the in-flight gate so the next queued edit can dispatch on the
    // following frame. Done unconditionally so a failed save does not
    // strand the deque.
    d.hasInFlightEdit = false;
}

// Worker half of the commit: CommitOrQueue decides queue-first (tracker offline at dispatch) or
// network-first with a queue fallback, and persists a queued edit here, off the UI thread. A throw is
// reported as a failed commit so the caller still posts a result back.
FieldEditCommitResult CommitOnWorker(AppController& app, const PendingFieldEdit& edit,
                                     const FieldEditCommitRequest& req) {
    FieldEditCommitResult result;
    result.CommitKind = FieldEditCommitResult::Kind::Failed;
    try {
        FieldEditCommitOutcome o = app.CommitOrQueueFieldEdit(req);
        switch (o.Kind) {
        case FieldEditCommitKind::SavedOnline:
            result.CommitKind = FieldEditCommitResult::Kind::SavedOnline;
            break;
        case FieldEditCommitKind::QueuedOffline:
            result.CommitKind = FieldEditCommitResult::Kind::QueuedOffline;
            break;
        case FieldEditCommitKind::Failed:
            result.CommitKind = FieldEditCommitResult::Kind::Failed;
            break;
        }
        result.Ok = o.Kind != FieldEditCommitKind::Failed;
        result.Error = std::move(o.Error);
        result.ApplyResult = std::move(o.Apply);
        result.QueuedAfterTransportFailure = o.QueuedAfterTransportFailure;
    } catch (const std::exception& ex) {
        LOG_ERROR("GridFieldEdit: commit worker threw issue=%s field=%s: %s", edit.IssueId.c_str(),
                  edit.Field.Id.c_str(), ex.what());
        result = FieldEditCommitResult();
        result.Error = "Saving this edit failed unexpectedly. If the cell still shows the old value, edit it again.";
    } catch (...) {
        LOG_ERROR("GridFieldEdit: commit worker threw a non-std exception issue=%s field=%s", edit.IssueId.c_str(),
                  edit.Field.Id.c_str());
        result = FieldEditCommitResult();
        result.Error = "Saving this edit failed unexpectedly. If the cell still shows the old value, edit it again.";
    }
    return result;
}

// Worker body — runs OFF the UI thread and posts exactly one completion lambda back via
// MainThreadDispatcher::PostCompletionToMainThread, which a full queue never evicts. CommitOnWorker
// contains any exception from the commit, so the post (and with it the UI-thread in-flight gate
// release) is reached on every path.
//
// Captures own value copies of all fields needed; AppController& is the
// only reference and remains valid for the lifetime of the app (workers
// are joined before destruction via JoinBackgroundTasks).
void RunCommitWorker(AppController& app, UiDrawSession& d, const PendingFieldEdit& edit,
                     const FieldEditCommitRequest& req) {
    FieldEditCommitResult result = CommitOnWorker(app, edit, req);

    // Hand the result back to the UI thread as a completion: it releases the pump's in-flight gate,
    // so a queue full of other posts must not evict it. The dispatcher's BeginShutdown-aware post is
    // safe even if the app is mid-teardown.
    app.mainThreadDispatcher.PostCompletionToMainThread(
        [&app, &d, edit, result]() mutable { ApplyCommitResultOnUiThread(app, d, edit, std::move(result)); });
}

} // namespace

// Enqueue half (called once per visible PANE per frame — review MEDIUM-1 split):
// folds this pane's freshly committed edits into the session queue. No dispatch,
// no chip decay — those run ONCE per frame in PumpGridFieldEdits (host-driven).
void EnqueueGridFieldEdits(UiDrawSession& d, const std::vector<PendingFieldEdit>& pendingEdits, bool readOnlyMode) {
    {
        // Keep queued edits latest-per-cell (drop older queued item for same cell). A cell is scoped
        // by its tracker too: two panes may show the same issue key on different trackers.
        if (!readOnlyMode) {
            for (const auto& edit : pendingEdits) {
                const std::string editKey = BuildCellKey(edit.IssueId, edit.Field.Id);
                for (auto it = d.queuedFieldEdits.begin(); it != d.queuedFieldEdits.end();) {
                    if (it->Target.BackendKey == edit.Target.BackendKey &&
                        BuildCellKey(it->IssueId, it->Field.Id) == editKey) {
                        it = d.queuedFieldEdits.erase(it);
                    } else {
                        ++it;
                    }
                }
                d.queuedFieldEdits.push_back(edit);
            }
        } else if (!pendingEdits.empty()) {
            d.gridEditSuccess.clear();
            d.gridEditError = "Edit skipped: Tracker is in read-only mode.";
        }

        // Pillar 6: only the user's own Read-only preference discards not-yet-sent edits. A tracker
        // error banner also makes the grid read-only, but those edits are held (the pump does not
        // dispatch while read-only) and go out once the tracker is usable again.
        if (readOnlyMode && d.cfg.ReadOnlyMode) {
            d.queuedFieldEdits.clear();
        }
    }

    // Moved from the former ProcessGridFieldEdits tail — behaviour-preserving for
    // the error banner: the worker-completion fold (ApplyCommitResultOnUiThread,
    // which sets gridEditError) runs from MainThreadDispatcher::Drain at the TOP
    // of the frame, before the pane loop, so it precedes this clear in BOTH the
    // old (post-pump) and new (pre-pump) positions. The delta-review concern (an
    // in-flight failure folded the same frame the user produces fresh edits gets
    // wiped) therefore exists in both positions and remains OPEN — tracked for
    // Slice 3's per-pane cue work. readOnlyMode guard: the read-only branch above
    // SETS gridEditError for these same pendingEdits; clearing it three lines
    // later self-wiped the banner before anything rendered it (review M).
    if (!readOnlyMode && !pendingEdits.empty()) {
        d.gridEditError.clear();
    }
}

void DiscardQueuedGridFieldEditsOnBackendSwitch(UiDrawSession& d) {
    if (d.queuedFieldEdits.empty()) {
        return;
    }
    LOG_WARN("GridFieldEdit: discarded %zu unsent edit(s) on tracker backend switch", d.queuedFieldEdits.size());
    d.queuedFieldEdits.clear();
    d.gridEditSuccess.clear();
    d.gridEditError = "Unsent edits discarded: the tracker backend changed before they could be sent.";
}

// Pump half (called ONCE per frame by the pane-window host — review MEDIUM-1):
// dispatches the next queued edit to a worker and decays success chips. Running this
// per visible pane faded chips N× faster. The edit's estimate bases come from the pane
// it was made in, which need not be the focused one (#2260).
void PumpGridFieldEdits(AppController& app, UiDrawSession& d, bool readOnlyMode) {
    SMATCHET_UI_PERF_SCOPE("grid.cell_commit_pump");

    // Dispatch the next queued edit to a worker thread. Only one in flight
    // at a time so the offline-queue ordering matches user intent and the
    // status chip can show progress meaningfully.
    if (!readOnlyMode && !d.hasInFlightEdit && !d.queuedFieldEdits.empty()) {
        PendingFieldEdit edit = d.queuedFieldEdits.front();
        d.queuedFieldEdits.pop_front();
        if (edit.Target.PaneId.empty()) {
            edit.Target = app.LatchPendingActionTarget(); // queued by a tool, not a pane: the focused pane
        }
        // Pillar 6: the worker queues first when the last probe says the tracker is unreachable, instead
        // of spending the HTTP retry window before the edit is saved anywhere. Read now, not at enqueue:
        // an edit can wait behind another in flight.
        edit.Target.Connectivity = app.GetLastTrackerConnectivityState();

        FieldEditCommitRequest req;
        req.IssueId = edit.IssueId;
        req.Field = edit.Field;
        req.Values = edit.Values;
        req.OriginalRichValue = edit.OriginalRichValue;
        req.OriginalValue = edit.OriginalValue;
        req.HasOriginalValue = edit.HasOriginalValue;
        req.Target = edit.Target;
        const std::shared_ptr<const std::vector<CachedTicket>> paneTickets = app.TicketsSnapshotForTarget(edit.Target);
        if (paneTickets) {
            const auto snapshotIt =
                std::find_if(paneTickets->begin(), paneTickets->end(),
                             [&edit](const CachedTicket& ticket) { return ticket.id == edit.IssueId; });
            if (snapshotIt != paneTickets->end()) {
                FieldEditPipelineService::CaptureTicketSnapshots(*snapshotIt, false, req);
            }
        }

        d.hasInFlightEdit = true;
        // Retained for legacy diagnostics + future inspection (in-flight cell
        // identification). The worker carries its own copies so these are
        // diagnostic-only after dispatch.
        d.inFlightEdit = edit;
        d.inFlightOriginalEstimateSnapshot = req.OriginalEstimateSnapshot;
        d.inFlightRemainingEstimateSnapshot = req.RemainingEstimateSnapshot;
        d.inFlightIssueTypeKeySnapshot = req.IssueTypeKeySnapshot;
        d.inFlightDelayFrames = 0;

        CellWriteFeedback feedback;
        feedback.State = CellWriteState::Saving;
        feedback.Message = "Saving to Tracker...";
        feedback.FramesRemaining = 0;
        d.cellFeedbackByKey[BuildCellKey(edit.IssueId, edit.Field.Id)] = feedback;

        LOG_TRACE("ProcessGridFieldEdits: dispatching worker for issue=%s field=%s", edit.IssueId.c_str(),
                  edit.Field.Id.c_str());

        // Worker runs CommitOrQueue (HTTP or SQLite enqueue) and posts the result
        // back to the UI thread. Lifetime: AppController owns the worker
        // thread; JoinBackgroundTasks is called before destruction.
        try {
            app.LaunchBackgroundTask([&app, &d, edit, req]() { RunCommitWorker(app, d, edit, req); });
        } catch (const std::exception& ex) {
            // Thread creation failed: no worker will post back, so release the in-flight gate here
            // (otherwise every later edit waits behind it forever). The edit is reported, not
            // silently re-queued: an automatic retry would spin every frame while threads are scarce.
            LOG_ERROR("GridFieldEdit: could not start the commit worker issue=%s field=%s: %s", edit.IssueId.c_str(),
                      edit.Field.Id.c_str(), ex.what());
            d.hasInFlightEdit = false;
            d.gridEditSuccess.clear();
            d.gridEditError = "Could not start saving this edit; it was not saved. Edit the cell again to retry.";
            CellWriteFeedback failed;
            failed.State = CellWriteState::Error;
            failed.Message = d.gridEditError;
            failed.FramesRemaining = 0;
            d.cellFeedbackByKey[BuildCellKey(edit.IssueId, edit.Field.Id)] = failed;
        }
    }

    // Decrement frame counters on success chips so they fade after their
    // FramesRemaining window. Runs every frame.
    {
        for (auto it = d.cellFeedbackByKey.begin(); it != d.cellFeedbackByKey.end();) {
            if (it->second.State == CellWriteState::Success && it->second.FramesRemaining > 0) {
                --it->second.FramesRemaining;
            }

            if (it->second.State == CellWriteState::Success && it->second.FramesRemaining <= 0) {
                it = d.cellFeedbackByKey.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// Composed enqueue+pump kept for single-shot callers outside the pane-window loop
// (the perf.grid_edit_pump command in BuiltinCommands_Perf.cpp).
void ProcessGridFieldEdits(AppController& app, UiDrawSession& d, const std::vector<PendingFieldEdit>& pendingEdits,
                           bool readOnlyMode) {
    EnqueueGridFieldEdits(d, pendingEdits, readOnlyMode);
    PumpGridFieldEdits(app, d, readOnlyMode);
}
