#include "IssueCreatePipeline.h"

#include "ITrackerClient.h"
#include "JiraFieldPayload.h"
#include "JiraTrackerFieldAdapter.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "StringUtil.h"

#include <algorithm>
#include <unordered_set>

namespace {

const TrackerField* FindFieldById(const std::vector<TrackerField>& catalog, const std::string& id) {
    for (const auto& field : catalog) {
        if (field.Id == id) {
            return &field;
        }
    }
    return nullptr;
}

/** Synthesize a minimal field for fields that aren't in the catalog (unlikely, but safe). */
TrackerField MakeSyntheticField(const std::string& id) {
    TrackerField f;
    f.Id = id;
    f.Type = "string";
    f.Family = TrackerFieldFamily::Text;
    return f;
}

/** PUT /issue fields: mutable custom + system fields; no project/type swap; status/sprint handled elsewhere. */
bool BuildUpdateFieldsPayload(const IssueDraft& draft, const std::vector<TrackerField>& catalog,
                              nlohmann::json& outFields, std::string& outError) {
    outFields = nlohmann::json::object();
    outError.clear();

    if (!draft.ParentKey.empty()) {
        outFields["parent"] = nlohmann::json{{"key", draft.ParentKey}};
    }

    for (const auto& kv : draft.FieldValues) {
        const std::string& fieldId = kv.first;
        if (fieldId.size() >= 2 && fieldId[0] == '_' && fieldId[1] == '_') {
            continue;
        }
        if (fieldId.empty() || IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) ||
            IssueDraftHelpers::IsSpecialDraftFieldId(fieldId) || fieldId == "project" || fieldId == "issuetype" ||
            fieldId == "parent") {
            continue;
        }
        if (fieldId == "status") {
            continue;
        }
        const std::string raw = kv.second;
        if (raw.empty()) {
            continue;
        }

        const TrackerField* field = FindFieldById(catalog, fieldId);
        TrackerField synthetic;
        if (!field) {
            synthetic = MakeSyntheticField(fieldId);
            field = &synthetic;
        }

        const JiraField jiraField = JiraTrackerFieldAdapter::ToJiraField(*field);
        if (JiraFieldPayload::IsSprintField(jiraField)) {
            continue;
        }

        nlohmann::json value;
        std::string err;
        if (!JiraFieldPayload::BuildValue(jiraField, {raw}, value, err)) {
            outError = "Field '" + fieldId + "': " + err;
            return false;
        }
        if (!value.is_null()) {
            outFields[fieldId] = std::move(value);
        }
    }
    return true;
}

struct PostIssueStepsOutcome {
    bool mergeStatusFromDraft = false;
    bool mergeSprintFieldsFromDraft = false;
};

bool IsSprintCatalogFieldId(const std::string& fieldId, const std::vector<TrackerField>& catalog) {
    if (fieldId == "status") {
        return false;
    }
    const TrackerField* sprintField = FindFieldById(catalog, fieldId);
    if (!sprintField) {
        return false;
    }
    return JiraFieldPayload::IsSprintField(JiraTrackerFieldAdapter::ToJiraField(*sprintField));
}

PostIssueStepsOutcome ApplyPostIssueSteps(ITrackerClient& client, const std::string& issueKey,
                                          const IssueDraft& draft, const std::vector<TrackerField>& catalog,
                                          IssueCreateResult& result) {
    PostIssueStepsOutcome outcome;
    const auto statusIt = draft.FieldValues.find("status");
    if (statusIt != draft.FieldValues.end()) {
        const std::string statusRaw = TrimCopy(statusIt->second);
        if (!statusRaw.empty()) {
            const TrackerField* statusField = FindFieldById(catalog, "status");
            TrackerField statusSynthetic;
            if (!statusField) {
                statusSynthetic = MakeSyntheticField("status");
                statusField = &statusSynthetic;
            }
            const JiraField jiraStatusField = JiraTrackerFieldAdapter::ToJiraField(*statusField);
            nlohmann::json statusValue;
            std::string statusBuildErr;
            if (JiraFieldPayload::BuildValue(jiraStatusField, {statusRaw}, statusValue, statusBuildErr) &&
                !statusValue.is_null()) {
                nlohmann::json statusUpdate = nlohmann::json::object();
                statusUpdate["status"] = std::move(statusValue);
                std::string transitionErr;
                if (client.UpdateIssueFields(issueKey, statusUpdate, transitionErr)) {
                    outcome.mergeStatusFromDraft = true;
                } else {
                    LOG_WARN("IssueCreatePipeline: issue %s: status not applied: %s", issueKey.c_str(),
                             transitionErr.c_str());
                }
            } else if (!statusBuildErr.empty()) {
                LOG_WARN("IssueCreatePipeline: issue %s: status payload invalid: %s", issueKey.c_str(),
                         statusBuildErr.c_str());
            }
        }
    }

    bool sprintWork = false;
    bool sprintAllOk = true;
    std::unordered_set<std::string> appliedSprintIds;
    for (const auto& kv : draft.FieldValues) {
        const std::string& fieldId = kv.first;
        if (fieldId.size() >= 2 && fieldId[0] == '_' && fieldId[1] == '_') {
            continue;
        }
        if (fieldId.empty() || IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) ||
            IssueDraftHelpers::IsSpecialDraftFieldId(fieldId) || fieldId == "project" || fieldId == "issuetype" ||
            fieldId == "parent") {
            continue;
        }
        const TrackerField* sprintField = FindFieldById(catalog, fieldId);
        if (sprintField == nullptr) {
            continue;
        }
        const JiraField jiraSprintField = JiraTrackerFieldAdapter::ToJiraField(*sprintField);
        if (!JiraFieldPayload::IsSprintField(jiraSprintField)) {
            continue;
        }
        const std::string raw = TrimCopy(kv.second);
        if (raw.empty()) {
            continue;
        }
        sprintWork = true;
        bool sprintSegmentSeen = false;
        for (const std::string& seg : JiraFieldPayload::SplitCommaSeparatedValues(raw)) {
            sprintSegmentSeen = true;
            const std::string sprintId = JiraFieldPayload::ResolveSprintIdForAgile(jiraSprintField, seg);
            if (sprintId.empty()) {
                sprintAllOk = false;
                LOG_WARN("IssueCreatePipeline: issue %s: sprint segment could not be resolved to id "
                         "(field %s, value '%s').",
                         issueKey.c_str(), fieldId.c_str(), seg.c_str());
                continue;
            }
            if (!appliedSprintIds.insert(sprintId).second) {
                continue;
            }
            std::string sprintErr;
            if (!client.AddIssueToSprint(issueKey, sprintId, sprintErr)) {
                sprintAllOk = false;
                LOG_WARN("IssueCreatePipeline: issue %s: AddIssueToSprint failed: %s", issueKey.c_str(),
                         sprintErr.c_str());
            }
        }
        if (!sprintSegmentSeen) {
            sprintAllOk = false;
        }
    }
    outcome.mergeSprintFieldsFromDraft = sprintWork && sprintAllOk;

    if (!draft.StagedAttachments.empty()) {
        std::vector<std::string> paths;
        paths.reserve(draft.StagedAttachments.size());
        for (const auto& a : draft.StagedAttachments) {
            if (!a.AbsPath.empty()) {
                paths.push_back(a.AbsPath);
            }
        }
        if (!paths.empty()) {
            std::string attachErr;
            client.AttachFilesToIssue(issueKey, paths, result.AttachmentFailures, attachErr);
            if (!result.AttachmentFailures.empty()) {
                LOG_WARN("IssueCreatePipeline: %zu attachment(s) failed for %s", result.AttachmentFailures.size(),
                         issueKey.c_str());
            }
        }
    }
    return outcome;
}

void MergePostStepDraftIntoCachedTicket(CachedTicket& ticket, const IssueDraft& draft,
                                        const std::vector<TrackerField>& catalog,
                                        const PostIssueStepsOutcome& postOutcome) {
    if (postOutcome.mergeStatusFromDraft) {
        const auto it = draft.FieldValues.find("status");
        if (it != draft.FieldValues.end()) {
            ticket.fieldValues["status"] = it->second;
        }
    }
    if (postOutcome.mergeSprintFieldsFromDraft) {
        for (const auto& kv : draft.FieldValues) {
            if (!IsSprintCatalogFieldId(kv.first, catalog)) {
                continue;
            }
            ticket.fieldValues[kv.first] = kv.second;
        }
    }
}

/** Overlay draft onto cache using only keys from the successful PUT fields payload (plus issue type/parent display). */
CachedTicket MergeDraftIntoCachedTicketForUpdate(const CachedTicket& existing, const IssueDraft& draft,
                                                 const std::string& issueKey,
                                                 const nlohmann::json& putFieldsSucceeded) {
    CachedTicket t = existing;
    t.id = issueKey;
    if (putFieldsSucceeded.is_object()) {
        for (auto it = putFieldsSucceeded.begin(); it != putFieldsSucceeded.end(); ++it) {
            const std::string fieldId = it.key();
            const auto dit = draft.FieldValues.find(fieldId);
            if (dit != draft.FieldValues.end()) {
                t.fieldValues[fieldId] = dit->second;
            }
        }
    }
    if (!draft.IssueTypeName.empty()) {
        t.fieldValues["issuetype"] = draft.IssueTypeName;
    } else if (!draft.IssueTypeId.empty()) {
        t.fieldValues["issuetype"] = draft.IssueTypeId;
    }
    if (!draft.ParentKey.empty()) {
        t.fieldValues["parent"] = draft.ParentKey;
    }
    return t;
}

} // namespace

namespace IssueCreatePipeline {

bool BuildFieldsPayload(const IssueDraft& draft, const std::vector<TrackerField>& catalog, nlohmann::json& outFields,
                        std::string& outError) {
    outFields = nlohmann::json::object();
    outError.clear();

    if (draft.ProjectKey.empty()) {
        outError = "Project key is empty.";
        return false;
    }
    outFields["project"] = nlohmann::json{{"key", draft.ProjectKey}};

    if (!draft.IssueTypeId.empty()) {
        outFields["issuetype"] = nlohmann::json{{"id", draft.IssueTypeId}};
    } else if (!draft.IssueTypeName.empty()) {
        outFields["issuetype"] = nlohmann::json{{"name", draft.IssueTypeName}};
    } else {
        outError = "Issue type is empty.";
        return false;
    }

    if (!draft.ParentKey.empty()) {
        outFields["parent"] = nlohmann::json{{"key", draft.ParentKey}};
    }

    for (const auto& kv : draft.FieldValues) {
        const std::string& fieldId = kv.first;
        // UI-only buffer keys (field ids starting with "__") must never be sent to Jira.
        if (fieldId.size() >= 2 && fieldId[0] == '_' && fieldId[1] == '_') {
            continue;
        }
        if (fieldId.empty() || IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) ||
            IssueDraftHelpers::IsSpecialDraftFieldId(fieldId) || fieldId == "project" || fieldId == "issuetype" ||
            fieldId == "parent") {
            continue;
        }
        const std::string raw = kv.second;
        if (raw.empty()) {
            continue;
        }

        const TrackerField* field = FindFieldById(catalog, fieldId);
        TrackerField synthetic;
        if (!field) {
            synthetic = MakeSyntheticField(fieldId);
            field = &synthetic;
        }

        const JiraField jiraField = JiraTrackerFieldAdapter::ToJiraField(*field);
        if (JiraFieldPayload::IsSprintField(jiraField)) {
            continue;
        }

        nlohmann::json value;
        std::string err;
        if (!JiraFieldPayload::BuildValue(jiraField, {raw}, value, err)) {
            outError = "Field '" + fieldId + "': " + err;
            return false;
        }
        if (!value.is_null()) {
            outFields[fieldId] = std::move(value);
        }
    }
    return true;
}

CachedTicket SeedCachedTicketFromDraft(const IssueDraft& draft, const std::vector<TrackerField>& catalog,
                                       const std::string& issueKey) {
    CachedTicket t;
    t.id = issueKey;
    for (const auto& kv : draft.FieldValues) {
        const std::string& fieldId = kv.first;
        if (fieldId.size() >= 2 && fieldId[0] == '_' && fieldId[1] == '_') {
            continue;
        }
        if (fieldId.empty() || IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) ||
            IssueDraftHelpers::IsSpecialDraftFieldId(fieldId) || fieldId == "project" || fieldId == "issuetype" ||
            fieldId == "parent") {
            continue;
        }
        if (fieldId == "status" || IsSprintCatalogFieldId(fieldId, catalog)) {
            continue;
        }
        t.fieldValues[fieldId] = kv.second;
    }
    if (!draft.IssueTypeName.empty()) {
        t.fieldValues["issuetype"] = draft.IssueTypeName;
    } else if (!draft.IssueTypeId.empty()) {
        t.fieldValues["issuetype"] = draft.IssueTypeId;
    }
    if (!draft.ParentKey.empty()) {
        t.fieldValues["parent"] = draft.ParentKey;
    }
    return t;
}

IssueCreateResult RunUpdateExisting(ITrackerClient& client, LocalCacheManager* cache, const IssueDraft& draft,
                                    const std::vector<TrackerField>& catalog) {
    IssueCreateResult result;
    std::string issueKey = JiraFieldPayload::ExtractIssueKey(draft.ExistingIssueKey);
    if (issueKey.empty()) {
        issueKey = TrimCopy(draft.ExistingIssueKey);
    }
    if (issueKey.empty()) {
        result.Error = "Issue key is empty.";
        LOG_WARN("IssueCreatePipeline: %s", result.Error.c_str());
        return result;
    }

    nlohmann::json fields;
    std::string buildErr;
    if (!BuildUpdateFieldsPayload(draft, catalog, fields, buildErr)) {
        result.Error = buildErr.empty() ? "Failed to build update payload." : buildErr;
        LOG_ERROR("IssueCreatePipeline: %s", result.Error.c_str());
        return result;
    }

    if (!fields.empty()) {
        std::string updateErr;
        if (!client.UpdateIssueFields(issueKey, fields, updateErr)) {
            result.Error = updateErr.empty() ? "Update failed." : updateErr;
            LOG_ERROR("IssueCreatePipeline: UpdateIssue failed: %s", result.Error.c_str());
            return result;
        }
    }

    result.Ok = true;
    result.IssueKey = issueKey;
    const PostIssueStepsOutcome postOutcome = ApplyPostIssueSteps(client, issueKey, draft, catalog, result);

    // SaveTicket replaces all `ticket_field_values` for the id — merge only fields from the successful PUT and,
    // separately, status/sprint after transition/sprint APIs succeed (they are not in the PUT payload).
    result.SeededTicket = SeedCachedTicketFromDraft(draft, catalog, issueKey);
    if (cache) {
        try {
            CachedTicket existing;
            if (cache->TryGetTicket(issueKey, existing)) {
                result.SeededTicket = MergeDraftIntoCachedTicketForUpdate(existing, draft, issueKey, fields);
            }
            MergePostStepDraftIntoCachedTicket(result.SeededTicket, draft, catalog, postOutcome);
            cache->SaveTicket(result.SeededTicket);
        } catch (const std::exception& ex) {
            LOG_WARN("IssueCreatePipeline: cache update after PUT failed issue=%s err=%s", issueKey.c_str(),
                     ex.what());
        }
    } else {
        MergePostStepDraftIntoCachedTicket(result.SeededTicket, draft, catalog, postOutcome);
    }
    return result;
}

IssueCreateResult Run(ITrackerClient& client, LocalCacheManager* cache, const IssueDraft& draft,
                      const RequiredFieldSet& required, const std::vector<TrackerField>& catalog) {
    IssueCreateResult result;

    IssueDraft work = draft;
    if (work.ExistingIssueKey.empty()) {
        const auto kit = work.FieldValues.find("key");
        if (kit != work.FieldValues.end() && !TrimCopy(kit->second).empty()) {
            std::string ev = JiraFieldPayload::ExtractIssueKey(kit->second);
            work.ExistingIssueKey = ev.empty() ? TrimCopy(kit->second) : ev;
        }
    }
    if (!work.ExistingIssueKey.empty()) {
        return RunUpdateExisting(client, cache, work, catalog);
    }

    result.MissingFieldIds = IssueDraftHelpers::MissingRequiredFields(work, required);
    if (!result.MissingFieldIds.empty()) {
        result.Error = "Missing required field(s).";
        LOG_WARN("IssueCreatePipeline: validation failed, %zu field(s) missing.", result.MissingFieldIds.size());
        return result;
    }

    nlohmann::json fields;
    std::string buildErr;
    if (!BuildFieldsPayload(work, catalog, fields, buildErr)) {
        result.Error = buildErr.empty() ? "Failed to build create payload." : buildErr;
        LOG_ERROR("IssueCreatePipeline: %s", result.Error.c_str());
        return result;
    }

    std::string createErr;
    std::string issueKey = client.CreateIssue(fields, createErr);
    if (issueKey.empty()) {
        result.Error = createErr.empty() ? "Create failed." : createErr;
        LOG_ERROR("IssueCreatePipeline: CreateIssue failed: %s", result.Error.c_str());
        return result;
    }
    result.Ok = true;
    result.IssueKey = issueKey;

    const PostIssueStepsOutcome postOutcome = ApplyPostIssueSteps(client, issueKey, work, catalog, result);

    result.SeededTicket = SeedCachedTicketFromDraft(work, catalog, issueKey);
    MergePostStepDraftIntoCachedTicket(result.SeededTicket, work, catalog, postOutcome);
    if (cache) {
        cache->SaveTicket(result.SeededTicket);
    }
    return result;
}

} // namespace IssueCreatePipeline
