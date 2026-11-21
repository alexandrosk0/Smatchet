// Every grid cell commit goes through this pipeline: Source/Core/src/Ui/AGENTS.md § Grid.
#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "FieldEditPipelineService.h" // FieldEditCommitRequest / FieldEditCommitOutcome (CommitOrQueueFieldEdit)
#include "Logger.h"
#include "MainThreadDispatcher.h"
#include "SmatchetLocalization.h"
#include "SmatchetToast.h"
#include "StringUtil.h"
#include "SmatchetUiSession.h"
#include "TrackerHttpUtils.h"
#include "Ui/BulkFieldEditPure.h"
#include "UiPerfMonitor.h"

#include "imgui.h"
#include <algorithm>
#include <deque>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

// A single-cell edit raises its own toast; a bulk row stays quiet and reports through its job's summary toast.
void PushUnlessBulk(bool bulkRow, const std::string& title, const std::string& message, ToastType type) {
    if (!bulkRow) {
        SmatchetToastManager::Instance().Push(title, message, type);
    }
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
    // A bulk row reports through its job's one summary toast (FoldBulkRowResult), not one toast per row, and
    // never through the single-edit error banner. Its cell badge still shows the row's own outcome.
    const bool bulkRow = edit.BulkJobId != 0;
    smatchet::grid::bulk::BulkRowState bulkState = smatchet::grid::bulk::BulkRowState::Failed;
    std::string bulkMessage;

    if (result.CommitKind == FieldEditCommitResult::Kind::QueuedOffline) {
        // The worker already persisted the edit to the offline queue (CommitOrQueue); only the
        // optimistic local apply is left.
        // The worker really enqueued it: a bulk row is "queued" even if the optimistic local apply below fails.
        bulkState = smatchet::grid::bulk::BulkRowState::Queued;
        if (!ApplyFieldEditResultBool(app, edit, result.ApplyResult, applyError)) {
            bulkMessage = applyError;
            PushUnlessBulk(bulkRow, SmatchetLocalization::T("toast.apply_error", "Apply Error"),
                           applyError.empty() ? std::string(SmatchetLocalization::T(
                                                    "toast.apply_queued_failed", "Failed to apply queued field edit."))
                                              : applyError,
                           ToastType::Error);
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Error;
            feedback.Message = applyError;
            feedback.FramesRemaining = 0;
            d.cellFeedbackByKey[editKey] = feedback;
        } else {
            PushUnlessBulk(bulkRow, SmatchetLocalization::T("toast.queued_offline", "Queued Offline"),
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
        bulkState = smatchet::grid::bulk::BulkRowState::Saved; // the tracker took it, whatever the local apply did
        if (!applied) {
            bulkMessage = applyError;
            PushUnlessBulk(bulkRow, SmatchetLocalization::T("toast.save_error", "Save Error"),
                           applyError.empty() ? std::string(SmatchetLocalization::T(
                                                    "toast.apply_saved_failed", "Failed to apply saved field update."))
                                              : applyError,
                           ToastType::Error);
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Error;
            feedback.Message = applyError;
            feedback.FramesRemaining = 0;
            d.cellFeedbackByKey[editKey] = feedback;
        } else {
            PushUnlessBulk(bulkRow, SmatchetLocalization::T("toast.success", "Success"),
                           SmatchetLocalization::T("toast.field_update_saved", "Field update saved to Tracker."),
                           ToastType::Success);
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Success;
            feedback.Message = "Saved";
            feedback.FramesRemaining = 180;
            d.cellFeedbackByKey[editKey] = feedback;
        }
    } else {
        const std::string failText = result.Error.empty()
                                         ? std::string(SmatchetLocalization::T("toast.save_field_failed",
                                                                               "Failed to save Tracker field update."))
                                         : result.Error;
        bulkMessage = failText;
        if (!bulkRow) {
            d.gridEditError = failText;
            d.gridEditSuccess.clear();
        }
        CellWriteFeedback feedback;
        feedback.State = CellWriteState::Error;
        feedback.Message = failText;
        feedback.FramesRemaining = 0;
        d.cellFeedbackByKey[editKey] = feedback;
    }

    // Clear the in-flight gate so the next queued edit can dispatch on the
    // following frame. Done unconditionally so a failed save does not
    // strand the deque.
    d.hasInFlightEdit = false;

    if (bulkRow) {
        FoldBulkRowResult(d, edit.BulkJobId, edit.IssueId, bulkState, bulkMessage);
    }
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
// MainThreadDispatcher::PostCompletionToMainThread, which a droppable-task overflow never evicts. CommitOnWorker
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

// The cell an edit targets, scoped by tracker (two panes may show the same issue key on different trackers).
std::string QueuedCellKey(const PendingFieldEdit& e) {
    return e.Target.BackendKey + '\x1f' + e.IssueId + '\x1f' + e.Field.Id;
}

// Drops every queued edit that one of `incoming` replaces (latest-per-cell). One pass over the queue against a set
// of the incoming cells, because a bulk job enqueues N edits at once and a scan per edit would be O(N^2) string
// builds on the UI thread. A cell is scoped by its tracker too: two panes may show the same issue key on
// different trackers. A dropped bulk row closes its job row so the job can still finish.
void SupersedeQueuedEdits(UiDrawSession& d, const std::vector<PendingFieldEdit>& incoming) {
    if (incoming.empty() || d.queuedFieldEdits.empty()) {
        return;
    }
    std::unordered_set<std::string> cells;
    cells.reserve(incoming.size());
    for (const PendingFieldEdit& edit : incoming) {
        cells.insert(QueuedCellKey(edit));
    }
    for (auto it = d.queuedFieldEdits.begin(); it != d.queuedFieldEdits.end();) {
        if (cells.count(QueuedCellKey(*it)) == 0) {
            ++it;
            continue;
        }
        const std::uint64_t droppedJob = it->BulkJobId;
        const std::string droppedIssue = it->IssueId;
        it = d.queuedFieldEdits.erase(it);
        if (droppedJob != 0) {
            FoldBulkRowResult(d, droppedJob, droppedIssue, smatchet::grid::bulk::BulkRowState::Skipped,
                              SmatchetLocalization::T("grid.bulk.skip_superseded", "replaced by a newer edit"));
        }
    }
}

// Queues `incoming` latest-per-cell: older queued edits for the same cells go (SupersedeQueuedEdits), and when one
// batch holds two edits of a cell only the last is queued. A dropped bulk row closes its job row.
void QueueLatestPerCell(UiDrawSession& d, const std::vector<PendingFieldEdit>& incoming) {
    if (incoming.empty()) {
        return; // the idle frame: no container is built
    }
    SupersedeQueuedEdits(d, incoming);
    if (incoming.size() == 1) {
        d.queuedFieldEdits.push_back(incoming.front());
        return;
    }
    std::unordered_map<std::string, std::size_t> lastOfCell;
    lastOfCell.reserve(incoming.size());
    for (std::size_t i = 0; i < incoming.size(); ++i) {
        lastOfCell[QueuedCellKey(incoming[i])] = i;
    }
    for (std::size_t i = 0; i < incoming.size(); ++i) {
        if (lastOfCell[QueuedCellKey(incoming[i])] == i) {
            d.queuedFieldEdits.push_back(incoming[i]);
        } else if (incoming[i].BulkJobId != 0) {
            FoldBulkRowResult(d, incoming[i].BulkJobId, incoming[i].IssueId,
                              smatchet::grid::bulk::BulkRowState::Skipped,
                              SmatchetLocalization::T("grid.bulk.skip_superseded", "replaced by a newer edit"));
        }
    }
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
            QueueLatestPerCell(d, pendingEdits);
        } else if (!pendingEdits.empty()) {
            d.gridEditSuccess.clear();
            d.gridEditError = "Edit skipped: Tracker is in read-only mode.";
        }

        // Pillar 6: only the user's own Read-only preference discards not-yet-sent edits. A tracker
        // error banner also makes the grid read-only, but those edits are held (the pump does not
        // dispatch while read-only) and go out once the tracker is usable again.
        if (readOnlyMode && d.cfg.ReadOnlyMode) {
            const std::deque<PendingFieldEdit> discarded = std::move(d.queuedFieldEdits);
            d.queuedFieldEdits.clear();
            for (const PendingFieldEdit& edit : discarded) {
                if (edit.BulkJobId != 0) {
                    FoldBulkRowResult(d, edit.BulkJobId, edit.IssueId, smatchet::grid::bulk::BulkRowState::Failed,
                                      SmatchetLocalization::T("grid.bulk.fail_read_only", "read-only mode"));
                }
            }
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
    // An edit made in a pane carries that pane's target (backend, queue namespace, generation), so it is
    // still sent to, queued for and applied in its own pane after focus moves (#2260) — keep it. Only an
    // unbound edit would bind to whichever pane is focused at dispatch, i.e. the new backend.
    const std::size_t before = d.queuedFieldEdits.size();
    d.queuedFieldEdits.erase(std::remove_if(d.queuedFieldEdits.begin(), d.queuedFieldEdits.end(),
                                            [](const PendingFieldEdit& edit) { return edit.Target.PaneId.empty(); }),
                             d.queuedFieldEdits.end());
    const std::size_t discarded = before - d.queuedFieldEdits.size();
    if (discarded == 0) {
        return;
    }
    LOG_WARN("GridFieldEdit: discarded %zu unsent unbound edit(s) on tracker backend switch", discarded);
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
            if (edit.BulkJobId != 0) {
                FoldBulkRowResult(d, edit.BulkJobId, edit.IssueId, smatchet::grid::bulk::BulkRowState::Failed,
                                  failed.Message);
            }
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

// Bulk field edit (docs/plans/bulk-edit-rows.md). A job fans out into one ordinary PendingFieldEdit per row, so each
// row goes through the same pump, CommitOrQueueFieldEdit, offline queue and audit as a single-cell edit.

namespace {

using smatchet::grid::bulk::BulkFieldEditJob;
using smatchet::grid::bulk::BulkFieldKind;
using smatchet::grid::bulk::BulkMode;
using smatchet::grid::bulk::BulkRow;
using smatchet::grid::bulk::BulkRowState;

// Finished jobs kept for the summary toast's click action and the later Revert.
constexpr std::size_t kMaxKeptBulkJobs = 8;

// One toast per finished job. "Queued" counts only rows the worker really enqueued (a row stays Pending until then).
void PushBulkSummaryToast(UiDrawSession& d, const BulkFieldEditJob& job) {
    const smatchet::grid::bulk::BulkSummary summary = smatchet::grid::bulk::Summarize(job);
    std::string message = smatchet::grid::bulk::BulkSummaryText(summary);
    std::set<std::string> failedIds;
    std::string firstError;
    for (const BulkRow& row : job.Rows) {
        if (row.State == BulkRowState::Failed) {
            failedIds.insert(row.IssueId);
            if (firstError.empty() && !row.Message.empty()) {
                firstError = row.IssueId + ": " + row.Message;
            }
        }
    }
    if (!firstError.empty()) {
        // A byte-level resize could cut a UTF-8 character in half; this backs off to a character boundary.
        message += "\n" + TruncateUtf8WithEllipsis(firstError, 160);
    }
    const std::string fieldName = job.Field.Name.empty() ? job.Field.Id : job.Field.Name;
    const std::string title = SmatchetLocalization::Format("grid.bulk.done_title", "Bulk edit: %s", fieldName.c_str());
    const ToastType type = smatchet::grid::bulk::BulkSummaryToastType(summary);
    if (failedIds.empty()) {
        SmatchetToastManager::Instance().Push(title, message, type, 6000);
        return;
    }
    // A click on the toast selects the failed rows, so the user can inspect or retry them.
    UiDrawSession* session = &d;
    const std::string paneId = job.PaneId;
    SmatchetToastManager::Instance().PushClickable(title, message, type, 8000, [session, paneId, failedIds]() {
        SelectGridRowsByIssueIds(*session, paneId, failedIds);
    });
}

void PruneFinishedBulkJobs(UiDrawSession& d) {
    for (auto it = d.bulkFieldEditJobs.begin();
         d.bulkFieldEditJobs.size() > kMaxKeptBulkJobs && it != d.bulkFieldEditJobs.end();) {
        if (smatchet::grid::bulk::Summarize(it->second).Done) {
            it = d.bulkFieldEditJobs.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

smatchet::grid::bulk::BulkFieldEditJob
PrepareBulkFieldEditJob(AppController& app, const std::string& paneId, const TrackerField& field,
                        const std::vector<std::string>& issueIds, const std::vector<std::string>& values,
                        smatchet::grid::bulk::BulkMode mode, std::vector<std::vector<std::string>>* rowValues) {
    BulkFieldEditJob job;
    job.PaneId = paneId;
    job.Field = field;
    job.Values = values;
    job.Mode = mode;
    const BulkFieldKind kind = smatchet::grid::bulk::ClassifyBulkField(field);
    const bool preview = rowValues == nullptr;
    if (rowValues) {
        rowValues->clear();
    }

    // The pane's own tickets, as they are now. Memory only: no network, no SQLite.
    const PendingActionTarget target = app.LatchPendingActionTargetForPane(paneId);
    const std::shared_ptr<const std::vector<CachedTicket>> snapshot = app.TicketsSnapshotForTarget(target);
    std::unordered_map<std::string, const CachedTicket*> byId;
    if (snapshot) {
        byId.reserve(snapshot->size());
        for (const CachedTicket& ticket : *snapshot) {
            byId.emplace(ticket.id, &ticket);
        }
    }

    std::unordered_set<std::string> seen;
    for (const std::string& issueId : issueIds) {
        if (issueId.empty() || !seen.insert(issueId).second) {
            continue; // one row per issue
        }
        BulkRow row;
        row.IssueId = issueId;
        std::vector<std::string> newValues;
        const auto it = byId.find(issueId);
        if (it == byId.end()) {
            row.State = BulkRowState::Skipped;
            row.Message = SmatchetLocalization::T("grid.bulk.skip_gone", "no longer in this view");
        } else {
            const CachedTicket& ticket = *it->second;
            row.BeforeValue = ticket.GetFieldValue(field.Id);
            row.BeforeValues = smatchet::grid::bulk::BeforeValuesFor(field, kind, row.BeforeValue);
            // Edit permission is a memory read (EditMetaCacheService, under its own mutex). It is looked up under the
            // job's latched backend key, not the focused pane's, the same as the commit side
            // (CanEditFieldForIssueWithType(target.BackendKey, ...)). The row's own issue type is passed so the check
            // does not scan the whole ticket list once per row.
            const std::string issueType = ToLowerAsciiCopy(TrimCopy(ticket.GetFieldValue("issuetype")));
            if (!app.CanEditFieldForIssueWithType(target.BackendKey, issueId, field.Id, &field, issueType)) {
                row.State = BulkRowState::Skipped;
                row.Message = SmatchetLocalization::T("grid.bulk.skip_not_editable", "not editable on this issue");
            } else if (mode != BulkMode::Replace && kind == BulkFieldKind::MultiChoice &&
                       !smatchet::grid::bulk::AllEntriesResolve(field, row.BeforeValues)) {
                // Add / Remove merges into the current set; an entry outside the option list (an archived version, a
                // component of another project) cannot be written back as an id, so this row is left alone.
                row.State = BulkRowState::Skipped;
                row.Message =
                    SmatchetLocalization::T("grid.bulk.skip_unresolved", "current value is not in this option list");
            } else if (!preview) {
                newValues = smatchet::grid::bulk::NewValuesForRow(kind, mode, row.BeforeValues, values);
                if (smatchet::grid::bulk::IsRowUnchanged(kind, row.BeforeValues, newValues)) {
                    row.State = BulkRowState::Skipped;
                    row.Message = SmatchetLocalization::T("grid.bulk.skip_unchanged", "already has this value");
                }
            }
        }
        job.Rows.push_back(row);
        if (rowValues) {
            rowValues->push_back(std::move(newValues));
        }
    }
    return job;
}

std::uint64_t StartBulkFieldEdit(AppController& app, UiDrawSession& d, const std::string& paneId,
                                 const TrackerField& field, const std::vector<std::string>& issueIds,
                                 const std::vector<std::string>& values, smatchet::grid::bulk::BulkMode mode,
                                 bool readOnlyMode) {
    const std::string fieldName = field.Name.empty() ? field.Id : field.Name;
    const std::string title = SmatchetLocalization::Format("grid.bulk.done_title", "Bulk edit: %s", fieldName.c_str());
    if (readOnlyMode) {
        SmatchetToastManager::Instance().Push(
            title, SmatchetLocalization::T("grid.bulk.read_only", "Nothing was changed: the grid is read-only."),
            ToastType::Warning);
        return 0;
    }

    std::vector<std::vector<std::string>> rowValues;
    BulkFieldEditJob job = PrepareBulkFieldEditJob(app, paneId, field, issueIds, values, mode, &rowValues);
    const smatchet::grid::bulk::BulkSummary prepared = smatchet::grid::bulk::Summarize(job);
    const int toSend = prepared.Remaining;
    if (toSend == 0) {
        // Nothing to send: report it now; no job is kept.
        SmatchetToastManager::Instance().Push(title,
                                              SmatchetLocalization::Format("grid.bulk.nothing_to_change",
                                                                           "Nothing to change: %d skipped.",
                                                                           prepared.Skipped),
                                              ToastType::Info);
        return 0;
    }

    job.Id = d.nextBulkJobId++;
    const PendingActionTarget target = app.LatchPendingActionTargetForPane(paneId);
    std::vector<PendingFieldEdit> edits;
    edits.reserve(static_cast<std::size_t>(toSend));
    for (std::size_t i = 0; i < job.Rows.size(); ++i) {
        const BulkRow& row = job.Rows[i];
        if (row.State != BulkRowState::Pending) {
            continue;
        }
        PendingFieldEdit edit;
        edit.IssueId = row.IssueId;
        edit.Field = field;
        edit.Values = rowValues[i];
        // ADR-0016: the same scalar-base capture QueueEdit does, so a queued bulk row that replays after someone
        // else changed the value raises a conflict instead of overwriting it.
        smatchet::grid::bulk::CaptureScalarBase(row.BeforeValue, edit);
        edit.Target = target;
        edit.BulkJobId = job.Id;
        edits.push_back(std::move(edit));
    }

    // Keep the selection by issue id: the first landed edit bumps the snapshot revision, which clears the
    // sort-order row selection (SmatchetActiveProjectGridTable.cpp rebuilds it from these ids).
    if (GridPane* pane = FindGridPaneById(d.gridPanes, paneId)) {
        GridRectSelection& sel = pane->gridState.RectSel;
        sel.KeepIssueIds.clear();
        sel.KeepIssueIds.insert(issueIds.begin(), issueIds.end());
        sel.KeepRows = sel.Rows;
    }

    const std::uint64_t jobId = job.Id;
    d.bulkFieldEditJobs[jobId] = std::move(job);
    PruneFinishedBulkJobs(d);
    EnqueueGridFieldEdits(d, edits, false);
    SmatchetToastManager::Instance().Push(
        title, SmatchetLocalization::Format("grid.bulk.saving", "Saving %d rows...", toSend), ToastType::Info, 2500);
    LOG_INFO("GridBulkEdit: job %llu field=%s rows=%d skipped=%d", static_cast<unsigned long long>(jobId),
             field.Id.c_str(), toSend, prepared.Skipped);
    return jobId;
}

void FoldBulkRowResult(UiDrawSession& d, std::uint64_t jobId, const std::string& issueId,
                       smatchet::grid::bulk::BulkRowState state, const std::string& message) {
    const auto it = d.bulkFieldEditJobs.find(jobId);
    if (it == d.bulkFieldEditJobs.end()) {
        LOG_WARN("GridBulkEdit: result for unknown job %llu issue=%s ignored", static_cast<unsigned long long>(jobId),
                 issueId.c_str());
        return;
    }
    BulkFieldEditJob& job = it->second;
    if (!smatchet::grid::bulk::SetRowState(job, issueId, state, message)) {
        LOG_WARN("GridBulkEdit: job %llu has no row for issue=%s", static_cast<unsigned long long>(jobId),
                 issueId.c_str());
        return;
    }
    if (!job.Reported && smatchet::grid::bulk::Summarize(job).Done) {
        job.Reported = true;
        PushBulkSummaryToast(d, job);
    }
}

void SelectGridRowsByIssueIds(UiDrawSession& d, const std::string& paneId, const std::set<std::string>& issueIds) {
    GridPane* pane = FindGridPaneById(d.gridPanes, paneId);
    if (!pane) {
        return; // the pane was closed since
    }
    GridRectSelection& sel = pane->gridState.RectSel;
    sel.KeepIssueIds = issueIds;
    sel.KeepRows = sel.Rows;
    // A sort signature that matches no order makes the pane's next draw clear the selection and rebuild it from
    // KeepIssueIds against its current sort order (SmatchetActiveProjectGridTable.cpp).
    sel.SortSignature = 0;
}
