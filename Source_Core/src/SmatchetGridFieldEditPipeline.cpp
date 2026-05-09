#include "SmatchetGridUiSupport.h"

#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetUiSession.h"
#include "SmatchetToast.h"
#include "StringUtil.h"
#include "TrackerHttpUtils.h"

#include "imgui.h"
#include <algorithm>
#include <string>
#include <vector>

void ProcessGridFieldEdits(AppController& app, UiDrawSession& d,
                           const std::vector<CachedTicket>& tickets,
                           const std::vector<PendingFieldEdit>& pendingEdits,
                           bool readOnlyMode) {
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

    {
        if (!readOnlyMode && !d.hasInFlightEdit && !d.queuedFieldEdits.empty()) {
            d.inFlightEdit = d.queuedFieldEdits.front();
            d.queuedFieldEdits.pop_front();
            d.inFlightOriginalEstimateSnapshot.clear();
            d.inFlightRemainingEstimateSnapshot.clear();
            d.inFlightIssueTypeKeySnapshot.clear();
            const auto snapshotIt = std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) {
                return ticket.id == d.inFlightEdit.IssueId;
            });
            if (snapshotIt != tickets.end()) {
                d.inFlightOriginalEstimateSnapshot = snapshotIt->GetFieldValue("timeoriginalestimate");
                d.inFlightRemainingEstimateSnapshot = snapshotIt->GetFieldValue("timeestimate");
                d.inFlightIssueTypeKeySnapshot = ToLowerAsciiCopy(TrimCopy(snapshotIt->GetFieldValue("issuetype")));
            }
            d.hasInFlightEdit = true;
            d.inFlightDelayFrames = 1;
            CellWriteFeedback feedback;
            feedback.State = CellWriteState::Saving;
            feedback.Message = "Saving to Tracker...";
            feedback.FramesRemaining = 0;
            d.cellFeedbackByKey[BuildCellKey(d.inFlightEdit.IssueId, d.inFlightEdit.Field.Id)] = feedback;
        }

        if (d.hasInFlightEdit) {
            if (d.inFlightDelayFrames > 0) {
                --d.inFlightDelayFrames;
            } else {
                const PendingFieldEdit edit = d.inFlightEdit;
                const std::string originalEstimateSnapshot = d.inFlightOriginalEstimateSnapshot;
                const std::string remainingEstimateSnapshot = d.inFlightRemainingEstimateSnapshot;
                const std::string issueTypeKeyForNetwork = d.inFlightIssueTypeKeySnapshot;
                const std::string editKey = BuildCellKey(edit.IssueId, edit.Field.Id);

                FieldEditCommitResult result;
                result.CommitKind = FieldEditCommitResult::Kind::Failed;
                if (app.SubmitFieldEditNetworkOnly(edit.IssueId, edit.Field, edit.Values, originalEstimateSnapshot,
                                                   remainingEstimateSnapshot, issueTypeKeyForNetwork,
                                                   result.ApplyResult)) {
                    result.Ok = true;
                    result.CommitKind = FieldEditCommitResult::Kind::SavedOnline;
                    result.Error.clear();
                } else {
                    result.Error = result.ApplyResult.Error;
                    if (IsTrackerTransportErrorText(result.ApplyResult.Error) &&
                        AppController::FieldEditSupportsOfflineQueue(edit.Field)) {
                        AppController::FieldEditResult prepared;
                        std::string payloadJson;
                        std::string prepErr;
                        if (app.TryPrepareOfflineFieldEdit(edit.IssueId, edit.Field, edit.Values,
                                                           originalEstimateSnapshot, remainingEstimateSnapshot,
                                                           issueTypeKeyForNetwork, prepared, payloadJson, prepErr)) {
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

                std::string applyError;
                if (result.CommitKind == FieldEditCommitResult::Kind::QueuedOffline) {
                    std::string qerr;
                    const std::int64_t qid =
                        app.QueueFieldEditOffline(edit.IssueId, edit.Field.Id, result.QueuedFieldsPayloadJson, qerr,
                                                  edit.OriginalRichValue);
                    if (qid <= 0) {
                        SmatchetToastManager::Instance().Push(
                            "Offline Error", qerr.empty() ? "Failed to queue offline field edit." : qerr,
                            ToastType::Error);
                        CellWriteFeedback feedback;
                        feedback.State = CellWriteState::Error;
                        feedback.Message = qerr;
                        feedback.FramesRemaining = 0;
                        d.cellFeedbackByKey[editKey] = feedback;
                    } else if (!app.ApplyFieldEditResult(edit.IssueId, result.ApplyResult, applyError)) {
                        SmatchetToastManager::Instance().Push(
                            "Apply Error", applyError.empty() ? "Failed to apply queued field edit." : applyError,
                            ToastType::Error);
                        CellWriteFeedback feedback;
                        feedback.State = CellWriteState::Error;
                        feedback.Message = applyError;
                        feedback.FramesRemaining = 0;
                        d.cellFeedbackByKey[editKey] = feedback;
                    } else {
                        SmatchetToastManager::Instance().Push("Queued Offline",
                                                              "Field edit will sync when Tracker is reachable.",
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
                            "Save Error", applyError.empty() ? "Failed to apply saved field update." : applyError,
                            ToastType::Error);
                        CellWriteFeedback feedback;
                        feedback.State = CellWriteState::Error;
                        feedback.Message = applyError;
                        feedback.FramesRemaining = 0;
                        d.cellFeedbackByKey[editKey] = feedback;
                    } else {
                        SmatchetToastManager::Instance().Push("Success", "Field update saved to Tracker.",
                                                              ToastType::Success);
                        CellWriteFeedback feedback;
                        feedback.State = CellWriteState::Success;
                        feedback.Message = "Saved";
                        feedback.FramesRemaining = 180;
                        d.cellFeedbackByKey[editKey] = feedback;
                    }
                } else {
                    d.gridEditError =
                        result.Error.empty() ? std::string("Failed to save Tracker field update.") : result.Error;
                    d.gridEditSuccess.clear();
                    CellWriteFeedback feedback;
                    feedback.State = CellWriteState::Error;
                    feedback.Message = d.gridEditError;
                    feedback.FramesRemaining = 0;
                    d.cellFeedbackByKey[editKey] = feedback;
                }

                d.hasInFlightEdit = false;
            }
        }
    }

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

    if (!pendingEdits.empty()) {
        d.gridEditError.clear();
    }
}
