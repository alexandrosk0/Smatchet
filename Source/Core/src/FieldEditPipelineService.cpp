#include "FieldEditPipelineService.h"

#include "TrackerFieldPayloadPure.h"

#include "EditMetaCacheService.h"         // editMeta_ ref: Ensure/CanEdit/Refresh editmeta checks
#include "IssueTransitionsCacheService.h" // transitions_ ref: invalidate after status edits
#include "IFieldEditDeps.h"
#include "ITrackerBackend.h"
#include "ITrackerIssueMutations.h"
#include "ITrackerIssueReader.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "FieldEditAuditSource.h"

#include "Logger.h"
#include "OfflineFirstPure.h" // RouteWrite (CommitOrQueue)
#include "SmatchetLocalization.h"
#include "StringUtil.h" // TruncateForLog, ToLowerAsciiCopy, TrimCopy
#include "TrackerFieldSchema.h"
#include "TrackerFieldValueUtils.h"

namespace {

bool IsEditableTimetrackingEstimateFieldId(const std::string& fieldId) {
    return TrackerFieldValueUtils::IsEditableTimetrackingEstimateFieldId(fieldId);
}

bool IsNonEditableTimetrackingFieldId(const std::string& fieldId) {
    return TrackerFieldValueUtils::IsNonEditableTimetrackingFieldId(fieldId);
}

bool ErrorTextContainsHttpStatus(const std::string& errorText, int statusCode) {
    if (statusCode < 100 || statusCode > 599) {
        return false;
    }
    const std::string needle = "HTTP " + std::to_string(statusCode);
    return errorText.find(needle) != std::string::npos;
}

// The sprint an edit names: an option id or name, or a bare numeric id.
Result<std::string> ResolveEditedSprintId(const TrackerField& field, const std::vector<std::string>& values) {
    if (values.empty()) {
        return Result<std::string>::Err("Clearing sprint is not supported by this action.");
    }
    std::string sprintId = TrackerFieldPayloadPure::ResolveSprintIdForAgile(field, values.front());
    if (sprintId.empty()) {
        return Result<std::string>::Err("Unknown sprint: " + values.front());
    }
    return Result<std::string>::Ok(std::move(sprintId));
}

// A sprint's display label: its option name, else the id.
std::string SprintDisplayValue(const TrackerField& field, const std::string& sprintId) {
    const TrackerFieldOption* option = TrackerFieldPayloadPure::FindOptionById(field.AllowedValueOptions, sprintId);
    return option != nullptr ? option->Value : sprintId;
}

// Both estimates are sent together, so both displays change together.
void PutEstimateDisplays(const TrackerFieldPayloadPure::TimetrackingEstimateEdit& edit,
                         std::unordered_map<std::string, std::string>& displays) {
    displays["timeoriginalestimate"] = edit.OriginalEstimate;
    displays["timeestimate"] = edit.RemainingEstimate;
}

Result<TrackerFieldPayloadPure::TimetrackingEstimateEdit> BuildEstimateEdit(const FieldEditCommitRequest& req,
                                                                            const std::vector<std::string>& values) {
    return TrackerFieldPayloadPure::BuildTimetrackingEstimateEdit(
        req.Field.Id, values.empty() ? std::string() : values.front(), req.OriginalEstimateSnapshot,
        req.RemainingEstimateSnapshot);
}

} // namespace

FieldEditPipelineService::FieldEditPipelineService(IFieldEditDeps& deps, EditMetaCacheService& editMeta,
                                                   IssueTransitionsCacheService& transitions)
    : deps_(deps), editMeta_(editMeta), transitions_(transitions) {}

bool FieldEditPipelineService::FieldEditSupportsOfflineQueue(const TrackerField& field) {
    // Sprint and estimate edits queue in their own payload shapes (TrackerFieldPayloadPure); the
    // derived, worklog-backed time fields cannot be edited at all.
    if (TrackerFieldPayloadPure::IsSprintField(field) || IsEditableTimetrackingEstimateFieldId(field.Id)) {
        return true;
    }
    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        return false;
    }
    switch (field.Family) {
    case TrackerFieldFamily::Text:
    case TrackerFieldFamily::Number:
    case TrackerFieldFamily::Date:
    case TrackerFieldFamily::DateTime:
    case TrackerFieldFamily::Labels:
    case TrackerFieldFamily::SelectSingle:
    case TrackerFieldFamily::SelectMulti:
    case TrackerFieldFamily::UserSingle:
    case TrackerFieldFamily::UserMulti:
    case TrackerFieldFamily::Status:
    case TrackerFieldFamily::CascadingSelect:
        return true;
    default:
        return false;
    }
}

void FieldEditPipelineService::CaptureTicketSnapshots(const CachedTicket& ticket, bool captureBase,
                                                      FieldEditCommitRequest& req) {
    req.OriginalEstimateSnapshot = ticket.GetFieldValue("timeoriginalestimate");
    req.RemainingEstimateSnapshot = ticket.GetFieldValue("timeestimate");
    req.IssueTypeKeySnapshot = ToLowerAsciiCopy(TrimCopy(ticket.GetFieldValue("issuetype")));
    if (!captureBase) {
        return;
    }
    std::string rich = ticket.GetFieldRichValue(req.Field.Id);
    if (!rich.empty()) {
        req.OriginalRichValue = std::move(rich);
        req.OriginalValue.clear();
        req.HasOriginalValue = false;
        return;
    }
    req.OriginalRichValue.clear();
    req.OriginalValue = ticket.GetFieldValue(req.Field.Id);
    req.HasOriginalValue = true; // a captured base, even when blank (ADR-0016)
}

PendingActionTarget FieldEditPipelineService::BindTarget(const PendingActionTarget& target) const {
    if (!target.PaneId.empty()) {
        return target;
    }
    PendingActionTarget focused = deps_.LatchFocusedPaneTarget();
    focused.Connectivity = target.Connectivity;
    return focused;
}

bool FieldEditPipelineService::TryBuildFieldEditPayloadForNetwork(
    const FieldEditCommitRequest& req, const std::shared_ptr<ITrackerBackend>& backend,
    nlohmann::json& outFieldsPayload, std::unordered_map<std::string, std::string>& outDisplayValues,
    std::string& outError) {
    const std::string& issueId = req.IssueId;
    const TrackerField& field = req.Field;
    outError.clear();
    outDisplayValues.clear();
    outFieldsPayload = nlohmann::json::object();
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        return false;
    }
    if (!backend) {
        outError = "Tracker backend is not initialized.";
        return false;
    }
    if (TrackerFieldPayloadPure::IsSprintField(field) || IsNonEditableTimetrackingFieldId(field.Id) ||
        IsEditableTimetrackingEstimateFieldId(field.Id)) {
        outError = "Field type not supported for this edit path.";
        return false;
    }

    // No editmeta fetch here: the network path loads it first, while a queued edit never waits on the
    // network (Pillar 6), so this check uses whatever is loaded and is optimistic otherwise.
    if (!editMeta_.CanEditFieldForIssueWithType(issueId, field.Id, &field, req.IssueTypeKeySnapshot)) {
        outError = "Field cannot be edited for this issue (Jira edit metadata).";
        return false;
    }

    nlohmann::json valuePayload;
    bool built = false;
    if (backend->Mutations()) {
        auto payloadResult = backend->Mutations()->BuildFieldPayload(field, req.Values);
        if (payloadResult) {
            valuePayload = std::move(payloadResult.value());
            built = true;
        } else {
            outError = payloadResult.error().Detail;
        }
    }
    if (!built) {
        LOG_WARN("FieldEditPipelineService::TryBuildFieldEditPayloadForNetwork build failed issue=%s field=%s err=%s",
                 issueId.c_str(), field.Id.c_str(), outError.c_str());
        return false;
    }

    outFieldsPayload = std::move(valuePayload);

    std::string displayValue;
    const std::vector<std::string> values = TrackerFieldPayloadPure::NonEmptyValues(req.Values);
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            displayValue += ", ";
        }
        displayValue += backend->Reader().ResolveDisplayValue(field.Id, &field, values[i]);
    }
    outDisplayValues[field.Id] = std::move(displayValue);
    return true;
}

FieldEditResult FieldEditPipelineService::SubmitFieldEditNetworkOnly(const FieldEditCommitRequest& req,
                                                                     const PendingActionTarget& target) {
    // The FieldEditResult IS the outcome: on failure it carries Error + ErrorTransient, which
    // CommitOrQueue reads to decide the offline-queue fallback.
    FieldEditResult result;
    LOG_TRACE("SubmitFieldEditNetworkOnly: source=%s issue=%s field=%s raw_values=%zu", FieldEditAuditSource::Current(),
              req.IssueId.c_str(), req.Field.Id.c_str(), req.Values.size());
    if (req.IssueId.empty()) {
        result.Error = "Issue id is empty.";
        return result;
    }
    if (!target.Backend) {
        result.Error = "No tracker backend initialized.";
        return result;
    }
    ITrackerIssueMutations* const mutations = target.Backend->Mutations();
    if (!mutations) {
        result.Error = "Tracker backend does not support issue mutations.";
        return result;
    }
    const std::vector<std::string> values = TrackerFieldPayloadPure::NonEmptyValues(req.Values);
    if (TrackerFieldPayloadPure::IsSprintField(req.Field)) {
        SubmitSprintFieldEditNetworkOnly(req, values, *mutations, result);
        return result;
    }
    if (IsNonEditableTimetrackingFieldId(req.Field.Id)) {
        result.Error = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        return result;
    }
    if (IsEditableTimetrackingEstimateFieldId(req.Field.Id)) {
        SubmitTimetrackingFieldEditNetworkOnly(req, values, *mutations, result);
        return result;
    }

    // Sprint and timetracking returned above; this field is permission-checked against the editmeta of
    // the edit's own backend.
    editMeta_.EnsureIssueEditMetaLoadedFor(target.Backend, req.IssueId, req.IssueTypeKeySnapshot);

    nlohmann::json fieldsPayload;
    std::unordered_map<std::string, std::string> displayValues;
    if (!TryBuildFieldEditPayloadForNetwork(req, target.Backend, fieldsPayload, displayValues, result.Error)) {
        return result;
    }
    if (!ApplyFieldUpdateWithEditMetaRetry(req, target, fieldsPayload, *mutations, result)) {
        return result;
    }

    result.Ok = true;
    result.UpdatedDisplayValues = std::move(displayValues);
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return result;
}

bool FieldEditPipelineService::ApplyFieldUpdateWithEditMetaRetry(const FieldEditCommitRequest& req,
                                                                 const PendingActionTarget& target,
                                                                 const nlohmann::json& fieldsPayload,
                                                                 ITrackerIssueMutations& mutations,
                                                                 FieldEditResult& outResult) {
    const std::string& issueId = req.IssueId;
    const TrackerField& field = req.Field;
    TrackerError updateErr = mutations.UpdateIssueFields(issueId, fieldsPayload);
    outResult.Error = updateErr.Detail;
    outResult.ErrorTransient = updateErr.IsRetryable();
    bool updateOk = updateErr.IsOk();
    bool didRetryAfter400 = false;
    if (!updateOk && ErrorTextContainsHttpStatus(outResult.Error, 400)) {
        didRetryAfter400 = true;
        editMeta_.RefreshIssueEditMetaFor(target.Backend, issueId, req.IssueTypeKeySnapshot);
        if (!editMeta_.CanEditFieldForIssueWithType(issueId, field.Id, &field, req.IssueTypeKeySnapshot)) {
            outResult.Error =
                "Field cannot be edited for this issue (Jira edit metadata refreshed after validation failure).";
            outResult.ErrorTransient = false;
            LOG_WARN("FieldEditPipelineService::SubmitFieldEditNetworkOnly blocked after editmeta refresh issue=%s "
                     "field=%s",
                     issueId.c_str(), field.Id.c_str());
            return false;
        }
        updateErr = mutations.UpdateIssueFields(issueId, fieldsPayload);
        outResult.Error = updateErr.Detail;
        outResult.ErrorTransient = updateErr.IsRetryable();
        updateOk = updateErr.IsOk();
    }
    if (!updateOk) {
        std::string payloadForLog;
        try {
            payloadForLog = fieldsPayload.dump();
        } catch (...) { // catch-all-ok: best-effort payload dump for the adjacent LOG_ERROR; fallback string on any
                        // json dump failure
            payloadForLog = "(payload dump failed)";
        }
        LOG_ERROR("FieldEditPipelineService::SubmitFieldEditNetworkOnly failed issue=%s field=%s retried_after_400=%d "
                  "tracker_error=%s request=%s",
                  issueId.c_str(), field.Id.c_str(), didRetryAfter400 ? 1 : 0, outResult.Error.c_str(),
                  TruncateForLog(payloadForLog, 1200).c_str());
        return false;
    }
    return true;
}

bool FieldEditPipelineService::SubmitSprintFieldEditNetworkOnly(const FieldEditCommitRequest& req,
                                                                const std::vector<std::string>& values,
                                                                ITrackerIssueMutations& mutations,
                                                                FieldEditResult& outResult) {
    const Result<std::string> sprintId = ResolveEditedSprintId(req.Field, values);
    if (!sprintId) {
        outResult.Error = sprintId.error();
        return false;
    }
    const TrackerError sprintErr = mutations.AddIssueToSprint(req.IssueId, sprintId.value());
    if (!sprintErr.IsOk()) {
        outResult.Error = sprintErr.Detail;
        outResult.ErrorTransient = sprintErr.IsRetryable();
        return false;
    }
    outResult.Ok = true;
    outResult.UpdatedDisplayValues[req.Field.Id] = SprintDisplayValue(req.Field, sprintId.value());
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return true;
}

bool FieldEditPipelineService::SubmitTimetrackingFieldEditNetworkOnly(const FieldEditCommitRequest& req,
                                                                      const std::vector<std::string>& values,
                                                                      ITrackerIssueMutations& mutations,
                                                                      FieldEditResult& outResult) {
    const Result<TrackerFieldPayloadPure::TimetrackingEstimateEdit> edit = BuildEstimateEdit(req, values);
    if (!edit) {
        outResult.Error = edit.error();
        return false;
    }
    const TrackerError updateErr = mutations.UpdateIssueFields(req.IssueId, edit.value().FieldsPayload);
    if (!updateErr.IsOk()) {
        outResult.Error = updateErr.Detail;
        outResult.ErrorTransient = updateErr.IsRetryable();
        return false;
    }
    outResult.Ok = true;
    PutEstimateDisplays(edit.value(), outResult.UpdatedDisplayValues);
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return true;
}

bool FieldEditPipelineService::TryPrepareOfflineFieldEdit(const FieldEditCommitRequest& req,
                                                          const PendingActionTarget& target, FieldEditResult& outResult,
                                                          std::string& outFieldsPayloadJson, std::string& outError) {
    outResult = FieldEditResult{};
    outFieldsPayloadJson.clear();
    outError.clear();
    nlohmann::json fieldsPayload;
    std::unordered_map<std::string, std::string> displayValues;
    const std::vector<std::string> values = TrackerFieldPayloadPure::NonEmptyValues(req.Values);
    if (TrackerFieldPayloadPure::IsSprintField(req.Field)) {
        // Replay sends {"sprint_add": id} with AddIssueToSprint, as the live edit does.
        const Result<std::string> sprintId = ResolveEditedSprintId(req.Field, values);
        if (!sprintId) {
            outError = sprintId.error();
            return false;
        }
        fieldsPayload = TrackerFieldPayloadPure::MakeSprintAddPayload(sprintId.value());
        displayValues[req.Field.Id] = SprintDisplayValue(req.Field, sprintId.value());
    } else if (IsEditableTimetrackingEstimateFieldId(req.Field.Id)) {
        Result<TrackerFieldPayloadPure::TimetrackingEstimateEdit> edit = BuildEstimateEdit(req, values);
        if (!edit) {
            outError = edit.error();
            return false;
        }
        PutEstimateDisplays(edit.value(), displayValues);
        fieldsPayload = std::move(edit.value().FieldsPayload);
    } else if (!TryBuildFieldEditPayloadForNetwork(req, target.Backend, fieldsPayload, displayValues, outError)) {
        return false;
    }
    try {
        outFieldsPayloadJson = fieldsPayload.dump();
    } catch (const std::exception& ex) {
        outError = ex.what();
        return false;
    } catch (...) {
        LOG_WARN("BuildFieldEditPayload: unknown exception serializing field payload");
        outError = "Failed to serialize field payload.";
        return false;
    }
    outResult.Ok = true;
    outResult.UpdatedDisplayValues = std::move(displayValues);
    return true;
}

FieldEditCommitOutcome FieldEditPipelineService::CommitOrQueue(const FieldEditCommitRequest& req) {
    const PendingActionTarget target = BindTarget(req.Target);
    const std::string auditOp = BackendAuditTrail::MakeOperationId("field-edit");
    const char* const auditSource = FieldEditAuditSource::Current();
    BackendAuditTrail::AppendBegin("field_edit_diff", auditSource, req.IssueId, auditOp,
                                   nlohmann::json{{"field_id", req.Field.Id}, {"backend_key", target.BackendKey}});
    const FieldEditCommitOutcome out = CommitOrQueueBound(req, target);
    BackendAuditTrail::AppendResult("field_edit_diff", auditSource, req.IssueId, auditOp,
                                    out.Kind != FieldEditCommitKind::Failed, out.Error,
                                    nlohmann::json{{"field_id", req.Field.Id},
                                                   {"before", req.OriginalValue},
                                                   {"after", req.Values},
                                                   {"queued", out.Kind == FieldEditCommitKind::QueuedOffline}});
    return out;
}

FieldEditCommitOutcome FieldEditPipelineService::CommitOrQueueBound(const FieldEditCommitRequest& req,
                                                                    const PendingActionTarget& target) {
    using smatchet::offline::WriteRoute;
    const bool queueable = FieldEditSupportsOfflineQueue(req.Field);
    const WriteRoute route =
        smatchet::offline::RouteWrite(target.Connectivity, queueable, ConfigManager::Load().ReadOnlyMode);
    if (route == WriteRoute::Reject) {
        FieldEditCommitOutcome rejected;
        rejected.Error = "Read-only mode is enabled in Preferences.";
        return rejected;
    }
    if (route == WriteRoute::QueueImmediately) {
        LOG_DEBUG("FieldEditPipelineService::CommitOrQueue tracker offline; queueing issue=%s field=%s",
                  req.IssueId.c_str(), req.Field.Id.c_str());
        return QueuePreparedEdit(req, target, false);
    }

    FieldEditCommitOutcome out;
    out.Apply = SubmitFieldEditNetworkOnly(req, target);
    if (out.Apply.Ok) {
        out.Kind = FieldEditCommitKind::SavedOnline;
        return out;
    }
    out.Error = out.Apply.Error;
    // Only a retryable failure (transport / 5xx / rate limit) falls back to the queue; a rejection
    // the user must act on (auth, validation) is reported, never queued to fail again on replay.
    if (!out.Apply.ErrorTransient || !queueable) {
        return out;
    }
    FieldEditCommitOutcome queued = QueuePreparedEdit(req, target, true);
    if (queued.Kind == FieldEditCommitKind::QueuedOffline) {
        return queued;
    }
    if (!queued.Error.empty()) {
        out.Error = queued.Error; // the edit is lost unless the user retries: say why it was not queued
    }
    return out;
}

FieldEditCommitOutcome FieldEditPipelineService::QueuePreparedEdit(const FieldEditCommitRequest& req,
                                                                   const PendingActionTarget& target,
                                                                   bool afterTransportFailure) {
    FieldEditCommitOutcome out;
    if (target.BackendKey.empty()) {
        // No queue namespace means replay could never pick the row up: refuse rather than orphan it.
        out.Error = "This edit is not tied to a tracker, so it cannot be saved offline.";
        return out;
    }
    FieldEditResult prepared;
    std::string payloadJson;
    if (!TryPrepareOfflineFieldEdit(req, target, prepared, payloadJson, out.Error)) {
        return out;
    }
    const std::int64_t queueId =
        deps_.EnqueueOfflineFieldEdit(target.BackendKey, req.IssueId, req.Field.Id, payloadJson, req.OriginalRichValue,
                                      req.OriginalValue, req.HasOriginalValue, out.Error);
    if (queueId <= 0) {
        if (out.Error.empty()) {
            out.Error = SmatchetLocalization::T("toast.offline_queue_failed", "Failed to queue offline field edit.");
        }
        return out;
    }
    out.Kind = FieldEditCommitKind::QueuedOffline;
    out.Apply = std::move(prepared);
    out.QueueId = queueId;
    out.QueuedAfterTransportFailure = afterTransportFailure;
    out.Error.clear();
    return out;
}

VoidResult FieldEditPipelineService::ApplyFieldEditResult(const PendingActionTarget& target, const std::string& issueId,
                                                          const FieldEditResult& result) {
    if (!result.Ok) {
        return VoidResult::Err(result.Error.empty() ? std::string("Failed to save field update.") : result.Error);
    }
    if (!deps_.HasCache()) {
        return VoidResult::Err(SmatchetLocalization::T(
            "fieldedit.cache_unavailable", "Local cache is unavailable, so this edit cannot be applied. Restart "
                                           "Smatchet or check Settings -> Preferences -> Local data."));
    }
    if (issueId.empty()) {
        return VoidResult::Err("Issue id is empty.");
    }
    const PendingActionTarget bound = BindTarget(target);

    // Invalidate cached transitions if a status field was updated (must come before cache update).
    if (result.UpdatedDisplayValues.count("status") != 0) {
        transitions_.InvalidateIssueTransitions(bound.BackendKey, issueId);
    }

    const std::shared_ptr<const std::vector<CachedTicket>> tickets = deps_.TicketsSnapshotFor(bound);
    if (!tickets) {
        // Saved or queued all the same; that pane's next sync shows the new value.
        LOG_INFO("FieldEditPipelineService::ApplyFieldEditResult pane '%s' was closed or switched tracker; "
                 "issue=%s is not updated locally",
                 bound.PaneId.c_str(), issueId.c_str());
        return VoidOk();
    }
    const auto ticketIt = std::find_if(tickets->begin(), tickets->end(),
                                       [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (ticketIt == tickets->end()) {
        return VoidOk(); // not a row this pane shows: nothing to update in place
    }

    CachedTicket updatedTicket = *ticketIt;
    for (const auto& pair : result.UpdatedDisplayValues) {
        updatedTicket.fieldValues[pair.first] = pair.second;
    }
    deps_.UpdateTicketFor(bound, updatedTicket);
    return VoidOk();
}
