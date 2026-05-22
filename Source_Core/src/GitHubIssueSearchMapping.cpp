#include "GitHubIssueSearchMapping.h"

#include <cstddef>
#include <cstdint>
#include <string>

// PR12 — pure-logic JSON → CachedTicket mapping. Extracted out of
// GitHubIssueSearch.cpp so doctest can exercise it without cpr.

namespace smatchet {
namespace github {

namespace {

// Truncate description bodies above this size — saves memory + log volume.
// Mirrors the constant in GitHubIssueSearch.cpp; kept private here.
constexpr std::size_t kGitHubBodyMaxBytes = 4096;

std::string JsonString(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) {
        return std::string();
    }
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return std::string();
    }
    if (it->is_string()) {
        return it->get<std::string>();
    }
    try {
        return it->dump();
    } catch (...) {
        return std::string();
    }
}

std::string IssueNumberString(const nlohmann::json& issue) {
    if (issue.is_object() && issue.contains("number") && issue["number"].is_number_integer()) {
        return std::to_string(issue["number"].get<std::int64_t>());
    }
    return std::string("0");
}

// True iff `issue` carries the PR sub-object (the GitHub-API marker that
// distinguishes PRs from plain issues on shared endpoints).
bool IsPullRequest(const nlohmann::json& issue) {
    return issue.is_object() && issue.contains("pull_request") && issue["pull_request"].is_object();
}

// Pillar-3 helper: safe nested member lookup. Returns the string value of
// `parent.child` when both objects exist and the child is a string. Returns
// empty otherwise.
std::string JsonNestedString(const nlohmann::json& parent, const char* outerKey, const char* innerKey) {
    if (!parent.is_object()) {
        return std::string();
    }
    const auto it = parent.find(outerKey);
    if (it == parent.end() || !it->is_object()) {
        return std::string();
    }
    return JsonString(*it, innerKey);
}

} // namespace

CachedTicket MapIssueOrPullRequestJsonToCachedTicket(const nlohmann::json& issue, const std::string& ownerHint,
                                                     const std::string& repoHint) {
    CachedTicket ticket;
    std::string owner = ownerHint;
    std::string repo = repoHint;

    // Cross-repo path embeds the repo in `repository_url` of the form
    // "https://api.github.com/repos/<owner>/<repo>".
    const std::string repoUrl = JsonString(issue, "repository_url");
    if (!repoUrl.empty()) {
        const std::size_t reposPos = repoUrl.find("/repos/");
        if (reposPos != std::string::npos) {
            const std::string tail = repoUrl.substr(reposPos + 7);
            const std::size_t slash = tail.find('/');
            if (slash != std::string::npos) {
                owner = tail.substr(0, slash);
                repo = tail.substr(slash + 1);
            }
        }
    }

    const std::string number = IssueNumberString(issue);
    if (!owner.empty() && !repo.empty() && number != "0") {
        ticket.id = owner + "/" + repo + "#" + number;
    } else {
        ticket.id = std::string("#") + number;
    }
    ticket.fieldValues["key"] = ticket.id;

    const bool isPr = IsPullRequest(issue);

    // PR12 — visible `[PR] ` summary prefix so users can tell PRs apart at a
    // glance even when the four pr.* columns aren't selected in the view.
    std::string title = JsonString(issue, "title");
    if (isPr) {
        title = std::string("[PR] ") + title;
    }
    ticket.fieldValues["summary"] = title;

    // PR12 — status encodes the merge state for PRs (open / closed /
    // merged-PR). For plain issues, pass through the raw GitHub state.
    const std::string state = JsonString(issue, "state");
    if (isPr) {
        std::string status;
        if (state == "open") {
            status = "open";
        } else {
            // closed branch — distinguish merged vs not-merged via the
            // `pull_request.merged_at` sub-field (only set on merged PRs).
            const std::string mergedAt = JsonNestedString(issue, "pull_request", "merged_at");
            status = mergedAt.empty() ? std::string("closed") : std::string("merged-PR");
        }
        ticket.fieldValues["status"] = status;
        ticket.fieldValues[kIsPullRequestSentinel] = "1";
    } else {
        ticket.fieldValues["status"] = state;
    }

    if (issue.is_object() && issue.contains("assignee") && issue["assignee"].is_object()) {
        ticket.fieldValues["assignee"] = JsonString(issue["assignee"], "login");
    } else {
        ticket.fieldValues["assignee"] = std::string();
    }

    if (issue.is_object() && issue.contains("user") && issue["user"].is_object()) {
        ticket.fieldValues["reporter"] = JsonString(issue["user"], "login");
    } else {
        ticket.fieldValues["reporter"] = std::string();
    }

    std::string labelStr;
    if (issue.is_object() && issue.contains("labels") && issue["labels"].is_array()) {
        for (const auto& lbl : issue["labels"]) {
            std::string name;
            if (lbl.is_object()) {
                name = JsonString(lbl, "name");
            } else if (lbl.is_string()) {
                name = lbl.get<std::string>();
            }
            if (name.empty()) {
                continue;
            }
            if (!labelStr.empty()) {
                labelStr.append(", ");
            }
            labelStr.append(name);
        }
    }
    ticket.fieldValues["labels"] = labelStr;

    std::string body = JsonString(issue, "body");
    if (body.size() > kGitHubBodyMaxBytes) {
        body.resize(kGitHubBodyMaxBytes);
    }
    ticket.fieldValues["description"] = body;

    ticket.fieldValues["created"] = JsonString(issue, "created_at");
    ticket.fieldValues["updated"] = JsonString(issue, "updated_at");

    if (issue.is_object() && issue.contains("milestone") && issue["milestone"].is_object()) {
        ticket.fieldValues["milestone"] = JsonString(issue["milestone"], "title");
    }

    return ticket;
}

void EnrichPullRequestFieldsFromJson(CachedTicket& ticket, const nlohmann::json& prDetail) {
    if (!prDetail.is_object()) {
        return;
    }
    ticket.fieldValues["pr.head"] = JsonNestedString(prDetail, "head", "ref");
    ticket.fieldValues["pr.base"] = JsonNestedString(prDetail, "base", "ref");

    // mergeable: bool / null. null → still computing on GitHub's side.
    std::string mergeable;
    if (prDetail.contains("mergeable")) {
        const auto& m = prDetail["mergeable"];
        if (m.is_null()) {
            mergeable = "computing";
        } else if (m.is_boolean()) {
            mergeable = m.get<bool>() ? "true" : "false";
        }
    }
    ticket.fieldValues["pr.mergeable"] = mergeable;

    std::string draft;
    if (prDetail.contains("draft") && prDetail["draft"].is_boolean()) {
        draft = prDetail["draft"].get<bool>() ? "true" : "false";
    }
    ticket.fieldValues["pr.draft"] = draft;
}

} // namespace github
} // namespace smatchet
