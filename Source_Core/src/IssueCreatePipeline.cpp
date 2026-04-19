#include "IssueCreatePipeline.h"

#include "ITrackerClient.h"
#include "JiraFieldPayload.h"
#include "LocalCacheManager.h"
#include "Logger.h"
#include "StringUtil.h"

#include <algorithm>
#include <unordered_set>

namespace {

const JiraField* FindFieldById(const std::vector<JiraField>& catalog, const std::string& id) {
    for (const auto& field : catalog) {
        if (field.Id == id) {
            return &field;
        }
    }
    return nullptr;
}

/** Synthesize a minimal JiraField for fields that aren't in the catalog (unlikely, but safe). */
JiraField MakeSyntheticField(const std::string& id) {
    JiraField f;
    f.Id = id;
    f.Type = "string";
    f.Family = JiraFieldFamily::Text;
    return f;
}

/** PUT /issue fields: mutable custom + system fields; no project/type swap; status/sprint handled elsewhere. */
bool BuildUpdateFieldsPayload(const IssueDraft& draft,
                              const std::vector<JiraField>& catalog,
                              nlohmann::json& outFields,
                              std::string& outError) {
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
        if (fieldId.empty() ||
            IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) ||
            IssueDraftHelpers::IsSpecialDraftFieldId(fieldId) ||
            fieldId == "project" || fieldId == "issuetype" || fieldId == "parent") {
            continue;
        }
        if (fieldId == "status") {
            continue;
        }
        const std::string raw = kv.second;
        if (raw.empty()) {
            continue;
        }

        const JiraField* field = FindFieldById(catalog, fieldId);
        JiraField synthetic;
        if (!field) {
            synthetic = MakeSyntheticField(fieldId);
            field = &synthetic;
        }

        if (JiraFieldPayload::IsSprintField(*field)) {
            continue;
        }

        nlohmann::json value;
        std::string err;
        if (!JiraFieldPayload::BuildValue(*field, {raw}, value, err)) {
            outError = "Field '" + fieldId + "': " + err;
            return false;
        }
        if (!value.is_null()) {
            outFields[fieldId] = std::move(value);
        }
    }
    return true;
}

void ApplyPostIssueSteps(ITrackerClient& client,
                         const std::string& issueKey,
                         const IssueDraft& draft,
                         const std::vector<JiraField>& catalog,
                         IssueCreateResult& result) {
    const auto statusIt = draft.FieldValues.find("status");
    if (statusIt != draft.FieldValues.end()) {
        const std::string statusRaw = TrimCopy(statusIt->second);
        if (!statusRaw.empty()) {
            const JiraField* statusField = FindFieldById(catalog, "status");
            JiraField statusSynthetic;
            if (!statusField) {
                statusSynthetic = MakeSyntheticField("status");
                statusField = &statusSynthetic;
            }
            nlohmann::json statusValue;
            std::string statusBuildErr;
            if (JiraFieldPayload::BuildValue(*statusField, {statusRaw}, statusValue, statusBuildErr) &&
                !statusValue.is_null()) {
                nlohmann::json statusUpdate = nlohmann::json::object();
                statusUpdate["status"] = std::move(statusValue);
                std::string transitionErr;
                if (!client.UpdateIssueFields(issueKey, statusUpdate, transitionErr)) {
                    LOG_WARN("IssueCreatePipeline: issue %s: status not applied: %s",
                             issueKey.c_str(),
                             transitionErr.c_str());
                }
            } else if (!statusBuildErr.empty()) {
                LOG_WARN("IssueCreatePipeline: issue %s: status payload invalid: %s",
                         issueKey.c_str(),
                         statusBuildErr.c_str());
            }
        }
    }

    std::unordered_set<std::string> appliedSprintIds;
    for (const auto& kv : draft.FieldValues) {
        const std::string& fieldId = kv.first;
        if (fieldId.size() >= 2 && fieldId[0] == '_' && fieldId[1] == '_') {
            continue;
        }
        if (fieldId.empty() ||
            IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) ||
            IssueDraftHelpers::IsSpecialDraftFieldId(fieldId) ||
            fieldId == "project" || fieldId == "issuetype" || fieldId == "parent") {
            continue;
        }
        const JiraField* sprintField = FindFieldById(catalog, fieldId);
        if (sprintField == nullptr || !JiraFieldPayload::IsSprintField(*sprintField)) {
            continue;
        }
        const std::string raw = TrimCopy(kv.second);
        if (raw.empty()) {
            continue;
        }
        for (const std::string& seg : JiraFieldPayload::SplitCommaSeparatedValues(raw)) {
            const std::string sprintId = JiraFieldPayload::ResolveSprintIdForAgile(*sprintField, seg);
            if (sprintId.empty()) {
                LOG_WARN("IssueCreatePipeline: issue %s: sprint segment could not be resolved to id "
                         "(field %s, value '%s').",
                         issueKey.c_str(),
                         fieldId.c_str(),
                         seg.c_str());
                continue;
            }
            if (!appliedSprintIds.insert(sprintId).second) {
                continue;
            }
            std::string sprintErr;
            if (!client.AddIssueToSprint(issueKey, sprintId, sprintErr)) {
                LOG_WARN("IssueCreatePipeline: issue %s: AddIssueToSprint failed: %s",
                         issueKey.c_str(),
                         sprintErr.c_str());
            }
        }
    }

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
                LOG_WARN("IssueCreatePipeline: %zu attachment(s) failed for %s",
                         result.AttachmentFailures.size(), issueKey.c_str());
            }
        }
    }
}

} // namespace

namespace IssueCreatePipeline {

bool BuildFieldsPayload(const IssueDraft& draft,
                         const std::vector<JiraField>& catalog,
                         nlohmann::json& outFields,
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
        if (fieldId.empty() ||
            IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) ||
            IssueDraftHelpers::IsSpecialDraftFieldId(fieldId) ||
            fieldId == "project" || fieldId == "issuetype" || fieldId == "parent") {
            continue;
        }
        const std::string raw = kv.second;
        if (raw.empty()) {
            continue;
        }

        const JiraField* field = FindFieldById(catalog, fieldId);
        JiraField synthetic;
        if (!field) {
            synthetic = MakeSyntheticField(fieldId);
            field = &synthetic;
        }

        if (JiraFieldPayload::IsSprintField(*field)) {
            continue;
        }

        nlohmann::json value;
        std::string err;
        if (!JiraFieldPayload::BuildValue(*field, {raw}, value, err)) {
            outError = "Field '" + fieldId + "': " + err;
            return false;
        }
        if (!value.is_null()) {
            outFields[fieldId] = std::move(value);
        }
    }
    return true;
}

CachedTicket SeedCachedTicketFromDraft(const IssueDraft& draft,
                                        const std::vector<JiraField>& /*catalog*/,
                                        const std::string& issueKey) {
    CachedTicket t;
    t.id = issueKey;
    t.fieldValues = draft.FieldValues;
    if (!draft.IssueTypeName.empty()) {
        t.fieldValues["issuetype"] = draft.IssueTypeName;
    }
    if (!draft.ParentKey.empty()) {
        t.fieldValues["parent"] = draft.ParentKey;
    }
    return t;
}

IssueCreateResult RunUpdateExisting(ITrackerClient& client,
                                    LocalCacheManager* /*cache*/,
                                    const IssueDraft& draft,
                                    const std::vector<JiraField>& catalog) {
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
    ApplyPostIssueSteps(client, issueKey, draft, catalog, result);

    // Intentionally do NOT cache->SaveTicket here: SaveTicket replaces the whole
    // ticket_field_values row with only the partial draft fields (e.g. a CSV with
    // just key + summary would wipe every other cached field). Callers trigger a
    // RefreshLocalData() on result.Ok which repopulates cache from Jira.
    result.SeededTicket = SeedCachedTicketFromDraft(draft, catalog, issueKey);
    return result;
}

IssueCreateResult Run(ITrackerClient& client,
                      LocalCacheManager* cache,
                      const IssueDraft& draft,
                      const RequiredFieldSet& required,
                      const std::vector<JiraField>& catalog) {
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
        LOG_WARN("IssueCreatePipeline: validation failed, %zu field(s) missing.",
                 result.MissingFieldIds.size());
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

    ApplyPostIssueSteps(client, issueKey, work, catalog, result);

    result.SeededTicket = SeedCachedTicketFromDraft(work, catalog, issueKey);
    if (cache) {
        cache->SaveTicket(result.SeededTicket);
    }
    return result;
}

} // namespace IssueCreatePipeline
