#include "GitHubIssueSearchMapping.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure-logic JSON → CachedTicket mapping. Extracted out of
// GitHubIssueSearch.cpp so doctest can exercise it without cpr.

namespace smatchet {
namespace github {

namespace {

// Defensive upper bound on description bodies — caps memory + log volume for a
// pathological payload. Set to GitHub's documented max issue/PR body length
// (65536 chars) so no real body is ever clipped; mirrors the long-text field
// editor buffer (kBufferSize = 64*1024 in TicketFieldEditor_Modal.cpp).
constexpr std::size_t kGitHubBodyMaxBytes = 65536;

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
    } catch (...) { // catch-all-ok: dump on untrusted JSON value
        return std::string();
    }
}

std::string IssueNumberString(const nlohmann::json& issue) {
    if (issue.is_object() && issue.contains("number") && issue["number"].is_number_integer()) {
        return std::to_string(issue["number"].get<std::int64_t>());
    }
    return std::string("0");
}

// issue-comments PR-A — reads the named integer member when present and integral,
// returning 0 otherwise (Pillar 3, no throw). There is no shared JsonInt helper, so
// this mirrors IssueNumberString's local-helper style.
long long GitHubJsonInt(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) {
        return 0;
    }
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_number_integer()) {
        return it->get<long long>();
    }
    return 0;
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

// Resolve the stable ticket id. Cross-repo path embeds the repo in
// `repository_url` of the form "https://api.github.com/repos/<owner>/<repo>".
std::string ResolveTicketId(const nlohmann::json& issue, const std::string& ownerHint, const std::string& repoHint) {
    std::string owner = ownerHint;
    std::string repo = repoHint;

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
        return owner + "/" + repo + "#" + number;
    }
    return std::string("#") + number;
}

// Derive the status string for a PR. For PRs, status encodes the merge state
// (open / closed / merged-PR) via the `pull_request.merged_at` sub-field.
std::string ResolvePullRequestStatus(const nlohmann::json& issue, const std::string& state) {
    if (state == "open") {
        return "open";
    }
    const std::string mergedAt = JsonNestedString(issue, "pull_request", "merged_at");
    return mergedAt.empty() ? std::string("closed") : std::string("merged-PR");
}

// Collect issue labels into the comma-separated string the grid stores.
std::string CollectLabels(const nlohmann::json& issue) {
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
    return labelStr;
}

} // namespace

CachedTicket MapIssueOrPullRequestJsonToCachedTicket(const nlohmann::json& issue, const std::string& ownerHint,
                                                     const std::string& repoHint) {
    CachedTicket ticket;

    ticket.id = ResolveTicketId(issue, ownerHint, repoHint);
    ticket.fieldValues["key"] = ticket.id;

    const bool isPr = IsPullRequest(issue);

    // github-commit-tracker-rows — stamp the row kind so the grid + commit-key
    // router can distinguish issues / PRs / commits without re-parsing the id.
    ticket.fieldValues["github.kind"] = isPr ? std::string("pull_request") : std::string("issue");

    // Visible `[PR] ` summary prefix so users can tell PRs apart at a
    // glance even when the four pr.* columns aren't selected in the view.
    std::string title = JsonString(issue, "title");
    if (isPr) {
        title = std::string(kPullRequestSummaryPrefix) + title;
    }
    ticket.fieldValues["summary"] = title;

    // Status encodes the merge state for PRs (open / closed /
    // merged-PR). For plain issues, pass through the raw GitHub state.
    const std::string state = JsonString(issue, "state");
    if (isPr) {
        ticket.fieldValues["status"] = ResolvePullRequestStatus(issue, state);
        ticket.fieldValues[kIsPullRequestSentinel] = "1";
    } else {
        ticket.fieldValues["status"] = state;
    }

    if (issue.is_object() && issue.contains("assignee") && issue["assignee"].is_object()) {
        ticket.fieldValues["assignee"] = JsonString(issue["assignee"], "login");
    } else {
        ticket.fieldValues["assignee"] = std::string();
    }

    // GitHub calls the creator `author` in the GraphQL/REST surface; Jira calls
    // it `reporter`. Write both so views can opt into either naming.
    std::string creatorLogin;
    if (issue.is_object() && issue.contains("user") && issue["user"].is_object()) {
        creatorLogin = JsonString(issue["user"], "login");
    }
    ticket.fieldValues["reporter"] = creatorLogin;
    ticket.fieldValues["author"] = creatorLogin;

    ticket.fieldValues["labels"] = CollectLabels(issue);

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

    // issue-comments PR-A — read-only comment count for the grid cell. GitHub's
    // /issues + /search/issues payloads carry an integer `comments` field; absent
    // / non-integer → 0. Commit rows leave `comments` unset (no count concept).
    ticket.fieldValues["comments"] = std::to_string(GitHubJsonInt(issue, "comments"));

    return ticket;
}

namespace {

// First line of a (possibly multi-line) commit message. Empty when `msg` is
// empty. Used for the `[commit <short>] <subject>` summary.
std::string FirstLine(const std::string& msg) {
    const std::size_t nl = msg.find('\n');
    if (nl == std::string::npos) {
        return msg;
    }
    return msg.substr(0, nl);
}

// First 7 chars of `sha` (git's conventional abbreviated id), or the whole
// string when shorter.
std::string ShortSha(const std::string& sha) {
    if (sha.size() <= 7) {
        return sha;
    }
    return sha.substr(0, 7);
}

} // namespace

CachedTicket MapCommitJsonToCachedTicket(const nlohmann::json& commit, const std::string& owner,
                                         const std::string& repo) {
    CachedTicket ticket;

    const std::string sha = JsonString(commit, "sha");
    const std::string shortSha = ShortSha(sha);

    if (!owner.empty() && !repo.empty() && !sha.empty()) {
        ticket.id = owner + "/" + repo + "@" + sha;
    } else {
        ticket.id = std::string("@") + sha;
    }
    ticket.fieldValues["key"] = ticket.id;
    ticket.fieldValues["github.kind"] = "commit";

    // Nested `commit` sub-object carries the git metadata (author/committer/
    // message/verification). Tolerant of its absence per Pillar 3.
    nlohmann::json inner = nlohmann::json::object();
    if (commit.is_object()) {
        const auto it = commit.find("commit");
        if (it != commit.end() && it->is_object()) {
            inner = *it;
        }
    }

    std::string message = JsonString(inner, "message");
    const std::string subject = FirstLine(message);
    ticket.fieldValues["summary"] = std::string("[commit ") + shortSha + "] " + subject;

    if (message.size() > kGitHubBodyMaxBytes) {
        message.resize(kGitHubBodyMaxBytes);
    }
    ticket.fieldValues["description"] = message;
    ticket.fieldValues["status"] = "commit";

    // Author/committer name+email come from the git metadata; the GitHub user
    // login (when GitHub resolved the email to an account) lives at the
    // top-level `author`/`committer` objects.
    const std::string gitAuthorName = JsonNestedString(inner, "author", "name");
    const std::string gitAuthorEmail = JsonNestedString(inner, "author", "email");
    const std::string gitCommitterName = JsonNestedString(inner, "committer", "name");
    const std::string gitCommitterEmail = JsonNestedString(inner, "committer", "email");
    const std::string authorDate = JsonNestedString(inner, "author", "date");
    const std::string committerDate = JsonNestedString(inner, "committer", "date");

    std::string authorLogin;
    if (commit.is_object() && commit.contains("author") && commit["author"].is_object()) {
        authorLogin = JsonString(commit["author"], "login");
    }
    const std::string displayAuthor = authorLogin.empty() ? gitAuthorName : authorLogin;
    ticket.fieldValues["author"] = displayAuthor;
    ticket.fieldValues["reporter"] = displayAuthor;
    ticket.fieldValues["created"] = authorDate;
    ticket.fieldValues["updated"] = committerDate;

    // GitHub-specific commit columns.
    ticket.fieldValues["commit.sha"] = sha;
    ticket.fieldValues["commit.short_sha"] = shortSha;
    ticket.fieldValues["commit.author_name"] = gitAuthorName;
    ticket.fieldValues["commit.author_email"] = gitAuthorEmail;
    ticket.fieldValues["commit.committer_name"] = gitCommitterName;
    ticket.fieldValues["commit.committer_email"] = gitCommitterEmail;
    ticket.fieldValues["commit.url"] = JsonString(commit, "html_url");

    // Parents → comma-joined short SHAs (merge commits have ≥2). Empty when the
    // payload omits the list or it isn't an array.
    std::string parents;
    if (commit.is_object() && commit.contains("parents") && commit["parents"].is_array()) {
        for (const auto& p : commit["parents"]) {
            if (!p.is_object()) {
                continue;
            }
            const std::string pSha = ShortSha(JsonString(p, "sha"));
            if (pSha.empty()) {
                continue;
            }
            if (!parents.empty()) {
                parents.append(", ");
            }
            parents.append(pSha);
        }
    }
    ticket.fieldValues["commit.parents"] = parents;

    // verification.verified is a bool → "true"/"false"; empty when absent.
    std::string verified;
    if (inner.is_object() && inner.contains("verification") && inner["verification"].is_object()) {
        const auto& v = inner["verification"];
        const auto it = v.find("verified");
        if (it != v.end() && it->is_boolean()) {
            verified = it->get<bool>() ? "true" : "false";
        }
    }
    ticket.fieldValues["commit.verified"] = verified;

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

    // issue-comments PR-A — GraphQL `comments { totalCount }` → the integer
    // `comments` field the REST mapper (GitHubJsonInt) reads. REST /issues
    // already returns a flat integer `comments`; this lines the GraphQL shape up.
    if (node.contains("comments") && node["comments"].is_object()) {
        const auto& comments = node["comments"];
        const auto it = comments.find("totalCount");
        if (it != comments.end() && it->is_number_integer()) {
            out["comments"] = it->get<std::int64_t>();
        }
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

std::vector<CachedTicket> MapGraphQlNodesToTickets(const nlohmann::json& nodes, const std::string& owner,
                                                   const std::string& repo, bool includePullRequests) {
    std::vector<CachedTicket> out;
    if (!nodes.is_array()) {
        return out;
    }
    out.reserve(nodes.size());
    for (const auto& node : nodes) {
        if (!node.is_object()) {
            continue;
        }
        // Reject nodes missing required identity fields before mapping. Without
        // these guards, GitHub objects lacking __typename or a positive integer
        // `number` would produce bogus tickets (e.g. `#0`).
        const auto tnIt = node.find("__typename");
        const bool hasType = (tnIt != node.end() && tnIt->is_string());
        if (!hasType) {
            continue;
        }
        const std::string typeName = tnIt->get<std::string>();
        const bool isPr = (typeName == "PullRequest");
        const bool isIssue = (typeName == "Issue");
        if (!isPr && !isIssue) {
            continue;
        }
        if (isPr && !includePullRequests) {
            continue;
        }
        const auto numIt = node.find("number");
        if (numIt == node.end() || !numIt->is_number_integer() || numIt->get<long long>() <= 0) {
            continue;
        }
        try {
            const nlohmann::json restShape = MapGraphQlNodeToRestShape(node);
            CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(restShape, owner, repo);
            if (isPr) {
                const nlohmann::json prShape = MapGraphQlPullRequestNodeToRestPrShape(node);
                if (!prShape.is_null()) {
                    EnrichPullRequestFieldsFromJson(t, prShape);
                }
            }
            // Strip the internal sentinel — callers see PR rows via the [PR]
            // summary prefix + pr.* columns, not the sentinel.
            t.fieldValues.erase(kIsPullRequestSentinel);
            out.push_back(std::move(t));
        } catch (const std::exception&) {
            // Malformed node — skip silently. The live caller logs aggregate
            // counts via LOG_INFO so a per-node LOG_WARN here would only
            // duplicate noise (and would force this pure-logic TU to depend
            // on Logger.h, which we deliberately avoid for doctest purity).
        }
    }
    return out;
}

} // namespace github
} // namespace smatchet
