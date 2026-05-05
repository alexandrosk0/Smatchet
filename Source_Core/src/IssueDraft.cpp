#include "IssueDraft.h"

#include "Logger.h"
#include "NewIssueInheritDefaults.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <unordered_set>

namespace {

bool IsHardMinimumEmpty(const IssueDraft& draft, const std::string& fieldId) {
    if (fieldId == "__project__") {
        return draft.ProjectKey.empty();
    }
    if (fieldId == "__issuetype__") {
        return draft.IssueTypeId.empty() && draft.IssueTypeName.empty();
    }
    if (fieldId == "__parent__") {
        return draft.ParentKey.empty();
    }
    const auto it = draft.FieldValues.find(fieldId);
    if (it == draft.FieldValues.end()) {
        return true;
    }
    for (char c : it->second) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            return false;
        }
    }
    return true;
}

} // namespace

namespace IssueDraftHelpers {

std::vector<std::string> DefaultNewIssueInheritFieldIds() { return DefaultNewIssueInheritFieldIdsList(); }

bool IsCreateSuppressedFieldId(const std::string& fieldId) {
    static const std::unordered_set<std::string> suppressed = {
        "id",
        "key",
        "status",
        "created",
        "updated",
        "creator",
        "reporter",
        "resolutiondate",
        "lastViewed",
        "votes",
        "watches",
        "comment",
        "worklog",
        "attachment",
        "progress",
        "aggregateprogress",
        "subtasks",
        "issuelinks",
        "history",
        "changelog",
        "timespent",
        "timeestimate",
        "timeoriginalestimate",
        "aggregatetimespent",
        "aggregatetimeoriginalestimate",
        "aggregatetimeestimate",
        "workratio",
        // Jira-managed / not on create screen (often present on cached issues).
        "statuscategorychangedate",
        "statusCategory",
        "issuerestriction",
    };
    return suppressed.find(fieldId) != suppressed.end();
}

bool IsSpecialDraftFieldId(const std::string& fieldId) {
    return fieldId == "project" || fieldId == "issuetype" || fieldId == "parent";
}

std::string ResolveIssueTypeIdFromTicket(const CachedTicket& ticket, const std::vector<TrackerField>& catalog,
                                         std::string* outName) {
    const std::string current = ticket.GetFieldValue("issuetype");
    if (outName) {
        *outName = current;
    }
    if (current.empty()) {
        return {};
    }
    for (const auto& field : catalog) {
        if (field.Id != "issuetype") {
            continue;
        }
        for (const auto& option : field.AllowedValueOptions) {
            if (option.Value == current || option.Id == current) {
                if (outName && !option.Value.empty()) {
                    *outName = option.Value;
                }
                return option.Id.empty() ? option.Value : option.Id;
            }
        }
        break;
    }
    return {};
}

IssueDraft FromCachedTicket(const CachedTicket& ticket, const std::vector<TrackerField>& catalog,
                            const std::string& fallbackProjectKey, const std::string& fallbackIssueTypeId,
                            const std::string& fallbackIssueTypeName, const std::vector<std::string>& inheritFieldIds) {
    IssueDraft draft;
    draft.ProjectKey = fallbackProjectKey;
    draft.IssueTypeId = fallbackIssueTypeId;
    draft.IssueTypeName = fallbackIssueTypeName;

    // Empty `inheritFieldIds` means "use default field list" AND keep legacy behavior of
    // always taking project / issue type / parent from the last row when it exists.
    const bool inheritDefaults = inheritFieldIds.empty();
    std::vector<std::string> resolvedInherit = inheritFieldIds;
    if (resolvedInherit.empty()) {
        resolvedInherit = DefaultNewIssueInheritFieldIds();
    }
    const std::unordered_set<std::string> inheritedFieldIds(resolvedInherit.begin(), resolvedInherit.end());

    if (!ticket.id.empty()) {
        const bool useRowProject = inheritDefaults || inheritedFieldIds.count("project") > 0;
        const bool useRowIssueType = inheritDefaults || inheritedFieldIds.count("issuetype") > 0;
        const bool useRowParent = inheritDefaults || inheritedFieldIds.count("parent") > 0;

        if (useRowProject) {
            const size_t dashPos = ticket.id.find('-');
            if (dashPos != std::string::npos && dashPos > 0) {
                draft.ProjectKey = ticket.id.substr(0, dashPos);
            }
        }
        if (useRowIssueType) {
            std::string resolvedName;
            const std::string resolvedId = ResolveIssueTypeIdFromTicket(ticket, catalog, &resolvedName);
            if (!resolvedId.empty()) {
                draft.IssueTypeId = resolvedId;
            }
            if (!resolvedName.empty()) {
                draft.IssueTypeName = resolvedName;
            }
        }
        if (useRowParent) {
            const std::string parentRaw = ticket.GetFieldValue("parent");
            if (!parentRaw.empty()) {
                std::string parentKey = parentRaw;
                const size_t sep = parentKey.find(" - ");
                if (sep != std::string::npos) {
                    parentKey = parentKey.substr(0, sep);
                }
                draft.ParentKey = parentKey;
            }
        }
    }

    for (const auto& kv : ticket.fieldValues) {
        const std::string& fieldId = kv.first;
        // Summary is required on create but must not be copied from the previous row.
        if (fieldId == "summary") {
            continue;
        }
        if (inheritedFieldIds.find(fieldId) == inheritedFieldIds.end()) {
            continue;
        }
        if (IsCreateSuppressedFieldId(fieldId)) {
            continue;
        }
        // project / issuetype / parent live on the draft struct for create; also copy into
        // FieldValues when the user listed them so grid cells show inherited values.
        if (IsSpecialDraftFieldId(fieldId)) {
            if (inheritDefaults) {
                continue;
            }
        }
        if (kv.second.empty()) {
            continue;
        }
        draft.FieldValues[fieldId] = kv.second;
    }

    return draft;
}

std::vector<std::string> MissingRequiredFields(const IssueDraft& draft, const RequiredFieldSet& required) {
    if (!draft.ExistingIssueKey.empty()) {
        return {};
    }
    std::vector<std::string> out;
    std::vector<std::string> hardMinimum = {"__project__", "summary"};
    if (required.RequiresIssueType) {
        hardMinimum.push_back("__issuetype__");
    }
    for (const auto& key : hardMinimum) {
        if (IsHardMinimumEmpty(draft, key)) {
            out.push_back(key);
        }
    }
    if (required.IsSubtask && draft.ParentKey.empty()) {
        out.push_back("__parent__");
    }
    for (const auto& id : required.FieldIds) {
        if (IsCreateSuppressedFieldId(id) || IsSpecialDraftFieldId(id)) {
            continue;
        }
        if (id == "summary") {
            continue;
        }
        if (IsHardMinimumEmpty(draft, id)) {
            out.push_back(id);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string ToJson(const IssueDraft& draft) {
    nlohmann::json j = nlohmann::json::object();
    j["projectKey"] = draft.ProjectKey;
    j["issueTypeId"] = draft.IssueTypeId;
    j["issueTypeName"] = draft.IssueTypeName;
    j["parentKey"] = draft.ParentKey;
    if (!draft.ExistingIssueKey.empty()) {
        j["existingIssueKey"] = draft.ExistingIssueKey;
    }
    nlohmann::json fields = nlohmann::json::object();
    for (const auto& kv : draft.FieldValues) {
        fields[kv.first] = kv.second;
    }
    j["fields"] = std::move(fields);
    nlohmann::json attachments = nlohmann::json::array();
    for (const auto& att : draft.StagedAttachments) {
        attachments.push_back(nlohmann::json{
            {"absPath", att.AbsPath},
            {"fileName", att.FileName},
            {"sizeBytes", att.SizeBytes},
        });
    }
    j["attachments"] = std::move(attachments);
    return j.dump();
}

bool FromJson(const std::string& json, IssueDraft& outDraft, std::string& outError) {
    outDraft = IssueDraft{};
    outError.clear();
    try {
        auto j = nlohmann::json::parse(json);
        outDraft.ProjectKey = j.value("projectKey", std::string());
        outDraft.IssueTypeId = j.value("issueTypeId", std::string());
        outDraft.IssueTypeName = j.value("issueTypeName", std::string());
        outDraft.ParentKey = j.value("parentKey", std::string());
        outDraft.ExistingIssueKey = j.value("existingIssueKey", std::string());
        if (j.contains("fields") && j["fields"].is_object()) {
            for (auto it = j["fields"].begin(); it != j["fields"].end(); ++it) {
                if (it.value().is_string()) {
                    outDraft.FieldValues[it.key()] = it.value().get<std::string>();
                } else if (it.value().is_number_integer()) {
                    outDraft.FieldValues[it.key()] = std::to_string(it.value().get<long long>());
                } else if (it.value().is_number_float()) {
                    outDraft.FieldValues[it.key()] = std::to_string(it.value().get<double>());
                } else if (it.value().is_boolean()) {
                    outDraft.FieldValues[it.key()] = it.value().get<bool>() ? "true" : "false";
                } else if (!it.value().is_null()) {
                    outDraft.FieldValues[it.key()] = it.value().dump();
                }
            }
        }
        if (j.contains("attachments") && j["attachments"].is_array()) {
            for (const auto& item : j["attachments"]) {
                if (!item.is_object()) {
                    continue;
                }
                StagedAttachment att;
                att.AbsPath = item.value("absPath", std::string());
                att.FileName = item.value("fileName", std::string());
                att.SizeBytes = item.value("sizeBytes", static_cast<std::uint64_t>(0));
                if (!att.AbsPath.empty()) {
                    outDraft.StagedAttachments.push_back(std::move(att));
                }
            }
        }
        return true;
    } catch (const std::exception& ex) {
        outError = ex.what();
        return false;
    } catch (...) {
        outError = "Unknown parse error";
        return false;
    }
}

namespace {

std::string TrimForDiff(const std::string& s) {
    size_t a = 0;
    size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    return s.substr(a, b - a);
}

/** Draft field ids we never want to show in the diff (caller-owned / server-managed). */
bool IsDiffSkippedFieldId(const std::string& fieldId) {
    if (fieldId.size() >= 2 && fieldId[0] == '_' && fieldId[1] == '_') {
        return true;
    }
    if (fieldId.empty()) {
        return true;
    }
    if (IsCreateSuppressedFieldId(fieldId)) {
        // Note: 'status' IS in this list, but we still want to diff status.
        return fieldId != "status";
    }
    // key column never goes to Jira; ExistingIssueKey is carried separately.
    if (fieldId == "key" || fieldId == "issuekey") {
        return true;
    }
    return false;
}

} // namespace

std::vector<FieldChange> ComputeFieldChanges(const IssueDraft& draft, const CachedTicket& existing) {
    std::vector<FieldChange> out;
    for (const auto& kv : draft.FieldValues) {
        if (IsDiffSkippedFieldId(kv.first)) {
            continue;
        }
        const std::string newVal = TrimForDiff(kv.second);
        const std::string oldVal = TrimForDiff(existing.GetFieldValue(kv.first));
        if (newVal == oldVal) {
            continue;
        }
        out.push_back(FieldChange{kv.first, oldVal, newVal});
    }

    if (!draft.ParentKey.empty()) {
        const std::string oldParent = TrimForDiff(existing.GetFieldValue("parent"));
        if (TrimForDiff(draft.ParentKey) != oldParent) {
            out.push_back(FieldChange{"parent", oldParent, draft.ParentKey});
        }
    }

    std::sort(out.begin(), out.end(), [](const FieldChange& a, const FieldChange& b) { return a.FieldId < b.FieldId; });
    return out;
}

void PruneUnchangedFields(IssueDraft& draft, const CachedTicket& existing) {
    for (auto it = draft.FieldValues.begin(); it != draft.FieldValues.end();) {
        if (IsDiffSkippedFieldId(it->first)) {
            ++it;
            continue;
        }
        const std::string newVal = TrimForDiff(it->second);
        const bool keyInCache = existing.fieldValues.find(it->first) != existing.fieldValues.end();
        if (!keyInCache && !newVal.empty()) {
            LOG_DEBUG("IssueDraftHelpers::PruneUnchangedFields: field '%s' not in cached ticket '%s'; treating "
                      "baseline as empty",
                      it->first.c_str(), existing.id.c_str());
        }
        const std::string oldVal = TrimForDiff(existing.GetFieldValue(it->first));
        if (newVal == oldVal) {
            it = draft.FieldValues.erase(it);
        } else {
            ++it;
        }
    }
    if (!draft.ParentKey.empty()) {
        const std::string oldParent = TrimForDiff(existing.GetFieldValue("parent"));
        if (TrimForDiff(draft.ParentKey) == oldParent) {
            draft.ParentKey.clear();
        }
    }
}

std::vector<std::string> MapFieldIdsToNames(const std::vector<std::string>& ids,
                                           const std::vector<TrackerField>& catalog) {
    std::vector<std::string> names;
    names.reserve(ids.size());
    for (const auto& id : ids) {
        if (id == "__project__") {
            names.push_back("Project");
        } else if (id == "__issuetype__") {
            names.push_back("Issue Type");
        } else if (id == "__parent__") {
            names.push_back("Parent");
        } else {
            bool found = false;
            for (const auto& f : catalog) {
                if (f.Id == id) {
                    names.push_back(f.Name);
                    found = true;
                    break;
                }
            }
            if (!found) {
                names.push_back(id);
            }
        }
    }
    return names;
}

} // namespace IssueDraftHelpers
