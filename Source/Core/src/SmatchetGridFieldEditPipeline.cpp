#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "Logger.h"
#include "MainThreadDispatcher.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "SmatchetUiSession.h"
#include "StringUtil.h"
#include "TrackerHttpUtils.h"
#include "UiPerfMonitor.h"

#include "imgui.h"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

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
        std::string qerr;
        const std::int64_t qid =
            app.QueueFieldEditOffline(edit.IssueId, edit.Field.Id, result.QueuedFieldsPayloadJson, qerr,
                                      edit.OriginalRichValue, edit.OriginalValue, edit.HasOriginalValue);
        if (qid <= 0) {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("toast.offline_error", "Offline Error"),
                qerr.empty() ? std::string(SmatchetLocalization::T("toast.offline_queue_failed",
                                                                   "Failed to queue offline field edit."))
                             : qerr,
                ToastType::Error);
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Error;
            feedback.Message = qerr;
            feedback.FramesRemaining = 0;
            d.cellFeedbackByKey[editKey] = feedback;
        } else if (!app.ApplyFieldEditResult(edit.IssueId, result.ApplyResult, applyError)) {
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
    } else if (result.CommitKind == FieldEditCommitResult::Kind::SavedOnline) {
        const bool applied = app.ApplyFieldEditResult(edit.IssueId, result.ApplyResult, applyError);
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

// Worker body — runs OFF the UI thread. Performs the HTTP commit and, on
// transport failure, prepares an offline-queue fallback. Posts a single
// completion lambda back via MainThreadDispatcher::PostToMainThread.
//
// Captures own value copies of all fields needed; AppController& is the
// only reference and remains valid for the lifetime of the app (workers
// are joined before destruction via JoinBackgroundTasks).
void RunCommitWorker(AppController& app, UiDrawSession& d, PendingFieldEdit edit, std::string originalEstimateSnapshot,
                     std::string remainingEstimateSnapshot, std::string issueTypeKeyForNetwork) {
    FieldEditCommitResult result;
    result.CommitKind = FieldEditCommitResult::Kind::Failed;

    if (app.SubmitFieldEditNetworkOnly(edit.IssueId, edit.Field, edit.Values, originalEstimateSnapshot,
                                       remainingEstimateSnapshot, issueTypeKeyForNetwork, result.ApplyResult)) {
        result.Ok = true;
        result.CommitKind = FieldEditCommitResult::Kind::SavedOnline;
        result.Error.clear();
    } else {
        result.Error = result.ApplyResult.Error;
        // N12 item 13b: the pipeline classified the mutation's TrackerError into ErrorTransient —
        // the offline-queue fallback no longer sniffs the flattened text.
        if (result.ApplyResult.ErrorTransient && AppController::FieldEditSupportsOfflineQueue(edit.Field)) {
            AppController::FieldEditResult prepared;
            std::string payloadJson;
            std::string prepErr;
            if (app.TryPrepareOfflineFieldEdit(edit.IssueId, edit.Field, edit.Values, originalEstimateSnapshot,
                                               remainingEstimateSnapshot, issueTypeKeyForNetwork, prepared, payloadJson,
                                               prepErr)) {
                result.ApplyResult = std::move(prepared);
                result.QueuedFieldsPayloadJson = std::move(payloadJson);
                result.CommitKind = FieldEditCommitResult::Kind::QueuedOffline;
                result.Ok = true;
                result.Error.clear();
            } else if (!prepErr.empty()) {
                result.Error = prepErr;
            }
        }
    }

    // Hand the result back to the UI thread. The dispatcher's bounded queue
    // and BeginShutdown-aware Post are safe even if the app is mid-teardown.
    app.PostToMainThread(
        [&app, &d, edit, result]() mutable { ApplyCommitResultOnUiThread(app, d, edit, std::move(result)); });
}

} // namespace

// Enqueue half (called once per visible PANE per frame — review MEDIUM-1 split):
// folds this pane's freshly committed edits into the session queue. No dispatch,
// no chip decay — those run ONCE per frame in PumpGridFieldEdits (host-driven).
void EnqueueGridFieldEdits(UiDrawSession& d, const std::vector<PendingFieldEdit>& pendingEdits, bool readOnlyMode) {
    {
        // Keep queued edits latest-per-cell (drop older queued item for same cell).
        if (!readOnlyMode) {
            for (const auto& edit : pendingEdits) {
                const std::string editKey = BuildCellKey(edit.IssueId, edit.Field.Id);
                for (auto it = d.queuedFieldEdits.begin(); it != d.queuedFieldEdits.end();) {
                    if (BuildCellKey(it->IssueId, it->Field.Id) == editKey) {
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

        if (readOnlyMode) {
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

// Pump half (called ONCE per frame by the pane-window host with the FOCUSED pane's
// live snapshot — review MEDIUM-1): dispatches the next queued edit to a worker and
// decays success chips. Running this per visible pane faded chips N× faster and
// could snapshot estimate bases from a non-focused pane's frozen ticket snapshot.
void PumpGridFieldEdits(AppController& app, UiDrawSession& d, const std::vector<CachedTicket>& tickets,
                        bool readOnlyMode) {
    SMATCHET_UI_PERF_SCOPE("grid.cell_commit_pump");

    // Dispatch the next queued edit to a worker thread. Only one in flight
    // at a time so the offline-queue ordering matches user intent and the
    // status chip can show progress meaningfully.
    if (!readOnlyMode && !d.hasInFlightEdit && !d.queuedFieldEdits.empty()) {
        PendingFieldEdit edit = d.queuedFieldEdits.front();
        d.queuedFieldEdits.pop_front();

        std::string originalEstimateSnapshot;
        std::string remainingEstimateSnapshot;
        std::string issueTypeKeyForNetwork;
        const auto snapshotIt = std::find_if(tickets.begin(), tickets.end(),
                                             [&](const CachedTicket& ticket) { return ticket.id == edit.IssueId; });
        if (snapshotIt != tickets.end()) {
            originalEstimateSnapshot = snapshotIt->GetFieldValue("timeoriginalestimate");
            remainingEstimateSnapshot = snapshotIt->GetFieldValue("timeestimate");
            issueTypeKeyForNetwork = ToLowerAsciiCopy(TrimCopy(snapshotIt->GetFieldValue("issuetype")));
        }

        d.hasInFlightEdit = true;
        // Retained for legacy diagnostics + future inspection (in-flight cell
        // identification). The worker carries its own copies so these are
        // diagnostic-only after dispatch.
        d.inFlightEdit = edit;
        d.inFlightOriginalEstimateSnapshot = originalEstimateSnapshot;
        d.inFlightRemainingEstimateSnapshot = remainingEstimateSnapshot;
        d.inFlightIssueTypeKeySnapshot = issueTypeKeyForNetwork;
        d.inFlightDelayFrames = 0;

        CellWriteFeedback feedback;
        feedback.State = CellWriteState::Saving;
        feedback.Message = "Saving to Tracker...";
        feedback.FramesRemaining = 0;
        d.cellFeedbackByKey[BuildCellKey(edit.IssueId, edit.Field.Id)] = feedback;

        LOG_TRACE("ProcessGridFieldEdits: dispatching worker for issue=%s field=%s", edit.IssueId.c_str(),
                  edit.Field.Id.c_str());

        // Worker runs SubmitFieldEditNetworkOnly (HTTP) and posts the result
        // back to the UI thread. Lifetime: AppController owns the worker
        // thread; JoinBackgroundTasks is called before destruction.
        app.LaunchBackgroundTask(
            [&app, &d, edit, originalEstimateSnapshot, remainingEstimateSnapshot, issueTypeKeyForNetwork]() mutable {
                RunCommitWorker(app, d, std::move(edit), std::move(originalEstimateSnapshot),
                                std::move(remainingEstimateSnapshot), std::move(issueTypeKeyForNetwork));
            });
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
void ProcessGridFieldEdits(AppController& app, UiDrawSession& d, const std::vector<CachedTicket>& tickets,
                           const std::vector<PendingFieldEdit>& pendingEdits, bool readOnlyMode) {
    EnqueueGridFieldEdits(d, pendingEdits, readOnlyMode);
    PumpGridFieldEdits(app, d, tickets, readOnlyMode);
}
