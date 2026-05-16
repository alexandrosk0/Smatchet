#include "IssueCreatePipeline.h"

#include "ITrackerClient.h"
#include "IssueCreatePipelineHelpers.h"
#include "TrackerFieldPayload.h"

#include "LocalCacheManager.h"
#include "Logger.h"
#include "StringUtil.h"

#include <algorithm>
#include <unordered_set>

namespace {

const TrackerField* FindFieldById(const std::vector<TrackerField>& catalog, const std::string& id) {
    auto it = std::find_if(catalog.begin(), catalog.end(), [&](const auto& field) { return field.Id == id; });
    return it == catalog.end() ? nullptr : &(*it);
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
// BuildUpdateFieldsPayload removed, logic moved to Backend::BuildUpdatePayload

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
    return TrackerFieldPayload::IsSprintField(*sprintField);
}

PostIssueStepsOutcome ApplyPostIssueSteps(ITrackerClient& client, const std::string& issueKey, const IssueDraft& draft,
                                          const std::vector<TrackerField>& catalog, IssueCreateResult& result) {
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
            const TrackerField jiraStatusField = *statusField;
            nlohmann::json statusValue;
            std::string statusBuildErr;
            if (TrackerFieldPayload::BuildValue(jiraStatusField, {statusRaw}, statusValue, statusBuildErr) &&
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
        const TrackerField jiraSprintField = *sprintField;
        if (!TrackerFieldPayload::IsSprintField(jiraSprintField)) {
            continue;
        }
        const std::string raw = TrimCopy(kv.second);
        if (raw.empty()) {
            continue;
        }
        sprintWork = true;
        bool sprintSegmentSeen = false;
        for (const std::string& seg : TrackerFieldPayload::SplitCommaSeparatedValues(raw)) {
            sprintSegmentSeen = true;
            const std::string sprintId = TrackerFieldPayload::ResolveSprintIdForAgile(jiraSprintField, seg);
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

} // namespace

namespace IssueCreatePipeline {

bool BuildFieldsPayload(const IssueDraft& /*draft*/, const std::vector<TrackerField>& /*catalog*/,
                        nlohmann::json& /*outFields*/, std::string& outError) {
    // This method is now just a wrapper for compatibility, but the pipeline should use the client directly.
    // However, ITrackerClient doesn't have a way to call BuildCreatePayload without an instance.
    // So we'll keep the signature for now but it's deprecated for polymorphic use.
    outError = "BuildFieldsPayload(draft, catalog, ...) is deprecated; use client.BuildCreatePayload instead.";
    return false;
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
    std::string issueKey = TrackerFieldPayload::ExtractIssueKey(draft.ExistingIssueKey);
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
    if (!client.BuildUpdatePayload(draft, catalog, fields, buildErr)) {
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
                result.SeededTicket =
                    IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate(existing, draft, issueKey, fields);
            }
            MergePostStepDraftIntoCachedTicket(result.SeededTicket, draft, catalog, postOutcome);
            cache->SaveTicket(result.SeededTicket);
        } catch (const std::exception& ex) {
            LOG_WARN("IssueCreatePipeline: cache update after PUT failed issue=%s err=%s", issueKey.c_str(), ex.what());
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
            std::string ev = TrackerFieldPayload::ExtractIssueKey(kit->second);
            work.ExistingIssueKey = ev.empty() ? TrimCopy(kit->second) : ev;
        }
    }
    if (!work.ExistingIssueKey.empty()) {
        return RunUpdateExisting(client, cache, work, catalog);
    }

    result.MissingFieldIds = IssueDraftHelpers::MissingRequiredFields(work, required);
    if (!result.MissingFieldIds.empty()) {
        std::vector<std::string> names = IssueDraftHelpers::MapFieldIdsToNames(result.MissingFieldIds, catalog);
        result.Error = "Missing required field(s): " + JoinStrings(names, ", ");
        LOG_WARN("IssueCreatePipeline: validation failed: %s", result.Error.c_str());
        return result;
    }

    LOG_DEBUG("IssueCreatePipeline: building create payload for project=%s issuetype=%s", work.ProjectKey.c_str(),
              work.IssueTypeId.c_str());

    nlohmann::json fields;
    std::string buildErr;
    if (!client.BuildCreatePayload(work, catalog, fields, buildErr)) {
        result.Error = buildErr.empty() ? "Failed to build create payload." : buildErr;
        LOG_ERROR("IssueCreatePipeline: %s", result.Error.c_str());
        return result;
    }

    std::string createErr;
    LOG_DEBUG("IssueCreatePipeline: calling CreateIssue with payload: %s", fields.dump().c_str());
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
