#include "FieldEditPipelineService.h"

#include "EditMetaCacheService.h" // editMeta_ ref: Ensure/CanEdit/Refresh editmeta checks
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
#include <vector>

#include <nlohmann/json.hpp>

#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "FieldEditAuditSource.h"

#include "Logger.h"
#include "SmatchetLocalization.h"
#include "StringUtil.h" // TruncateForLog
#include "TrackerFieldSchema.h"
#include "TrackerFieldValueUtils.h"

namespace {

// COPIED (internal linkage) from AppController_CatalogAndFieldEdit.cpp — also copied in
// EditMetaCacheService.cpp. Anonymous-namespace internal linkage in each TU → no ODR clash.
bool IsSprintField(const TrackerField& field) {
    return field.Family == TrackerFieldFamily::Sprint || field.SchemaCustom.find("gh-sprint") != std::string::npos;
}

bool IsEditableTimetrackingEstimateFieldId(const std::string& fieldId) {
    return TrackerFieldValueUtils::IsEditableTimetrackingEstimateFieldId(fieldId);
}

// MOVED (sole survivors of the field-edit pipeline) from AppController_CatalogAndFieldEdit.cpp.
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

} // namespace

FieldEditPipelineService::FieldEditPipelineService(IFieldEditDeps& deps, EditMetaCacheService& editMeta)
    : deps_(deps), editMeta_(editMeta) {}

bool FieldEditPipelineService::FieldEditSupportsOfflineQueue(const TrackerField& field) {
    if (IsSprintField(field)) {
        return false;
    }
    if (IsNonEditableTimetrackingFieldId(field.Id) || IsEditableTimetrackingEstimateFieldId(field.Id)) {
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
    case TrackerFieldFamily::CascadingSelect:
        return true;
    default:
        return false;
    }
}

bool FieldEditPipelineService::TryBuildFieldEditPayloadForNetwork(
    const std::string& issueId, const TrackerField& field, const std::vector<std::string>& rawValues,
    const std::string& originalEstimateSnapshot, const std::string& remainingEstimateSnapshot,
    const std::string& issueTypeKeySnapshot, nlohmann::json& outFieldsPayload,
    std::unordered_map<std::string, std::string>& outDisplayValues, std::string& outError) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    (void)originalEstimateSnapshot;
    (void)remainingEstimateSnapshot;
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
    if (IsSprintField(field) || IsNonEditableTimetrackingFieldId(field.Id) ||
        IsEditableTimetrackingEstimateFieldId(field.Id)) {
        outError = "Field type not supported for this edit path.";
        return false;
    }

    std::vector<std::string> values;
    values.reserve(rawValues.size());
    std::copy_if(rawValues.begin(), rawValues.end(), std::back_inserter(values),
                 [](const std::string& value) { return !value.empty(); });

    const std::string* issueTypeKeyOpt = issueTypeKeySnapshot.empty() ? nullptr : &issueTypeKeySnapshot;
    if (!IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id)) {
        editMeta_.EnsureIssueEditMetaLoaded(issueId, nullptr, issueTypeKeyOpt);
    }
    if (!IsSprintField(field) && !IsEditableTimetrackingEstimateFieldId(field.Id) &&
        !editMeta_.CanEditFieldForIssue(issueId, field.Id, &field, issueTypeKeyOpt)) {
        outError = "Field cannot be edited for this issue (Jira edit metadata).";
        return false;
    }

    nlohmann::json valuePayload;
    bool built = false;
    if (backend->Mutations()) {
        auto payloadResult = backend->Mutations()->BuildFieldPayload(field, rawValues);
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
    if (!values.empty()) {
        for (size_t i = 0; i < values.size(); ++i) {
            if (i != 0) {
                displayValue += ", ";
            }
            displayValue += backend->Reader().ResolveDisplayValue(field.Id, &field, values[i]);
        }
    }
    outDisplayValues[field.Id] = std::move(displayValue);
    return true;
}

bool FieldEditPipelineService::SubmitFieldEditSprint(const SubmitFieldEditCtx& ctx, std::string& outError) {
    const std::string& issueId = ctx.issueId;
    const TrackerField& field = ctx.field;
    const auto& values = ctx.values;
    const auto& tickets = *ctx.ticketsSnap;
    ITrackerIssueMutations* mutations = ctx.mutations;
    const std::string& fieldEditAuditOp = ctx.fieldEditAuditOp;
    const char* const fieldEditAuditSource = ctx.fieldEditAuditSource;

    if (values.empty()) {
        outError = "Clearing sprint is not supported by this action.";
        LOG_WARN("FieldEditPipelineService::SubmitFieldEdit sprint clear not supported issue=%s field=%s",
                 issueId.c_str(), field.Id.c_str());
        return false;
    }
    const std::string sprintId = values.front();
    auto ticketIt =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    BackendAuditTrail::AppendBegin("field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp,
                                   nlohmann::json{{"field_id", field.Id}, {"kind", "sprint"}});
    const TrackerError sprintErr = mutations->AddIssueToSprint(issueId, sprintId);
    outError = sprintErr.Detail;
    if (!sprintErr.IsOk()) {
        LOG_ERROR("FieldEditPipelineService::SubmitFieldEdit sprint update failed issue=%s field=%s sprint=%s err=%s",
                  issueId.c_str(), field.Id.c_str(), sprintId.c_str(), outError.c_str());
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, false, outError,
            nlohmann::json{{"field_id", field.Id},
                           {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                           {"after", sprintId}});
        return false;
    }
    if (ticketIt != tickets.end()) {
        CachedTicket updatedTicket = *ticketIt;
        std::string displayValue = sprintId;
        auto optIt = std::find_if(field.AllowedValueOptions.begin(), field.AllowedValueOptions.end(),
                                  [&](const auto& option) { return option.Id == sprintId; });
        if (optIt != field.AllowedValueOptions.end()) {
            displayValue = optIt->Value;
        }
        updatedTicket.fieldValues[field.Id] = displayValue;
        deps_.UpdateTicket(updatedTicket);
    } else {
        deps_.RefreshLocalData();
    }
    BackendAuditTrail::AppendResult(
        "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
        nlohmann::json{{"field_id", field.Id},
                       {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                       {"after", sprintId}});
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return true;
}

bool FieldEditPipelineService::SubmitFieldEditTimetracking(const SubmitFieldEditCtx& ctx, std::string& outError) {
    const std::string& issueId = ctx.issueId;
    const TrackerField& field = ctx.field;
    const auto& values = ctx.values;
    const auto& tickets = *ctx.ticketsSnap;
    ITrackerIssueMutations* mutations = ctx.mutations;
    const std::string& fieldEditAuditOp = ctx.fieldEditAuditOp;
    const char* const fieldEditAuditSource = ctx.fieldEditAuditSource;

    const std::string editedValue = values.empty() ? std::string() : values.front();
    if (editedValue.empty()) {
        outError = "Clearing Jira timetracking estimates is not supported by this editor.";
        LOG_WARN("FieldEditPipelineService::SubmitFieldEdit blocked timetracking clear issue=%s field=%s",
                 issueId.c_str(), field.Id.c_str());
        return false;
    }

    auto ticketIt =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });

    std::string originalEstimate =
        (ticketIt != tickets.end()) ? ticketIt->GetFieldValue("timeoriginalestimate") : std::string();
    std::string remainingEstimate =
        (ticketIt != tickets.end()) ? ticketIt->GetFieldValue("timeestimate") : std::string();
    const std::string beforeOriginalEstimate = originalEstimate;
    const std::string beforeRemainingEstimate = remainingEstimate;
    if (field.Id == "timeoriginalestimate") {
        originalEstimate = editedValue;
    } else {
        remainingEstimate = editedValue;
    }

    nlohmann::json timetrackingPayload = nlohmann::json::object();
    if (!originalEstimate.empty()) {
        timetrackingPayload["originalEstimate"] = originalEstimate;
    }
    if (!remainingEstimate.empty()) {
        timetrackingPayload["remainingEstimate"] = remainingEstimate;
    }
    if (timetrackingPayload.empty()) {
        outError = "Timetracking update requires at least one estimate value.";
        return false;
    }

    nlohmann::json fieldsPayload = nlohmann::json::object();
    fieldsPayload["timetracking"] = std::move(timetrackingPayload);
    BackendAuditTrail::AppendBegin("field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp,
                                   nlohmann::json{{"field_id", "timetracking"}, {"kind", "timetracking"}});
    const TrackerError timetrackingErr = mutations->UpdateIssueFields(issueId, fieldsPayload);
    outError = timetrackingErr.Detail;
    if (!timetrackingErr.IsOk()) {
        std::string payloadForLog;
        try {
            payloadForLog = fieldsPayload.dump();
        } catch (...) { // catch-all-ok: dump for logging
            payloadForLog = "(payload dump failed)";
        }
        LOG_ERROR("FieldEditPipelineService::SubmitFieldEdit failed issue=%s field=%s tracker_error=%s request=%s",
                  issueId.c_str(), field.Id.c_str(), outError.c_str(), TruncateForLog(payloadForLog, 1200).c_str());
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, false, outError,
            nlohmann::json{{"field_id", "timetracking"},
                           {"before", nlohmann::json{{"timeoriginalestimate", beforeOriginalEstimate},
                                                     {"timeestimate", beforeRemainingEstimate}}},
                           {"after", fieldsPayload["timetracking"]}});
        return false;
    }

    if (ticketIt != tickets.end()) {
        CachedTicket updatedTicket = *ticketIt;
        updatedTicket.fieldValues["timeoriginalestimate"] = originalEstimate;
        updatedTicket.fieldValues["timeestimate"] = remainingEstimate;
        deps_.UpdateTicket(updatedTicket);
    } else {
        deps_.RefreshLocalData();
    }
    BackendAuditTrail::AppendResult(
        "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
        nlohmann::json{{"field_id", "timetracking"},
                       {"before", nlohmann::json{{"timeoriginalestimate", beforeOriginalEstimate},
                                                 {"timeestimate", beforeRemainingEstimate}}},
                       {"after", fieldsPayload["timetracking"]}});
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return true;
}

bool FieldEditPipelineService::SubmitFieldEditRegular(const SubmitFieldEditCtx& ctx, std::string& outError) {
    const std::string& issueId = ctx.issueId;
    const TrackerField& field = ctx.field;
    const auto& rawValues = ctx.rawValues;
    const auto& values = ctx.values;
    const auto& tickets = *ctx.ticketsSnap;
    ITrackerIssueMutations* mutations = ctx.mutations;
    const std::shared_ptr<ITrackerBackend>& backend = ctx.backend;
    const std::string& fieldEditAuditOp = ctx.fieldEditAuditOp;
    const char* const fieldEditAuditSource = ctx.fieldEditAuditSource;

    editMeta_.EnsureIssueEditMetaLoaded(issueId, nullptr);

    if (!editMeta_.CanEditFieldForIssue(issueId, field.Id, &field)) {
        outError = "Field cannot be edited for this issue (Jira edit metadata).";
        LOG_WARN("FieldEditPipelineService::SubmitFieldEdit blocked by editmeta issue=%s field=%s", issueId.c_str(),
                 field.Id.c_str());
        return false;
    }

    auto fieldPayloadResult = mutations->BuildFieldPayload(field, rawValues);
    if (!fieldPayloadResult) {
        outError = fieldPayloadResult.error().Detail;
        LOG_WARN("FieldEditPipelineService::SubmitFieldEdit invalid value issue=%s field=%s err=%s", issueId.c_str(),
                 field.Id.c_str(), outError.c_str());
        return false;
    }
    nlohmann::json fieldsPayload = std::move(fieldPayloadResult.value());

    auto ticketIt =
        std::find_if(tickets.begin(), tickets.end(), [&](const CachedTicket& ticket) { return ticket.id == issueId; });

    BackendAuditTrail::AppendBegin("field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp,
                                   nlohmann::json{{"field_id", field.Id}, {"kind", "issue_fields"}});
    TrackerError updateErr = mutations->UpdateIssueFields(issueId, fieldsPayload);
    outError = updateErr.Detail;
    bool updateOk = updateErr.IsOk();
    bool didRetryAfter400 = false;
    if (!updateOk && ErrorTextContainsHttpStatus(outError, 400)) {
        didRetryAfter400 = true;
        editMeta_.RefreshIssueEditMeta(issueId, nullptr);
        if (!editMeta_.CanEditFieldForIssue(issueId, field.Id, &field)) {
            outError = "Field cannot be edited for this issue (Jira edit metadata refreshed after validation failure).";
            LOG_WARN("FieldEditPipelineService::SubmitFieldEdit blocked after editmeta refresh issue=%s field=%s",
                     issueId.c_str(), field.Id.c_str());
            BackendAuditTrail::AppendResult(
                "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, false, outError,
                nlohmann::json{
                    {"field_id", field.Id},
                    {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                    {"after", rawValues}});
            return false;
        }
        updateErr = mutations->UpdateIssueFields(issueId, fieldsPayload);
        outError = updateErr.Detail;
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
        LOG_ERROR("FieldEditPipelineService::SubmitFieldEdit failed issue=%s field=%s retried_after_400=%d "
                  "tracker_error=%s request=%s",
                  issueId.c_str(), field.Id.c_str(), didRetryAfter400 ? 1 : 0, outError.c_str(),
                  TruncateForLog(payloadForLog, 1200).c_str());
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, false, outError,
            nlohmann::json{{"field_id", field.Id},
                           {"before", ticketIt != tickets.end() ? ticketIt->GetFieldValue(field.Id) : std::string()},
                           {"after", rawValues}});
        return false;
    }

    // Keep local cache and in-memory model in sync with the successful backend update.
    if (ticketIt != tickets.end()) {
        CachedTicket updatedTicket = *ticketIt;

        std::string displayValue;
        if (!values.empty()) {
            for (size_t i = 0; i < values.size(); ++i) {
                if (i != 0) {
                    displayValue += ", ";
                }
                displayValue += backend->Reader().ResolveDisplayValue(field.Id, &field, values[i]);
            }
        }

        updatedTicket.fieldValues[field.Id] = displayValue;
        deps_.UpdateTicket(updatedTicket);
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
            nlohmann::json{
                {"field_id", field.Id}, {"before", ticketIt->GetFieldValue(field.Id)}, {"after", displayValue}});
    } else {
        deps_.RefreshLocalData();
        BackendAuditTrail::AppendResult(
            "field_edit_diff", fieldEditAuditSource, issueId, fieldEditAuditOp, true, std::string(),
            nlohmann::json{{"field_id", field.Id}, {"before", "unknown"}, {"after", rawValues}});
    }

    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return true;
}

bool FieldEditPipelineService::SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                                               const std::vector<std::string>& rawValues, std::string& outError) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        LOG_WARN("FieldEditPipelineService::SubmitFieldEdit blocked by read-only mode issue=%s field=%s",
                 issueId.c_str(), field.Id.c_str());
        return false;
    }
    if (!backend || !deps_.HasCache()) {
        outError = "Backend or cache is not initialized.";
        LOG_WARN("FieldEditPipelineService::SubmitFieldEdit skipped issue=%s field=%s: %s", issueId.c_str(),
                 field.Id.c_str(), outError.c_str());
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        LOG_WARN("FieldEditPipelineService::SubmitFieldEdit skipped field=%s: %s", field.Id.c_str(), outError.c_str());
        return false;
    }

    ITrackerIssueMutations* const mutations = backend->Mutations();
    if (!mutations) {
        outError = "Tracker backend does not support issue mutations.";
        return false;
    }

    const std::string fieldEditAuditOp = BackendAuditTrail::MakeOperationId("field-edit");
    const char* const fieldEditAuditSource = FieldEditAuditSource::Current();
    LOG_TRACE("SubmitFieldEdit: source=%s issue=%s field=%s raw_values=%zu", fieldEditAuditSource, issueId.c_str(),
              field.Id.c_str(), rawValues.size());

    std::vector<std::string> values;
    values.reserve(rawValues.size());
    std::copy_if(rawValues.begin(), rawValues.end(), std::back_inserter(values),
                 [](const std::string& value) { return !value.empty(); });

    const std::shared_ptr<const std::vector<CachedTicket>> ticketsSnap = deps_.GetActiveTicketsSnapshot();

    const SubmitFieldEditCtx ctx{
        issueId, field, rawValues, values, mutations, backend, ticketsSnap, fieldEditAuditOp, fieldEditAuditSource};

    if (IsSprintField(field)) {
        return SubmitFieldEditSprint(ctx, outError);
    }

    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        outError = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        LOG_WARN("FieldEditPipelineService::SubmitFieldEdit blocked non-editable timetracking issue=%s field=%s",
                 issueId.c_str(), field.Id.c_str());
        return false;
    }

    if (IsEditableTimetrackingEstimateFieldId(field.Id)) {
        return SubmitFieldEditTimetracking(ctx, outError);
    }

    return SubmitFieldEditRegular(ctx, outError);
}

bool FieldEditPipelineService::SubmitFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                                          const std::vector<std::string>& rawValues,
                                                          const std::string& originalEstimateSnapshot,
                                                          const std::string& remainingEstimateSnapshot,
                                                          const std::string& issueTypeKeySnapshot,
                                                          FieldEditResult& outResult) {
    std::shared_ptr<ITrackerBackend> backend = deps_.BackendShared();
    outResult = FieldEditResult{};
    LOG_TRACE("SubmitFieldEditNetworkOnly: source=%s issue=%s field=%s raw_values=%zu", FieldEditAuditSource::Current(),
              issueId.c_str(), field.Id.c_str(), rawValues.size());
    if (ConfigManager::Load().ReadOnlyMode) {
        outResult.Error = "Read-only mode is enabled in Preferences.";
        LOG_WARN("FieldEditPipelineService::SubmitFieldEditNetworkOnly blocked by read-only mode issue=%s field=%s",
                 issueId.c_str(), field.Id.c_str());
        return false;
    }
    if (issueId.empty()) {
        outResult.Error = "Issue id is empty.";
        return false;
    }

    if (!backend) {
        outResult.Error = "No tracker backend initialized.";
        return false;
    }
    ITrackerIssueMutations* const mutations = backend->Mutations();
    if (!mutations) {
        outResult.Error = "Tracker backend does not support issue mutations.";
        return false;
    }
    std::vector<std::string> values;
    values.reserve(rawValues.size());
    std::copy_if(rawValues.begin(), rawValues.end(), std::back_inserter(values),
                 [](const std::string& value) { return !value.empty(); });

    if (IsSprintField(field)) {
        bool handled = false;
        const bool ok = SubmitSprintFieldEditNetworkOnly(issueId, field, values, *mutations, outResult, handled);
        if (handled) {
            return ok;
        }
    }

    if (IsNonEditableTimetrackingFieldId(field.Id)) {
        outResult.Error = "This Jira time field is derived or worklog-backed and cannot be edited directly.";
        return false;
    }

    if (IsEditableTimetrackingEstimateFieldId(field.Id)) {
        bool handled = false;
        const bool ok =
            SubmitTimetrackingFieldEditNetworkOnly(issueId, field, values, originalEstimateSnapshot,
                                                   remainingEstimateSnapshot, *mutations, outResult, handled);
        if (handled) {
            return ok;
        }
    }

    const std::string* issueTypeKeyOpt = issueTypeKeySnapshot.empty() ? nullptr : &issueTypeKeySnapshot;

    nlohmann::json fieldsPayload;
    std::unordered_map<std::string, std::string> displayValues;
    if (!TryBuildFieldEditPayloadForNetwork(issueId, field, rawValues, originalEstimateSnapshot,
                                            remainingEstimateSnapshot, issueTypeKeySnapshot, fieldsPayload,
                                            displayValues, outResult.Error)) {
        return false;
    }

    if (!ApplyFieldUpdateWithEditMetaRetry(issueId, field, fieldsPayload, issueTypeKeyOpt, *mutations, outResult)) {
        return false;
    }

    outResult.Ok = true;
    outResult.UpdatedDisplayValues = std::move(displayValues);
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return true;
}

bool FieldEditPipelineService::ApplyFieldUpdateWithEditMetaRetry(const std::string& issueId, const TrackerField& field,
                                                                 const nlohmann::json& fieldsPayload,
                                                                 const std::string* issueTypeKeyOpt,
                                                                 ITrackerIssueMutations& mutations,
                                                                 FieldEditResult& outResult) {
    TrackerError updateErr = mutations.UpdateIssueFields(issueId, fieldsPayload);
    outResult.Error = updateErr.Detail;
    bool updateOk = updateErr.IsOk();
    bool didRetryAfter400 = false;
    if (!updateOk && ErrorTextContainsHttpStatus(outResult.Error, 400)) {
        didRetryAfter400 = true;
        editMeta_.RefreshIssueEditMeta(issueId, nullptr, issueTypeKeyOpt);
        if (!editMeta_.CanEditFieldForIssue(issueId, field.Id, &field, issueTypeKeyOpt)) {
            outResult.Error =
                "Field cannot be edited for this issue (Jira edit metadata refreshed after validation failure).";
            LOG_WARN("FieldEditPipelineService::SubmitFieldEditNetworkOnly blocked after editmeta refresh issue=%s "
                     "field=%s",
                     issueId.c_str(), field.Id.c_str());
            return false;
        }
        updateErr = mutations.UpdateIssueFields(issueId, fieldsPayload);
        outResult.Error = updateErr.Detail;
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

bool FieldEditPipelineService::SubmitSprintFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                                                const std::vector<std::string>& values,
                                                                ITrackerIssueMutations& mutations,
                                                                FieldEditResult& outResult, bool& handled) {
    handled = true;
    if (values.empty()) {
        outResult.Error = "Clearing sprint is not supported by this action.";
        return false;
    }
    const std::string sprintId = values.front();
    const TrackerError sprintErr = mutations.AddIssueToSprint(issueId, sprintId);
    if (!sprintErr.IsOk()) {
        outResult.Error = sprintErr.Detail;
        return false;
    }
    std::string displayValue = sprintId;
    auto optIt = std::find_if(field.AllowedValueOptions.begin(), field.AllowedValueOptions.end(),
                              [&](const auto& option) { return option.Id == sprintId; });
    if (optIt != field.AllowedValueOptions.end()) {
        displayValue = optIt->Value;
    }
    outResult.Ok = true;
    outResult.UpdatedDisplayValues[field.Id] = std::move(displayValue);
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return true;
}

bool FieldEditPipelineService::SubmitTimetrackingFieldEditNetworkOnly(
    const std::string& issueId, const TrackerField& field, const std::vector<std::string>& values,
    const std::string& originalEstimateSnapshot, const std::string& remainingEstimateSnapshot,
    ITrackerIssueMutations& mutations, FieldEditResult& outResult, bool& handled) {
    handled = true;
    const std::string editedValue = values.empty() ? std::string() : values.front();
    if (editedValue.empty()) {
        outResult.Error = "Clearing Jira timetracking estimates is not supported by this editor.";
        return false;
    }
    std::string originalEstimate = originalEstimateSnapshot;
    std::string remainingEstimate = remainingEstimateSnapshot;
    if (field.Id == "timeoriginalestimate") {
        originalEstimate = editedValue;
    } else {
        remainingEstimate = editedValue;
    }

    nlohmann::json timetrackingPayload = nlohmann::json::object();
    if (!originalEstimate.empty()) {
        timetrackingPayload["originalEstimate"] = originalEstimate;
    }
    if (!remainingEstimate.empty()) {
        timetrackingPayload["remainingEstimate"] = remainingEstimate;
    }
    if (timetrackingPayload.empty()) {
        outResult.Error = "Timetracking update requires at least one estimate value.";
        return false;
    }

    nlohmann::json fieldsPayload = nlohmann::json::object();
    fieldsPayload["timetracking"] = std::move(timetrackingPayload);
    const TrackerError updateErr = mutations.UpdateIssueFields(issueId, fieldsPayload);
    if (!updateErr.IsOk()) {
        outResult.Error = updateErr.Detail;
        return false;
    }
    outResult.Ok = true;
    outResult.UpdatedDisplayValues["timeoriginalestimate"] = std::move(originalEstimate);
    outResult.UpdatedDisplayValues["timeestimate"] = std::move(remainingEstimate);
    deps_.RequestDeferredLiveTrackerBackendSuccessNotify();
    return true;
}

bool FieldEditPipelineService::TryPrepareOfflineFieldEdit(const std::string& issueId, const TrackerField& field,
                                                          const std::vector<std::string>& rawValues,
                                                          const std::string& originalEstimateSnapshot,
                                                          const std::string& remainingEstimateSnapshot,
                                                          const std::string& issueTypeKeySnapshot,
                                                          FieldEditResult& outResult, std::string& outFieldsPayloadJson,
                                                          std::string& outError) {
    outResult = FieldEditResult{};
    outFieldsPayloadJson.clear();
    outError.clear();
    if (ConfigManager::Load().ReadOnlyMode) {
        outError = "Read-only mode is enabled in Preferences.";
        return false;
    }
    nlohmann::json fieldsPayload;
    std::unordered_map<std::string, std::string> displayValues;
    if (!TryBuildFieldEditPayloadForNetwork(issueId, field, rawValues, originalEstimateSnapshot,
                                            remainingEstimateSnapshot, issueTypeKeySnapshot, fieldsPayload,
                                            displayValues, outError)) {
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

bool FieldEditPipelineService::ApplyFieldEditResult(const std::string& issueId, const FieldEditResult& result,
                                                    std::string& outError) {
    outError.clear();
    if (!result.Ok) {
        outError = result.Error.empty() ? std::string("Failed to save field update.") : result.Error;
        return false;
    }
    if (!deps_.HasCache()) {
        outError = SmatchetLocalization::T("fieldedit.cache_unavailable",
                                           "Local cache is unavailable, so this edit cannot be applied. Restart "
                                           "Smatchet or check Settings -> Preferences -> Local data.");
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        return false;
    }

    const auto ticketsSnapApply = deps_.GetActiveTicketsSnapshot();
    const auto& ticketsApply = *ticketsSnapApply;
    auto ticketIt = std::find_if(ticketsApply.begin(), ticketsApply.end(),
                                 [&](const CachedTicket& ticket) { return ticket.id == issueId; });
    if (ticketIt == ticketsApply.end()) {
        deps_.RefreshLocalData();
        return true;
    }

    CachedTicket updatedTicket = *ticketIt;
    for (const auto& pair : result.UpdatedDisplayValues) {
        updatedTicket.fieldValues[pair.first] = pair.second;
    }
    deps_.UpdateTicket(updatedTicket);
    return true;
}
