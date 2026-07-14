#include "GitHubMutationPure.h"

#include "GitHubIssueSearchMapping.h" // kPullRequestSummaryPrefix — shared with the read-path mapper
#include "StringUtil.h"
#include "TrackerFieldPayloadPure.h"

#include <cctype>

namespace smatchet {
namespace github {

namespace {

// Multi-value ids arrive as a JSON string array (the BuildFieldPayload shape) or a
// CSV string (the offline-queue verbatim-string fallback replays the raw cell text).
// Accept both; trim tokens, drop blanks. False on any non-string element.
bool CollectStringSet(const nlohmann::json& value, std::vector<std::string>& out) {
    if (value.is_string()) {
        out = TrackerFieldPayloadPure::SplitCommaSeparatedTrimmed(value.get<std::string>());
        return true;
    }
    if (value.is_array()) {
        for (const auto& element : value) {
            if (!element.is_string()) {
                return false;
            }
            const std::string token = TrimCopy(element.get<std::string>());
            if (!token.empty()) {
                out.push_back(token);
            }
        }
        return true;
    }
    if (value.is_null()) {
        return true; // null clears the set (same contract as an empty array)
    }
    return false;
}

// Scalar ids must be a JSON string (or null = clear). False otherwise.
bool ExtractScalarString(const nlohmann::json& value, std::string& out) {
    if (value.is_string()) {
        out = value.get<std::string>();
        return true;
    }
    if (value.is_null()) {
        out.clear();
        return true;
    }
    return false;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool IsAllDigits(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

using PlanResult = Result<GitHubIssueUpdatePlan, std::string>;

PlanResult ErrNotAString(const std::string& fieldId) {
    return PlanResult::Err("GitHub field '" + fieldId + "' expects a string value");
}

} // namespace

Result<GitHubIssueUpdatePlan, std::string> BuildGitHubIssueUpdatePlan(const nlohmann::json& fields) {
    if (!fields.is_object() || fields.empty()) {
        return PlanResult::Err("GitHub issue update payload is empty");
    }
    GitHubIssueUpdatePlan plan;
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const std::string& id = it.key();
        const nlohmann::json& value = it.value();
        if (id == "summary") {
            std::string title;
            if (!ExtractScalarString(value, title)) {
                return ErrNotAString(id);
            }
            // The grid shows pull-request titles behind a display prefix added by
            // GitHubIssueSearchMapping, and an edit of that cell round-trips it — so
            // strip exactly one leading occurrence, otherwise every pull-request
            // title edit compounds another prefix.
            const std::string prefix = kPullRequestSummaryPrefix;
            if (title.compare(0, prefix.size(), prefix) == 0) {
                title.erase(0, prefix.size());
            }
            if (TrimCopy(title).empty()) {
                return PlanResult::Err("GitHub issues require a non-empty title (summary)");
            }
            plan.PatchBody["title"] = title;
        } else if (id == "description") {
            std::string body;
            if (!ExtractScalarString(value, body)) {
                return ErrNotAString(id);
            }
            plan.PatchBody["body"] = body; // empty clears the body
        } else if (id == "status") {
            std::string state;
            if (!ExtractScalarString(value, state)) {
                return ErrNotAString(id);
            }
            state = ToLowerAscii(TrimCopy(state));
            if (state != "open" && state != "closed") {
                return PlanResult::Err("GitHub state must be 'open' or 'closed' (got '" + state +
                                       "'); PR merge state is read-only");
            }
            plan.PatchBody["state"] = state;
        } else if (id == "assignee") {
            std::vector<std::string> assignees;
            if (!CollectStringSet(value, assignees)) {
                return PlanResult::Err("GitHub field 'assignee' expects login names (string or string array)");
            }
            plan.PatchBody["assignees"] = nlohmann::json(assignees); // empty array unassigns
        } else if (id == "labels") {
            std::vector<std::string> labels;
            if (!CollectStringSet(value, labels)) {
                return PlanResult::Err("GitHub field 'labels' expects label names (string or string array)");
            }
            plan.HasLabelsEdit = true;
            plan.LabelsIntended = labels; // empty set removes every label
        } else if (id == "milestone") {
            std::string milestone;
            if (!ExtractScalarString(value, milestone)) {
                return ErrNotAString(id);
            }
            milestone = TrimCopy(milestone);
            if (milestone.empty()) {
                plan.PatchBody["milestone"] = nullptr; // clear
            } else if (IsAllDigits(milestone) && milestone.size() <= 9) {
                // Already the milestone number (digit-cap keeps stoll in range; a longer
                // digit run is treated as a title and fails resolve with a clean error).
                plan.PatchBody["milestone"] = std::stoll(milestone);
            } else {
                plan.NeedsMilestoneResolve = true; // display title — caller resolves the number
                plan.MilestoneTitle = milestone;
            }
        } else if (id.compare(0, 4, "pv2.") == 0) {
            // Projects v2 edit — BuildFieldPayload embedded the field node id and the
            // ready ProjectV2FieldValue (null = clear); a bare string here means the
            // offline queue's verbatim fallback ran without the catalog, which cannot
            // carry the node id — reject with a pointer at the real path.
            if (!value.is_object() || !value.contains("fieldId") || !value["fieldId"].is_string()) {
                return PlanResult::Err("GitHub project field '" + id +
                                       "' needs the catalog-built payload (fieldId + value); re-apply the edit "
                                       "from the grid");
            }
            GitHubProjectV2Edit edit;
            edit.FieldNodeId = value["fieldId"].get<std::string>();
            edit.Value = value.contains("value") ? value["value"] : nlohmann::json(nullptr);
            plan.ProjectV2Edits.push_back(std::move(edit));
        } else {
            return PlanResult::Err("GitHub field '" + id + "' is not editable");
        }
    }
    return PlanResult::Ok(std::move(plan));
}

std::int64_t FindGitHubMilestoneNumberByTitle(const nlohmann::json& milestonesArray, const std::string& title) {
    if (!milestonesArray.is_array() || title.empty()) {
        return 0;
    }
    for (const auto& milestone : milestonesArray) {
        if (!milestone.is_object()) {
            continue;
        }
        const auto titleIt = milestone.find("title");
        if (titleIt == milestone.end() || !titleIt->is_string() || titleIt->get<std::string>() != title) {
            continue;
        }
        const auto numberIt = milestone.find("number");
        if (numberIt != milestone.end() && numberIt->is_number_integer()) {
            const std::int64_t number = numberIt->get<std::int64_t>();
            if (number > 0) {
                return number;
            }
        }
    }
    return 0;
}

std::vector<std::string> ExtractGitHubIssueLabelNames(const nlohmann::json& issueJson) {
    std::vector<std::string> out;
    if (!issueJson.is_object()) {
        return out;
    }
    const auto labelsIt = issueJson.find("labels");
    if (labelsIt == issueJson.end() || !labelsIt->is_array()) {
        return out;
    }
    for (const auto& label : *labelsIt) {
        std::string name;
        if (label.is_object()) {
            const auto nameIt = label.find("name");
            if (nameIt != label.end() && nameIt->is_string()) {
                name = nameIt->get<std::string>();
            }
        } else if (label.is_string()) {
            name = label.get<std::string>();
        }
        if (!name.empty()) {
            out.push_back(name);
        }
    }
    return out;
}

} // namespace github
} // namespace smatchet
