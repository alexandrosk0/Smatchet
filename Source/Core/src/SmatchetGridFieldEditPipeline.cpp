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
#include "TicketGridSortPure.h" // IsTrackerDateOrDateTimeField
#include "TrackerHttpUtils.h"
#include "Ui/BulkFieldEditPure.h"
#include "Ui/GridPastePure.h" // IsFreeValueField
#include "UiPerfMonitor.h"

#include "imgui.h"
#include <algorithm>
#include <deque>
#include <exception>
#include <functional>
#include <map>
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

// --- Grid undo (docs/plans/shipped/input-ux-w3-grid-spreadsheet.md, slice S3) ---
// Every grid write is recorded in `UiDrawSession::gridUndo` when it is enqueued, the commit result marks the change
// landed or failed, and an undo queues the inverse edit through the same pipeline.

using smatchet::grid::undo::CellChange;
using smatchet::grid::undo::ChangeState;

// The cell's display value as the pane's ticket shows it now. Read after the local apply, it is the value we wrote
// and so the ADR-0016 conflict base of the inverse edit. Empty for a cleared field (or an issue that is gone).
std::string DisplayOf(AppController& app, const PendingActionTarget& target, const std::string& issueId,
                      const std::string& fieldId, bool* found = nullptr) {
    if (found != nullptr) {
        *found = false;
    }
    const std::shared_ptr<const std::vector<CachedTicket>> tickets = app.TicketsSnapshotForTarget(target);
    if (!tickets) {
        return std::string();
    }
    for (const CachedTicket& ticket : *tickets) {
        if (ticket.id == issueId) {
            if (found != nullptr) {
                *found = true;
            }
            return ticket.GetFieldValue(fieldId);
        }
    }
    return std::string();
}

std::string DisplayNow(AppController& app, const PendingFieldEdit& edit) {
    return DisplayOf(app, edit.Target, edit.IssueId, edit.Field.Id);
}

// Forgets the context of every entry that has left the history (taken, emptied by failures or pushed out).
void PruneUndoContext(UiDrawSession& d) {
    for (std::map<std::uint64_t, GridUndoContext>::iterator it = d.gridUndoContext.begin();
         it != d.gridUndoContext.end();) {
        if (d.gridUndo.Contains(it->first)) {
            ++it;
        } else {
            it = d.gridUndoContext.erase(it);
        }
    }
}

// The commit result of one recorded edit: the change landed (saved online or queued offline) or it did not.
void SettleUndoChange(AppController& app, UiDrawSession& d, const PendingFieldEdit& edit, bool landed) {
    if (edit.UndoEntryId == 0) {
        return;
    }
    // The enqueue path has no controller; the first commit result does, and the toast's Undo needs it.
    const std::map<std::uint64_t, GridUndoContext>::iterator ctx = d.gridUndoContext.find(edit.UndoEntryId);
    if (ctx != d.gridUndoContext.end() && ctx->second.App == nullptr) {
        ctx->second.App = &app;
    }
    if (landed) {
        d.gridUndo.MarkLanded(edit.UndoEntryId, edit.IssueId, edit.Field.Id, DisplayNow(app, edit));
    } else {
        d.gridUndo.MarkFailed(edit.UndoEntryId, edit.IssueId, edit.Field.Id);
    }
    PruneUndoContext(d);
}

// An edit that never reached the tracker (superseded in the queue, discarded, never started): its change is dropped.
void DropUndoChange(UiDrawSession& d, const PendingFieldEdit& edit) {
    if (edit.UndoEntryId != 0) {
        d.gridUndo.MarkFailed(edit.UndoEntryId, edit.IssueId, edit.Field.Id);
        PruneUndoContext(d);
    }
}

// The values that put a cell back: option ids for a choice field, the label strings for labels, the displayed text for
// a free-value field (text, number, duration), which is what its editor wrote.
std::vector<std::string> UndoBeforeValues(const TrackerField& field, const std::string& display) {
    const smatchet::grid::bulk::BulkFieldKind kind = smatchet::grid::bulk::ClassifyBulkField(field);
    if (kind == smatchet::grid::bulk::BulkFieldKind::Unsupported) {
        return std::vector<std::string>{display};
    }
    return smatchet::grid::bulk::BeforeValuesFor(field, kind, display);
}

// Which single edits the history records: a scalar write with a captured base, made in a pane, that is not itself an
// undo or a bulk row (a bulk job records one entry for all its rows). Dates are left out (their display form is not
// the form the editor writes). A field of an Unsupported kind is recorded only when it is a free value (text, number,
// duration): a user, status or select field whose option list is filled lazily is not, its display name is not the
// value the tracker takes.
bool IsRecordable(const PendingFieldEdit& edit) {
    if (!edit.HasOriginalValue || edit.IsUndo || edit.BulkJobId != 0 || edit.UndoEntryId != 0 ||
        edit.Target.PaneId.empty()) {
        return false;
    }
    if (IsTrackerDateOrDateTimeField(edit.Field.Id, &edit.Field)) {
        return false;
    }
    return smatchet::grid::bulk::ClassifyBulkField(edit.Field) != smatchet::grid::bulk::BulkFieldKind::Unsupported ||
           smatchet::grid::paste::IsFreeValueField(edit.Field);
}

// Pushes the history entry of each recordable edit and stamps its id on the edit.
void RecordUndoEntries(UiDrawSession& d, std::vector<PendingFieldEdit>& edits) {
    for (PendingFieldEdit& edit : edits) {
        if (!IsRecordable(edit)) {
            continue;
        }
        smatchet::grid::undo::Entry entry;
        entry.PaneId = edit.Target.PaneId;
        CellChange change;
        change.IssueId = edit.IssueId;
        change.FieldId = edit.Field.Id;
        change.Before = UndoBeforeValues(edit.Field, edit.OriginalValue);
        change.After = edit.Values;
        change.BeforeDisplay = edit.OriginalValue;
        entry.Changes.push_back(std::move(change));
        edit.UndoEntryId = d.gridUndo.Push(std::move(entry));
        GridUndoContext context;
        context.Target = edit.Target;
        context.Field = edit.Field;
        d.gridUndoContext[edit.UndoEntryId] = std::move(context);
    }
    PruneUndoContext(d);
}

// ADR-0016 on the online path: the tracker call itself does not compare the base, so an undo drops every change whose
// cell no longer shows the value we wrote (someone else, or the user, changed it since) and says so, instead of
// overwriting it. The rest of the entry still undoes. A row that has left the pane's result set cannot be compared and
// is kept (the queue's own base check still guards the offline path). A display text a later sync rewrote (trimmed,
// reformatted) reads as a change: it fails safe, with the undo refused rather than wrong.
std::vector<CellChange> FilterUndoConflicts(AppController& app, const GridUndoContext& context,
                                            std::vector<CellChange> inverse) {
    std::vector<CellChange> kept;
    std::size_t conflicts = 0;
    for (CellChange& change : inverse) {
        bool found = false;
        const std::string shown = DisplayOf(app, context.Target, change.IssueId, change.FieldId, &found);
        if (!found || shown == change.BeforeDisplay) {
            kept.push_back(std::move(change));
        } else {
            ++conflicts;
        }
    }
    if (conflicts != 0) {
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("grid.undo.conflict_title", "Not undone"),
            SmatchetLocalization::T("grid.undo.conflict_message",
                                    "The value changed since it was saved, so it was left as it is."),
            ToastType::Warning);
    }
    return kept;
}

// The inverse of a taken entry, queued as ordinary edits: one edit for a single cell, one per-row bulk job for a
// bulk entry (each row gets its own Before back). Each inverse carries the value we wrote as its conflict base, so a
// field someone else changed meanwhile reports a conflict instead of being overwritten.
void RunInverse(AppController& app, UiDrawSession& d, const smatchet::grid::undo::Entry& entry) {
    const std::map<std::uint64_t, GridUndoContext>::iterator ctxIt = d.gridUndoContext.find(entry.Id);
    if (ctxIt == d.gridUndoContext.end()) {
        return;
    }
    const GridUndoContext context = ctxIt->second;
    d.gridUndoContext.erase(ctxIt);
    const std::vector<CellChange> inverse = FilterUndoConflicts(app, context, smatchet::grid::undo::Inverse(entry));
    if (inverse.empty()) {
        return;
    }
    if (entry.BulkJobId == 0) {
        std::vector<PendingFieldEdit> edits;
        for (const CellChange& change : inverse) {
            PendingFieldEdit edit;
            edit.IssueId = change.IssueId;
            edit.Field = context.Field;
            edit.Values = change.After;
            edit.OriginalValue = change.BeforeDisplay;
            edit.HasOriginalValue = true;
            edit.Target = context.Target;
            edit.IsUndo = true;
            edits.push_back(std::move(edit));
        }
        EnqueueGridFieldEdits(d, edits, false);
        return;
    }
    std::vector<smatchet::grid::bulk::BulkRowInput> rows;
    for (const CellChange& change : inverse) {
        smatchet::grid::bulk::BulkRowInput row;
        row.IssueId = change.IssueId;
        row.Values = change.After;
        row.ConflictBase = change.BeforeDisplay;
        row.HasConflictBase = true;
        rows.push_back(std::move(row));
    }
    StartBulkFieldEdit(app, d, entry.PaneId, context.Field, rows, false, true);
}

// The save succeeded: for a recorded single edit the toast carries Undo (replacing the plain "saved" toast), for an
// undo's own commit it says so, and a bulk row stays quiet (its job's summary toast reports).
void PushSavedToast(AppController& app, UiDrawSession& d, const PendingFieldEdit& edit, bool bulkRow) {
    if (bulkRow) {
        return;
    }
    if (edit.IsUndo) {
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("grid.undo.done_title", "Undone"),
            SmatchetLocalization::T("grid.undo.done_message", "The previous value was restored."), ToastType::Success);
        return;
    }
    const std::string title = SmatchetLocalization::T("toast.success", "Success");
    const std::string message = SmatchetLocalization::T("toast.field_update_saved", "Field update saved to Tracker.");
    if (edit.UndoEntryId == 0 || !d.gridUndo.Contains(edit.UndoEntryId)) {
        SmatchetToastManager::Instance().Push(title, message, ToastType::Success);
        return;
    }
    const std::uint64_t entryId = edit.UndoEntryId;
    UiDrawSession* session = &d;
    AppController* controller = &app;
    SmatchetToastManager::Instance().PushUndoable(
        title, message, 6000, [session, controller, entryId]() { UndoGridEntryById(*controller, *session, entryId); },
        std::function<void()>());
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
            SettleUndoChange(app, d, edit, false);
        } else {
            SettleUndoChange(app, d, edit, true);
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
            SettleUndoChange(app, d, edit, false);
        } else {
            SettleUndoChange(app, d, edit, true);
            PushSavedToast(app, d, edit, bulkRow);
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
        // The value the user typed, so reopening the cell puts it back in the editor with the message (#31).
        feedback.RejectedValue = bulkRow || edit.Values.empty() ? std::string() : edit.Values.front();
        d.cellFeedbackByKey[editKey] = feedback;
        SettleUndoChange(app, d, edit, false);
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
        DropUndoChange(d, *it);
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
        } else {
            DropUndoChange(d, incoming[i]);
            if (incoming[i].BulkJobId == 0) {
                continue;
            }
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
            // A recordable edit gets its history entry here, the one choke point every grid write crosses; the
            // stamped copy is what is queued.
            std::vector<PendingFieldEdit> recorded;
            const bool anyRecordable = std::any_of(pendingEdits.begin(), pendingEdits.end(), IsRecordable);
            if (anyRecordable) {
                recorded = pendingEdits;
                RecordUndoEntries(d, recorded);
            }
            QueueLatestPerCell(d, anyRecordable ? recorded : pendingEdits);
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
                DropUndoChange(d, edit);
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
                                            [&d](const PendingFieldEdit& edit) {
                                                const bool unbound = edit.Target.PaneId.empty();
                                                if (unbound) {
                                                    DropUndoChange(d, edit);
                                                }
                                                return unbound;
                                            }),
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
            DropUndoChange(d, edit);
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
    const std::string title =
        job.IsUndo ? SmatchetLocalization::Format("grid.undo.bulk_done_title", "Undone: %s", fieldName.c_str())
                   : SmatchetLocalization::Format("grid.bulk.done_title", "Bulk edit: %s", fieldName.c_str());
    const ToastType type = smatchet::grid::bulk::BulkSummaryToastType(summary);
    if (failedIds.empty()) {
        // A clean first-time job offers Undo on its toast; an undo's own job and a job whose entry is gone do not.
        const std::map<std::uint64_t, GridUndoContext>::const_iterator ctx = d.gridUndoContext.find(job.UndoEntryId);
        if (!job.IsUndo && job.UndoEntryId != 0 && d.gridUndo.Contains(job.UndoEntryId) &&
            ctx != d.gridUndoContext.end() && ctx->second.App != nullptr) {
            UiDrawSession* session = &d;
            AppController* controller = ctx->second.App;
            const std::uint64_t entryId = job.UndoEntryId;
            SmatchetToastManager::Instance().PushUndoable(
                title, message, 6000,
                [session, controller, entryId]() { UndoGridEntryById(*controller, *session, entryId); },
                std::function<void()>());
            return;
        }
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

namespace {

using smatchet::grid::bulk::BulkRowInput;

// The one preparation both forms share: one row per distinct input id, each with its own values.
BulkFieldEditJob PrepareJobCore(AppController& app, const std::string& paneId, const TrackerField& field,
                                const std::vector<BulkRowInput>& inputs, const std::vector<std::string>& sharedValues,
                                smatchet::grid::bulk::BulkMode mode, std::vector<std::vector<std::string>>* rowValues) {
    BulkFieldEditJob job;
    job.PaneId = paneId;
    job.Field = field;
    job.Values = sharedValues;
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
    for (const BulkRowInput& input : inputs) {
        const std::string& issueId = input.IssueId;
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
                newValues = smatchet::grid::bulk::NewValuesForRow(kind, mode, row.BeforeValues, input.Values);
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

// The shared form: every row gets the same `values`.
std::vector<BulkRowInput> SharedRowInputs(const std::vector<std::string>& issueIds,
                                          const std::vector<std::string>& values) {
    std::vector<BulkRowInput> inputs;
    inputs.reserve(issueIds.size());
    for (const std::string& issueId : issueIds) {
        BulkRowInput input;
        input.IssueId = issueId;
        input.Values = values;
        inputs.push_back(std::move(input));
    }
    return inputs;
}

// One history entry for the whole job: each row that is sent is one change, and every row's edit points at the entry
// so its commit result lands on it. Dates are left out, as for a single edit (IsRecordable).
void RecordBulkUndoEntry(AppController& app, UiDrawSession& d, BulkFieldEditJob& job, const PendingActionTarget& target,
                         const std::vector<std::vector<std::string>>& rowValues, std::vector<PendingFieldEdit>& edits) {
    if (IsTrackerDateOrDateTimeField(job.Field.Id, &job.Field)) {
        return;
    }
    smatchet::grid::undo::Entry entry;
    entry.PaneId = job.PaneId;
    entry.BulkJobId = job.Id;
    for (std::size_t i = 0; i < job.Rows.size(); ++i) {
        if (job.Rows[i].State != BulkRowState::Pending) {
            continue;
        }
        smatchet::grid::undo::CellChange change;
        change.IssueId = job.Rows[i].IssueId;
        change.FieldId = job.Field.Id;
        change.Before = UndoBeforeValues(job.Field, job.Rows[i].BeforeValue); // an empty text restores as ""
        change.After = rowValues[i];
        change.BeforeDisplay = job.Rows[i].BeforeValue;
        entry.Changes.push_back(std::move(change));
    }
    const std::uint64_t entryId = d.gridUndo.Push(std::move(entry));
    GridUndoContext context;
    context.Target = target;
    context.Field = job.Field;
    context.App = &app;
    d.gridUndoContext[entryId] = std::move(context);
    job.UndoEntryId = entryId;
    for (PendingFieldEdit& edit : edits) {
        edit.UndoEntryId = entryId;
    }
    PruneUndoContext(d);
}

// Keeps a whole-row selection by issue id while the job's rows land: the first landed edit bumps the snapshot
// revision, which moves the sort-order row indices (SmatchetActiveProjectGridTable.cpp remaps them from these ids).
// With no rows selected (a cell range, one cell) nothing is added.
void KeepJobSelection(UiDrawSession& d, const std::string& paneId, const std::vector<BulkRowInput>& inputs) {
    GridPane* pane = FindGridPaneById(d.gridPanes, paneId);
    if (pane == nullptr || pane->gridState.RectSel.Rows.empty()) {
        return;
    }
    GridRectSelection& sel = pane->gridState.RectSel;
    sel.KeepIssueIds.clear();
    for (const BulkRowInput& input : inputs) {
        sel.KeepIssueIds.insert(input.IssueId);
    }
    sel.KeepRows = sel.Rows;
}

// `sharedSelection`: the shared-value form, whose rows are the selection it was started from.
std::uint64_t StartJobCore(AppController& app, UiDrawSession& d, const std::string& paneId, const TrackerField& field,
                           const std::vector<BulkRowInput>& inputs, const std::vector<std::string>& sharedValues,
                           smatchet::grid::bulk::BulkMode mode, bool readOnlyMode, bool isUndo, bool sharedSelection) {
    const std::string fieldName = field.Name.empty() ? field.Id : field.Name;
    const std::string title = SmatchetLocalization::Format("grid.bulk.done_title", "Bulk edit: %s", fieldName.c_str());
    if (readOnlyMode) {
        SmatchetToastManager::Instance().Push(
            title, SmatchetLocalization::T("grid.bulk.read_only", "Nothing was changed: the grid is read-only."),
            ToastType::Warning);
        return 0;
    }

    std::vector<std::vector<std::string>> rowValues;
    BulkFieldEditJob job = PrepareJobCore(app, paneId, field, inputs, sharedValues, mode, &rowValues);
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
    job.IsUndo = isUndo;
    const PendingActionTarget target = app.LatchPendingActionTargetForPane(paneId);
    std::unordered_map<std::string, const BulkRowInput*> inputById;
    inputById.reserve(inputs.size());
    for (const BulkRowInput& input : inputs) {
        inputById.emplace(input.IssueId, &input);
    }
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
        // else changed the value raises a conflict instead of overwriting it. A row that brings its own base (an
        // undo: the value we wrote) keeps it, whatever the cell shows now.
        const std::unordered_map<std::string, const BulkRowInput*>::const_iterator input = inputById.find(row.IssueId);
        if (input != inputById.end() && input->second->HasConflictBase) {
            edit.OriginalValue = input->second->ConflictBase;
            edit.HasOriginalValue = true;
        } else {
            smatchet::grid::bulk::CaptureScalarBase(row.BeforeValue, edit);
        }
        edit.Target = target;
        edit.BulkJobId = job.Id;
        edits.push_back(std::move(edit));
    }

    if (!isUndo) { // an undo's own job records no history
        RecordBulkUndoEntry(app, d, job, target, rowValues, edits);
    }
    if (sharedSelection) {
        KeepJobSelection(d, paneId, inputs);
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

} // namespace

std::uint64_t StartBulkFieldEdit(AppController& app, UiDrawSession& d, const std::string& paneId,
                                 const TrackerField& field, const std::vector<std::string>& issueIds,
                                 const std::vector<std::string>& values, smatchet::grid::bulk::BulkMode mode,
                                 bool readOnlyMode) {
    return StartJobCore(app, d, paneId, field, SharedRowInputs(issueIds, values), values, mode, readOnlyMode, false,
                        true);
}

std::uint64_t StartBulkFieldEdit(AppController& app, UiDrawSession& d, const std::string& paneId,
                                 const TrackerField& field, const std::vector<smatchet::grid::bulk::BulkRowInput>& rows,
                                 bool readOnlyMode, bool isUndo) {
    return StartJobCore(app, d, paneId, field, rows, std::vector<std::string>(),
                        smatchet::grid::bulk::BulkMode::Replace, readOnlyMode, isUndo, false);
}

smatchet::grid::bulk::BulkFieldEditJob
PrepareBulkFieldEditJob(AppController& app, const std::string& paneId, const TrackerField& field,
                        const std::vector<std::string>& issueIds, const std::vector<std::string>& values,
                        smatchet::grid::bulk::BulkMode mode, std::vector<std::vector<std::string>>* rowValues) {
    return PrepareJobCore(app, paneId, field, SharedRowInputs(issueIds, values), values, mode, rowValues);
}

smatchet::grid::bulk::BulkFieldEditJob
PrepareBulkFieldEditJob(AppController& app, const std::string& paneId, const TrackerField& field,
                        const std::vector<smatchet::grid::bulk::BulkRowInput>& rows,
                        std::vector<std::vector<std::string>>* rowValues) {
    return PrepareJobCore(app, paneId, field, rows, std::vector<std::string>(), smatchet::grid::bulk::BulkMode::Replace,
                          rowValues);
}

bool QueueSingleCellFieldEdit(AppController& app, UiDrawSession& d, const std::string& paneId,
                              const TrackerField& field, const smatchet::grid::bulk::BulkRowInput& row,
                              bool readOnlyMode) {
    std::vector<std::vector<std::string>> rowValues;
    const BulkFieldEditJob job =
        PrepareBulkFieldEditJob(app, paneId, field, std::vector<BulkRowInput>{row}, &rowValues);
    if (job.Rows.size() != 1 || rowValues.size() != 1 || job.Rows.front().State != BulkRowState::Pending) {
        return false;
    }
    PendingFieldEdit edit;
    edit.IssueId = job.Rows.front().IssueId;
    edit.Field = field;
    edit.Values = rowValues.front();
    smatchet::grid::bulk::CaptureScalarBase(job.Rows.front().BeforeValue, edit);
    edit.Target = app.LatchPendingActionTargetForPane(paneId);
    EnqueueGridFieldEdits(d, std::vector<PendingFieldEdit>{edit}, readOnlyMode);
    return true;
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

// Read-only mode writes nothing, an undo included: the entry stays in the history, and the user is told why.
static bool UndoBlockedByReadOnly(const UiDrawSession& d) {
    if (!d.cfg.ReadOnlyMode) {
        return false;
    }
    SmatchetToastManager::Instance().Push(
        SmatchetLocalization::T("grid.undo.readonly_title", "Read-only mode"),
        SmatchetLocalization::T("grid.undo.readonly_message",
                                "Nothing is written in read-only mode, so nothing was undone."),
        ToastType::Info);
    return true;
}

void UndoGridEntryById(AppController& app, UiDrawSession& d, std::uint64_t entryId) {
    if (UndoBlockedByReadOnly(d)) {
        return;
    }
    smatchet::grid::undo::Entry entry;
    if (!d.gridUndo.Take(entryId, entry)) {
        return; // already undone, or pushed out of the bounded history
    }
    RunInverse(app, d, entry);
}

bool UndoNewestGridEntry(AppController& app, UiDrawSession& d, const std::string& paneId) {
    if (UndoBlockedByReadOnly(d)) {
        return false;
    }
    smatchet::grid::undo::Entry entry;
    switch (d.gridUndo.TakeNewest(paneId, entry)) {
    case smatchet::grid::undo::History::TakeResult::Taken:
        RunInverse(app, d, entry);
        return true;
    case smatchet::grid::undo::History::TakeResult::StillSaving:
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("grid.undo.saving_title", "Still saving"),
            SmatchetLocalization::T("grid.undo.saving_message", "Try again when the change has landed."),
            ToastType::Info);
        return false;
    case smatchet::grid::undo::History::TakeResult::Empty:
        break;
    }
    SmatchetToastManager::Instance().Push(
        SmatchetLocalization::T("grid.undo.empty_title", "Nothing to undo"),
        SmatchetLocalization::T("grid.undo.empty_message", "No grid edit to undo in this pane."), ToastType::Info);
    return false;
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
