#include "IssueCreatePipelineHelpers.h"

#include <string>

namespace IssueCreatePipelineHelpers {

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

} // namespace IssueCreatePipelineHelpers
