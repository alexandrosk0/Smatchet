#include "IssueCreatePipeline.h"
#include <nlohmann/json.hpp> // fan-in Phase 2: IssueCreatePipeline.h dropped json.hpp for json_fwd; BuildFieldsPayload constructs json here.

#include "ITrackerIssueMutations.h"
#include "IssueCreatePipelineHelpers.h"
#include "TrackerFieldPayload.h"

#include "ISyncCache.h"
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

// Apply the draft's "status" via UpdateIssueFields. Returns true on a successful
// transition, letting the caller merge the new status into the cached ticket.
bool ApplyPostIssueStatusStep(ITrackerIssueMutations& client, const std::string& issueKey, const IssueDraft& draft,
                              const std::vector<TrackerField>& catalog) {
    const auto statusIt = draft.FieldValues.find("status");
    if (statusIt == draft.FieldValues.end()) {
        return false;
    }
    const std::string statusRaw = TrimCopy(statusIt->second);
    if (statusRaw.empty()) {
        return false;
    }
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
        const TrackerError transitionErr = client.UpdateIssueFields(issueKey, statusUpdate);
        if (transitionErr.IsOk()) {
            return true;
        }
        LOG_WARN("IssueCreatePipeline: issue %s: status not applied: %s", issueKey.c_str(),
                 transitionErr.Detail.c_str());
    } else if (!statusBuildErr.empty()) {
        LOG_WARN("IssueCreatePipeline: issue %s: status payload invalid: %s", issueKey.c_str(), statusBuildErr.c_str());
    }
    return false;
}

// Apply one sprint-typed draft field, adding the issue to each resolved sprint id.
// Returns false if any segment failed to resolve / apply (so the caller withholds the merge).
bool ApplySprintFieldSegments(ITrackerIssueMutations& client, const std::string& issueKey, const std::string& fieldId,
                              const TrackerField& jiraSprintField, const std::string& raw,
                              std::unordered_set<std::string>& appliedSprintIds) {
    bool allOk = true;
    bool sprintSegmentSeen = false;
    for (const std::string& seg : TrackerFieldPayload::SplitCommaSeparatedValues(raw)) {
        sprintSegmentSeen = true;
        const std::string sprintId = TrackerFieldPayload::ResolveSprintIdForAgile(jiraSprintField, seg);
        if (sprintId.empty()) {
            allOk = false;
            LOG_WARN("IssueCreatePipeline: issue %s: sprint segment could not be resolved to id "
                     "(field %s, value '%s').",
                     issueKey.c_str(), fieldId.c_str(), seg.c_str());
            continue;
        }
        if (!appliedSprintIds.insert(sprintId).second) {
            continue;
        }
        const TrackerError sprintErr = client.AddIssueToSprint(issueKey, sprintId);
        if (!sprintErr.IsOk()) {
            allOk = false;
            LOG_WARN("IssueCreatePipeline: issue %s: AddIssueToSprint failed: %s", issueKey.c_str(),
                     sprintErr.Detail.c_str());
        }
    }
    if (!sprintSegmentSeen) {
        allOk = false;
    }
    return allOk;
}

// Apply all sprint-typed draft fields. Returns true iff at least one sprint field did
// work AND every segment resolved/applied cleanly (gating the cached-ticket merge).
bool ApplyPostIssueSprintSteps(ITrackerIssueMutations& client, const std::string& issueKey, const IssueDraft& draft,
                               const std::vector<TrackerField>& catalog) {
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
        if (!ApplySprintFieldSegments(client, issueKey, fieldId, jiraSprintField, raw, appliedSprintIds)) {
            sprintAllOk = false;
        }
    }
    return sprintWork && sprintAllOk;
}

void ApplyPostIssueAttachmentStep(ITrackerIssueMutations& client, const std::string& issueKey, const IssueDraft& draft,
                                  IssueCreateResult& result) {
    if (draft.StagedAttachments.empty()) {
        return;
    }
    std::vector<std::string> paths;
    paths.reserve(draft.StagedAttachments.size());
    for (const auto& a : draft.StagedAttachments) {
        if (!a.AbsPath.empty()) {
            paths.push_back(a.AbsPath);
        }
    }
    if (!paths.empty()) {
        // Ok payload = per-file failures (plan landmine L3). A hard Err (auth / empty key / Plane
        // unsupported) leaves AttachmentFailures empty — matching the prior contract, where this
        // caller ignored the bool/error and the hard-fail path never populated the failures list.
        auto attachResult = client.AttachFilesToIssue(issueKey, paths);
        if (attachResult) {
            result.AttachmentFailures = std::move(attachResult.value());
        }
        if (!result.AttachmentFailures.empty()) {
            LOG_WARN("IssueCreatePipeline: %zu attachment(s) failed for %s", result.AttachmentFailures.size(),
                     issueKey.c_str());
        }
    }
}

PostIssueStepsOutcome ApplyPostIssueSteps(ITrackerIssueMutations& client, const std::string& issueKey,
                                          const IssueDraft& draft, const std::vector<TrackerField>& catalog,
                                          IssueCreateResult& result) {
    PostIssueStepsOutcome outcome;
    outcome.mergeStatusFromDraft = ApplyPostIssueStatusStep(client, issueKey, draft, catalog);
    outcome.mergeSprintFieldsFromDraft = ApplyPostIssueSprintSteps(client, issueKey, draft, catalog);
    ApplyPostIssueAttachmentStep(client, issueKey, draft, result);
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
    // However, ITrackerBackend doesn't have a way to call BuildCreatePayload without an instance.
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

IssueCreateResult RunUpdateExisting(ITrackerIssueMutations& client, ISyncCache* cache,
                                    const std::string& cacheBackendKey, const IssueDraft& draft,
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

    auto updatePayloadResult = client.BuildUpdatePayload(draft, catalog);
    if (!updatePayloadResult) {
        const std::string& buildErr = updatePayloadResult.error().Detail;
        result.Error = buildErr.empty() ? "Failed to build update payload." : buildErr;
        LOG_ERROR("IssueCreatePipeline: %s", result.Error.c_str());
        return result;
    }
    nlohmann::json fields = std::move(updatePayloadResult.value());

    if (!fields.empty()) {
        const TrackerError updateErr = client.UpdateIssueFields(issueKey, fields);
        if (!updateErr.IsOk()) {
            result.Error = updateErr.Detail.empty() ? "Update failed." : updateErr.Detail;
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
            if (cache->TryGetTicket(cacheBackendKey, issueKey, existing)) {
                result.SeededTicket =
                    IssueCreatePipelineHelpers::MergeDraftIntoCachedTicketForUpdate(existing, draft, issueKey, fields);
            }
            MergePostStepDraftIntoCachedTicket(result.SeededTicket, draft, catalog, postOutcome);
            cache->SaveTicket(cacheBackendKey, result.SeededTicket);
        } catch (const std::exception& ex) {
            LOG_WARN("IssueCreatePipeline: cache update after PUT failed issue=%s err=%s", issueKey.c_str(), ex.what());
        }
    } else {
        MergePostStepDraftIntoCachedTicket(result.SeededTicket, draft, catalog, postOutcome);
    }
    return result;
}

IssueCreateResult Run(ITrackerIssueMutations& client, ISyncCache* cache, const std::string& cacheBackendKey,
                      const IssueDraft& draft, const RequiredFieldSet& required,
                      const std::vector<TrackerField>& catalog) {
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
        return RunUpdateExisting(client, cache, cacheBackendKey, work, catalog);
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

    auto createPayloadResult = client.BuildCreatePayload(work, catalog);
    if (!createPayloadResult) {
        const std::string& buildErr = createPayloadResult.error().Detail;
        result.Error = buildErr.empty() ? "Failed to build create payload." : buildErr;
        LOG_ERROR("IssueCreatePipeline: %s", result.Error.c_str());
        return result;
    }
    nlohmann::json fields = std::move(createPayloadResult.value());

    LOG_DEBUG("IssueCreatePipeline: calling CreateIssue with payload: %s", fields.dump().c_str());
    auto createResult = client.CreateIssue(fields);
    // Plane's "created, key unknown" path returns Ok(empty) (not Err); both an Err and an empty Ok key
    // mean "no usable key" and take the same failure branch as the prior bool/string contract did.
    std::string issueKey = createResult ? createResult.value() : std::string();
    if (issueKey.empty()) {
        const std::string createErr = createResult ? std::string() : createResult.error().Detail;
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
        // Wrapped like RunUpdateExisting's post-PUT cache save (above): SaveTicket is
        // SQLiteCpp-backed and throws (DB locked / disk full / the TextMerge cell-budget
        // bail-out surfacing upstream). Left bare, a throw here unwinds out of
        // ReplayOneCreate past the point where OfflineQueueService resets its in-flight
        // latch — the latch then stays true forever (replay silently dead until restart)
        // and, since DeletePendingCreate never ran either, the next replay re-creates this
        // same issue server-side. CPP_CODE_AUDIT.md #6.
        //
        // LOG_WARN + swallow (not the exception-handling-policy.md Cache/DB tier's default
        // LOG_ERROR + rethrow) is deliberate here, matching RunUpdateExisting: result.Ok and
        // result.IssueKey are already set above — the tracker-side create already succeeded.
        // Rethrowing would unwind past that return, so ReplayOneCreate would never see
        // result.Ok=true, never call DeletePendingCreate, and retry the create on the next
        // tick — reintroducing this exact finding's "duplicate issue" failure mode via a
        // different path. The local cache is a best-effort mirror of server truth here, not
        // the source of truth, so a cache-only failure must not roll back a successful create.
        try {
            cache->SaveTicket(cacheBackendKey, result.SeededTicket);
        } catch (const std::exception& ex) {
            LOG_WARN("IssueCreatePipeline: cache save after Create failed issue=%s err=%s", issueKey.c_str(),
                     ex.what());
        }
    }
    return result;
}

} // namespace IssueCreatePipeline
