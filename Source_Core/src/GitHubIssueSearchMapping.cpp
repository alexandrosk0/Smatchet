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

namespace {

// Map GraphQL IssueState/PullRequestState ("OPEN" | "CLOSED" | "MERGED") to
// the REST-shape lower-case state expected by MapIssueOrPullRequestJsonToCachedTicket.
// Note: REST never returns "merged" — it returns "closed" + pull_request.merged_at
// for merged PRs. We preserve that contract: MERGED → "closed" + mergedAt set.
std::string GraphQlStateToRest(const std::string& gqlState) {
    if (gqlState == "OPEN") {
        return "open";
    }
    // CLOSED + MERGED both map to "closed"; merged-vs-not is disambiguated
    // downstream via pull_request.merged_at on the same item.
    return "closed";
}

// Pull a string from `obj[key]` only when present + a string. Empty on miss.
std::string MaybeString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) {
        return std::string();
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

} // namespace

nlohmann::json MapGraphQlNodeToRestShape(const nlohmann::json& node) {
    nlohmann::json out = nlohmann::json::object();
    if (!node.is_object()) {
        return out;
    }

    const std::string typeName = MaybeString(node, "__typename");
    const bool isPr = (typeName == "PullRequest");

    // number (GraphQL returns int64-compatible scalar)
    if (node.contains("number") && node["number"].is_number_integer()) {
        out["number"] = node["number"].get<std::int64_t>();
    }
    out["title"] = MaybeString(node, "title");
    out["body"] = MaybeString(node, "body");
    out["created_at"] = MaybeString(node, "createdAt");
    out["updated_at"] = MaybeString(node, "updatedAt");

    // state — GraphQL uses uppercase; REST mapper expects lowercase.
    out["state"] = GraphQlStateToRest(MaybeString(node, "state"));

    // repository{owner.login, name} → repository_url
    if (node.contains("repository") && node["repository"].is_object()) {
        const auto& repo = node["repository"];
        std::string ownerLogin;
        if (repo.contains("owner") && repo["owner"].is_object()) {
            ownerLogin = MaybeString(repo["owner"], "login");
        }
        const std::string repoName = MaybeString(repo, "name");
        if (!ownerLogin.empty() && !repoName.empty()) {
            out["repository_url"] = std::string("https://api.github.com/repos/") + ownerLogin + "/" + repoName;
        }
    }

    // author{login} → user{login}
    if (node.contains("author") && node["author"].is_object()) {
        nlohmann::json user = nlohmann::json::object();
        user["login"] = MaybeString(node["author"], "login");
        out["user"] = user;
    }

    // assignees.nodes[0].login → assignee{login}  (REST shape exposes only the
    // first assignee on the legacy `assignee` field; we mirror that contract.)
    if (node.contains("assignees") && node["assignees"].is_object()) {
        const auto& nodes = node["assignees"].value("nodes", nlohmann::json::array());
        if (nodes.is_array() && !nodes.empty() && nodes.front().is_object()) {
            nlohmann::json assignee = nlohmann::json::object();
            assignee["login"] = MaybeString(nodes.front(), "login");
            out["assignee"] = assignee;
        }
    }

    // labels.nodes[].name → labels[]{name}
    if (node.contains("labels") && node["labels"].is_object()) {
        const auto& nodes = node["labels"].value("nodes", nlohmann::json::array());
        nlohmann::json restLabels = nlohmann::json::array();
        if (nodes.is_array()) {
            for (const auto& lbl : nodes) {
                if (!lbl.is_object()) {
                    continue;
                }
                nlohmann::json item = nlohmann::json::object();
                item["name"] = MaybeString(lbl, "name");
                restLabels.push_back(item);
            }
        }
        out["labels"] = restLabels;
    }

    // milestone{title}
    if (node.contains("milestone") && node["milestone"].is_object()) {
        nlohmann::json ms = nlohmann::json::object();
        ms["title"] = MaybeString(node["milestone"], "title");
        out["milestone"] = ms;
    }

    // PR detection: emit the `pull_request` marker sub-object so the existing
    // mapper's IsPullRequest() returns true. Carry `merged_at` through so the
    // merged-PR vs closed-PR discriminator stays accurate.
    if (isPr) {
        nlohmann::json pr = nlohmann::json::object();
        if (node.contains("mergedAt") && node["mergedAt"].is_string()) {
            pr["merged_at"] = node["mergedAt"].get<std::string>();
        } else {
            pr["merged_at"] = nullptr;
        }
        out["pull_request"] = pr;
    }

    return out;
}

nlohmann::json MapGraphQlPullRequestNodeToRestPrShape(const nlohmann::json& node) {
    if (!node.is_object() || MaybeString(node, "__typename") != "PullRequest") {
        return nlohmann::json();
    }
    nlohmann::json out = nlohmann::json::object();

    nlohmann::json head = nlohmann::json::object();
    head["ref"] = MaybeString(node, "headRefName");
    out["head"] = head;

    nlohmann::json base = nlohmann::json::object();
    base["ref"] = MaybeString(node, "baseRefName");
    out["base"] = base;

    // GraphQL `mergeable`: MERGEABLE / CONFLICTING / UNKNOWN.
    // Map to REST shape: true / false / null (null encodes as "computing"
    // downstream in EnrichPullRequestFieldsFromJson).
    if (node.contains("mergeable") && node["mergeable"].is_string()) {
        const std::string m = node["mergeable"].get<std::string>();
        if (m == "MERGEABLE") {
            out["mergeable"] = true;
        } else if (m == "CONFLICTING") {
            out["mergeable"] = false;
        } else {
            // UNKNOWN / anything else → REST null → "computing"
            out["mergeable"] = nullptr;
        }
    } else {
        out["mergeable"] = nullptr;
    }

    if (node.contains("isDraft") && node["isDraft"].is_boolean()) {
        out["draft"] = node["isDraft"].get<bool>();
    } else {
        out["draft"] = false;
    }
    return out;
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
