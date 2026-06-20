#include "LinearMutationPure.h"

#include <cstddef>
#include <unordered_map>
#include <utility>

namespace smatchet {
namespace linear {

// === GraphQL documents (hand-written, mirroring LinearIssueSearch.cpp) ===

const char* ResolveIssueQueryDocument() { return "query($id:String!){ issue(id:$id){ id } }"; }

const char* IssueUpdateMutationDocument() {
    return "mutation($id: String!, $input: IssueUpdateInput!){ "
           "issueUpdate(id:$id, input:$input){ success issue{ id identifier } } }";
}

const char* IssueCreateMutationDocument() {
    return "mutation($input: IssueCreateInput!){ "
           "issueCreate(input:$input){ success issue{ id identifier url } } }";
}

const char* CommentCreateMutationDocument() {
    return "mutation($input: CommentCreateInput!){ commentCreate(input:$input){ success comment{ id } } }";
}

// === Option / priority resolution ===

std::string ResolveOptionUuid(const TrackerField& field, const std::string& value) {
    for (std::size_t i = 0; i < field.AllowedValueOptions.size(); ++i) {
        if (field.AllowedValueOptions[i].Id == value) {
            return value; // already a UUID
        }
    }
    for (std::size_t i = 0; i < field.AllowedValueOptions.size(); ++i) {
        if (field.AllowedValueOptions[i].Value == value) {
            return field.AllowedValueOptions[i].Id; // display → UUID
        }
    }
    return value;
}

bool MapPriorityValueToInt(const std::string& value, int& outPriority) {
    if (value == "0" || value == "No priority" || value == "None") {
        outPriority = 0;
        return true;
    }
    if (value == "1" || value == "Urgent") {
        outPriority = 1;
        return true;
    }
    if (value == "2" || value == "High") {
        outPriority = 2;
        return true;
    }
    if (value == "3" || value == "Medium") {
        outPriority = 3;
        return true;
    }
    if (value == "4" || value == "Low") {
        outPriority = 4;
        return true;
    }
    return false;
}

// === Input builders ===

Result<nlohmann::json, TrackerError> BuildIssueUpdateInput(const TrackerField& field,
                                                           const std::vector<std::string>& values) {
    nlohmann::json input = nlohmann::json::object();
    const std::string& id = field.Id;
    const std::string first = values.empty() ? std::string() : values.front();

    if (id == "summary") {
        input["title"] = first;
    } else if (id == "description") {
        input["description"] = first; // Linear stores markdown verbatim
    } else if (id == "status") {
        input["stateId"] = ResolveOptionUuid(field, first);
    } else if (id == "assignee") {
        // Empty clears the assignee (IssueUpdateInput accepts null assigneeId).
        if (first.empty()) {
            input["assigneeId"] = nullptr;
        } else {
            input["assigneeId"] = ResolveOptionUuid(field, first);
        }
    } else if (id == "labels") {
        nlohmann::json labelIds = nlohmann::json::array();
        for (std::size_t i = 0; i < values.size(); ++i) {
            labelIds.push_back(ResolveOptionUuid(field, values[i]));
        }
        input["labelIds"] = labelIds; // set-replace: full intended set
    } else if (id == "priority") {
        int priorityInt = 0;
        if (first.empty()) {
            input["priority"] = 0; // cleared → No priority
        } else if (MapPriorityValueToInt(first, priorityInt)) {
            input["priority"] = priorityInt;
        } else {
            return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(
                std::string("LinearClient: unrecognised priority value '") + first + "'"));
        }
    } else if (id == "project") {
        if (first.empty()) {
            input["projectId"] = nullptr;
        } else {
            input["projectId"] = ResolveOptionUuid(field, first);
        }
    } else {
        return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(
            std::string("LinearClient: field '") + id + "' is not editable on Linear"));
    }
    return Result<nlohmann::json, TrackerError>::Ok(std::move(input));
}

nlohmann::json BuildLabelIdsFromCsv(const std::string& labelsCsv, const TrackerField* labelsField) {
    nlohmann::json labelIds = nlohmann::json::array();
    std::size_t start = 0;
    while (start <= labelsCsv.size()) {
        const std::size_t comma = labelsCsv.find(',', start);
        const std::size_t end = (comma == std::string::npos) ? labelsCsv.size() : comma;
        const std::string token = labelsCsv.substr(start, end - start);
        const std::size_t first = token.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            const std::size_t last = token.find_last_not_of(" \t\r\n");
            const std::string trimmed = token.substr(first, last - first + 1);
            labelIds.push_back(labelsField != nullptr ? ResolveOptionUuid(*labelsField, trimmed) : trimmed);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return labelIds;
}

Result<nlohmann::json, TrackerError> BuildIssueCreateInput(const IssueDraft& draft,
                                                           const std::vector<TrackerField>& catalog,
                                                           const std::string& teamId) {
    using PayloadResult = Result<nlohmann::json, TrackerError>;
    if (teamId.empty()) {
        return PayloadResult::Err(TrackerErrorInvalidRequest(
            "Linear issue create requires a configured Team (set Preferences > Tracker > Linear team)"));
    }

    auto fieldOr = [&draft](const char* key) -> std::string {
        const std::unordered_map<std::string, std::string>::const_iterator it = draft.FieldValues.find(key);
        return it == draft.FieldValues.end() ? std::string() : it->second;
    };
    auto fieldForId = [&catalog](const std::string& id) -> const TrackerField* {
        for (std::size_t i = 0; i < catalog.size(); ++i) {
            if (catalog[i].Id == id) {
                return &catalog[i];
            }
        }
        return nullptr;
    };
    auto resolveViaCatalog = [&fieldForId](const std::string& id, const std::string& value) -> std::string {
        if (value.empty()) {
            return value;
        }
        const TrackerField* f = fieldForId(id);
        return f != nullptr ? ResolveOptionUuid(*f, value) : value;
    };

    const std::string summary = fieldOr("summary");
    if (summary.empty()) {
        return PayloadResult::Err(
            TrackerErrorInvalidRequest("Linear issue create requires a non-empty title (summary)"));
    }

    nlohmann::json input = nlohmann::json::object();
    input["teamId"] = teamId;
    input["title"] = summary;

    const std::string description = fieldOr("description");
    if (!description.empty()) {
        input["description"] = description; // markdown verbatim
    }
    const std::string priority = fieldOr("priority");
    if (!priority.empty()) {
        int priorityInt = 0;
        if (MapPriorityValueToInt(priority, priorityInt)) {
            input["priority"] = priorityInt;
        }
    }
    const std::string status = fieldOr("status");
    if (!status.empty()) {
        input["stateId"] = resolveViaCatalog("status", status);
    }
    const std::string assignee = fieldOr("assignee");
    if (!assignee.empty()) {
        input["assigneeId"] = resolveViaCatalog("assignee", assignee);
    }
    const std::string project = fieldOr("project");
    if (!project.empty()) {
        input["projectId"] = resolveViaCatalog("project", project);
    }
    const std::string labelsCsv = fieldOr("labels");
    if (!labelsCsv.empty()) {
        const nlohmann::json labelIds = BuildLabelIdsFromCsv(labelsCsv, fieldForId("labels"));
        if (!labelIds.empty()) {
            input["labelIds"] = labelIds;
        }
    }
    return PayloadResult::Ok(std::move(input));
}

nlohmann::json BuildCommentCreateInput(const std::string& issueUuid, const std::string& body) {
    nlohmann::json input = nlohmann::json::object();
    input["issueId"] = issueUuid;
    input["body"] = body; // markdown verbatim
    return input;
}

// === Response parsers ===

bool ParseMutationSucceeded(const nlohmann::json& parsed, const char* mutationName, nlohmann::json& outIssue) {
    outIssue = nlohmann::json();
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object()) {
        return false;
    }
    const nlohmann::json& data = parsed["data"];
    if (!data.contains(mutationName) || !data[mutationName].is_object()) {
        return false;
    }
    const nlohmann::json& payload = data[mutationName];
    nlohmann::json::const_iterator issueIt = payload.find("issue");
    if (issueIt != payload.end() && issueIt->is_object()) {
        outIssue = *issueIt;
    }
    return payload.value("success", false);
}

std::string ParseResolvedIssueUuid(const nlohmann::json& parsed, const std::string& identifier, std::string& outError) {
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object() ||
        !parsed["data"].contains("issue") || !parsed["data"]["issue"].is_object()) {
        outError = std::string("Linear issue '") + identifier + "' not found";
        return "";
    }
    const std::string uuid = parsed["data"]["issue"].value("id", std::string());
    if (uuid.empty()) {
        outError = std::string("Linear issue '") + identifier + "' resolved to an empty UUID";
    }
    return uuid;
}

std::string ParseCreatedIssueIdentifier(const nlohmann::json& createdIssue) {
    return createdIssue.is_object() ? createdIssue.value("identifier", std::string()) : std::string();
}

} // namespace linear
} // namespace smatchet
