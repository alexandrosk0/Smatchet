// GitHubIssueSearchMapping doctest — pure JSON → CachedTicket mapping +
// per-PR field enrichment helper. PR12 of docs/design/github-tracker-backend.md.

#include "GitHubIssueSearchMapping.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

using namespace smatchet::github;

namespace {

bool StartsWith(const std::string& haystack, const std::string& prefix) {
    return haystack.size() >= prefix.size() && haystack.compare(0, prefix.size(), prefix) == 0;
}

std::string GetField(const CachedTicket& t, const std::string& key) {
    const auto it = t.fieldValues.find(key);
    return it == t.fieldValues.end() ? std::string() : it->second;
}

} // namespace

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — plain issue carries no [PR] prefix, no sentinel") {
    nlohmann::json issue;
    issue["number"] = 42;
    issue["title"] = "Bug: thing broken";
    issue["state"] = "open";
    issue["repository_url"] = "https://api.github.com/repos/alexandrosk0/Smatchet";

    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "", "");
    CHECK(t.id == "alexandrosk0/Smatchet#42");
    CHECK(GetField(t, "summary") == "Bug: thing broken");
    CHECK_FALSE(StartsWith(GetField(t, "summary"), "[PR] "));
    CHECK(GetField(t, "status") == "open");
    CHECK(t.fieldValues.count(kIsPullRequestSentinel) == 0);
    // PR-only fields stay empty for non-PR rows.
    CHECK(GetField(t, "pr.head").empty());
    CHECK(GetField(t, "pr.base").empty());
    CHECK(GetField(t, "pr.mergeable").empty());
    CHECK(GetField(t, "pr.draft").empty());
}

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — open PR gets [PR] prefix + status=open + sentinel") {
    nlohmann::json issue;
    issue["number"] = 7;
    issue["title"] = "Add feature foo";
    issue["state"] = "open";
    issue["repository_url"] = "https://api.github.com/repos/alexandrosk0/Smatchet";
    issue["pull_request"] = nlohmann::json::object();
    issue["pull_request"]["url"] = "https://api.github.com/repos/alexandrosk0/Smatchet/pulls/7";
    issue["pull_request"]["merged_at"] = nullptr;

    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "", "");
    CHECK(StartsWith(GetField(t, "summary"), "[PR] "));
    CHECK(GetField(t, "summary") == "[PR] Add feature foo");
    CHECK(GetField(t, "status") == "open");
    CHECK(t.fieldValues.count(kIsPullRequestSentinel) == 1);
    CHECK(GetField(t, kIsPullRequestSentinel) == "1");
}

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — merged PR encodes status as merged-PR") {
    nlohmann::json issue;
    issue["number"] = 11;
    issue["title"] = "Merge me";
    issue["state"] = "closed";
    issue["repository_url"] = "https://api.github.com/repos/alexandrosk0/Smatchet";
    issue["pull_request"] = nlohmann::json::object();
    issue["pull_request"]["merged_at"] = "2026-01-01T00:00:00Z";

    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "", "");
    CHECK(GetField(t, "status") == "merged-PR");
}

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — closed-without-merge PR encodes status as closed") {
    nlohmann::json issue;
    issue["number"] = 12;
    issue["title"] = "Abandoned";
    issue["state"] = "closed";
    issue["repository_url"] = "https://api.github.com/repos/alexandrosk0/Smatchet";
    issue["pull_request"] = nlohmann::json::object();
    issue["pull_request"]["merged_at"] = nullptr;

    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "", "");
    CHECK(GetField(t, "status") == "closed");
}

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — single-issue payload uses ownerHint/repoHint") {
    nlohmann::json issue;
    issue["number"] = 99;
    issue["title"] = "Hint-only";
    issue["state"] = "open";
    // no repository_url — single-issue endpoint shape.

    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "octocat", "Hello-World");
    CHECK(t.id == "octocat/Hello-World#99");
}

TEST_CASE("EnrichPullRequestFieldsFromJson — populates head/base/mergeable/draft from /pulls payload") {
    CachedTicket t;
    nlohmann::json pr;
    pr["head"] = nlohmann::json::object();
    pr["head"]["ref"] = "feat/foo";
    pr["base"] = nlohmann::json::object();
    pr["base"]["ref"] = "develop";
    pr["mergeable"] = true;
    pr["draft"] = false;

    EnrichPullRequestFieldsFromJson(t, pr);
    CHECK(GetField(t, "pr.head") == "feat/foo");
    CHECK(GetField(t, "pr.base") == "develop");
    CHECK(GetField(t, "pr.mergeable") == "true");
    CHECK(GetField(t, "pr.draft") == "false");
}

TEST_CASE("EnrichPullRequestFieldsFromJson — null mergeable encodes as 'computing'") {
    CachedTicket t;
    nlohmann::json pr;
    pr["head"] = nlohmann::json::object();
    pr["head"]["ref"] = "feat/foo";
    pr["base"] = nlohmann::json::object();
    pr["base"]["ref"] = "develop";
    pr["mergeable"] = nullptr;
    pr["draft"] = true;

    EnrichPullRequestFieldsFromJson(t, pr);
    CHECK(GetField(t, "pr.mergeable") == "computing");
    CHECK(GetField(t, "pr.draft") == "true");
}

TEST_CASE("EnrichPullRequestFieldsFromJson — false mergeable encodes as 'false'") {
    CachedTicket t;
    nlohmann::json pr;
    pr["mergeable"] = false;
    EnrichPullRequestFieldsFromJson(t, pr);
    CHECK(GetField(t, "pr.mergeable") == "false");
}

TEST_CASE("EnrichPullRequestFieldsFromJson — missing fields leave empty strings (Pillar 3 tolerance)") {
    CachedTicket t;
    nlohmann::json pr = nlohmann::json::object();
    EnrichPullRequestFieldsFromJson(t, pr);
    CHECK(GetField(t, "pr.head").empty());
    CHECK(GetField(t, "pr.base").empty());
    CHECK(GetField(t, "pr.mergeable").empty());
    CHECK(GetField(t, "pr.draft").empty());
}

TEST_CASE("EnrichPullRequestFieldsFromJson — non-object input is a no-op") {
    CachedTicket t;
    nlohmann::json pr = "not-an-object";
    EnrichPullRequestFieldsFromJson(t, pr);
    CHECK(t.fieldValues.empty());
}

// PR12 Strategy C — GraphQL node → REST shape adapter tests.

TEST_CASE("MapGraphQlNodeToRestShape — Issue node maps to REST issue shape") {
    nlohmann::json node;
    node["__typename"] = "Issue";
    node["number"] = 42;
    node["title"] = "Bug: thing broken";
    node["state"] = "OPEN";
    node["body"] = "details";
    node["createdAt"] = "2026-01-01T00:00:00Z";
    node["updatedAt"] = "2026-01-02T00:00:00Z";
    node["author"] = nlohmann::json::object();
    node["author"]["login"] = "octocat";
    node["repository"] = nlohmann::json::object();
    node["repository"]["name"] = "Smatchet";
    node["repository"]["owner"] = nlohmann::json::object();
    node["repository"]["owner"]["login"] = "alexandrosk0";

    const nlohmann::json rest = MapGraphQlNodeToRestShape(node);
    CHECK(rest["number"].get<int>() == 42);
    CHECK(rest["title"].get<std::string>() == "Bug: thing broken");
    CHECK(rest["state"].get<std::string>() == "open");
    CHECK(rest["body"].get<std::string>() == "details");
    CHECK(rest["created_at"].get<std::string>() == "2026-01-01T00:00:00Z");
    CHECK(rest["updated_at"].get<std::string>() == "2026-01-02T00:00:00Z");
    CHECK(rest["user"]["login"].get<std::string>() == "octocat");
    CHECK(rest["repository_url"].get<std::string>() == "https://api.github.com/repos/alexandrosk0/Smatchet");
    // Issue nodes do NOT carry the `pull_request` marker.
    CHECK(rest.find("pull_request") == rest.end());
}

TEST_CASE("MapGraphQlNodeToRestShape — PullRequest node carries pull_request{merged_at}") {
    nlohmann::json node;
    node["__typename"] = "PullRequest";
    node["number"] = 7;
    node["title"] = "Add foo";
    node["state"] = "MERGED";
    node["mergedAt"] = "2026-01-03T12:00:00Z";
    node["repository"] = nlohmann::json::object();
    node["repository"]["name"] = "Smatchet";
    node["repository"]["owner"] = nlohmann::json::object();
    node["repository"]["owner"]["login"] = "alexandrosk0";

    const nlohmann::json rest = MapGraphQlNodeToRestShape(node);
    // MERGED → REST "closed" (REST never returns "merged" — merge state is in
    // pull_request.merged_at).
    CHECK(rest["state"].get<std::string>() == "closed");
    REQUIRE(rest.contains("pull_request"));
    CHECK(rest["pull_request"]["merged_at"].get<std::string>() == "2026-01-03T12:00:00Z");
}

TEST_CASE("MapGraphQlNodeToRestShape — open PR carries pull_request{merged_at:null}") {
    nlohmann::json node;
    node["__typename"] = "PullRequest";
    node["number"] = 8;
    node["title"] = "WIP";
    node["state"] = "OPEN";
    // mergedAt omitted (open PR has no mergedAt)
    const nlohmann::json rest = MapGraphQlNodeToRestShape(node);
    CHECK(rest["state"].get<std::string>() == "open");
    REQUIRE(rest.contains("pull_request"));
    CHECK(rest["pull_request"]["merged_at"].is_null());
}

TEST_CASE("MapGraphQlNodeToRestShape — assignees.nodes[0].login flattens to assignee.login") {
    nlohmann::json node;
    node["__typename"] = "Issue";
    node["number"] = 1;
    node["title"] = "t";
    node["state"] = "OPEN";
    node["assignees"] = nlohmann::json::object();
    nlohmann::json assigneesNodes = nlohmann::json::array();
    nlohmann::json a = nlohmann::json::object();
    a["login"] = "alice";
    assigneesNodes.push_back(a);
    node["assignees"]["nodes"] = assigneesNodes;

    const nlohmann::json rest = MapGraphQlNodeToRestShape(node);
    REQUIRE(rest.contains("assignee"));
    CHECK(rest["assignee"]["login"].get<std::string>() == "alice");
}

TEST_CASE("MapGraphQlNodeToRestShape — labels.nodes[].name flattens to labels[].name") {
    nlohmann::json node;
    node["__typename"] = "Issue";
    node["number"] = 1;
    node["title"] = "t";
    node["state"] = "OPEN";
    node["labels"] = nlohmann::json::object();
    nlohmann::json labelsNodes = nlohmann::json::array();
    nlohmann::json l1 = nlohmann::json::object();
    l1["name"] = "bug";
    nlohmann::json l2 = nlohmann::json::object();
    l2["name"] = "p0";
    labelsNodes.push_back(l1);
    labelsNodes.push_back(l2);
    node["labels"]["nodes"] = labelsNodes;

    const nlohmann::json rest = MapGraphQlNodeToRestShape(node);
    REQUIRE(rest.contains("labels"));
    REQUIRE(rest["labels"].is_array());
    REQUIRE(rest["labels"].size() == 2);
    CHECK(rest["labels"][0]["name"].get<std::string>() == "bug");
    CHECK(rest["labels"][1]["name"].get<std::string>() == "p0");
}

TEST_CASE("MapGraphQlNodeToRestShape — milestone{title} maps") {
    nlohmann::json node;
    node["__typename"] = "Issue";
    node["number"] = 1;
    node["title"] = "t";
    node["state"] = "OPEN";
    node["milestone"] = nlohmann::json::object();
    node["milestone"]["title"] = "v1.0";

    const nlohmann::json rest = MapGraphQlNodeToRestShape(node);
    REQUIRE(rest.contains("milestone"));
    CHECK(rest["milestone"]["title"].get<std::string>() == "v1.0");
}

TEST_CASE("MapGraphQlNodeToRestShape — non-object input returns empty object") {
    nlohmann::json node = "not-an-object";
    const nlohmann::json rest = MapGraphQlNodeToRestShape(node);
    CHECK(rest.is_object());
    CHECK(rest.empty());
}

// Integration: GraphQL adapter → existing CachedTicket mapper end-to-end.
TEST_CASE("MapGraphQlNodeToRestShape — composes with MapIssueOrPullRequestJsonToCachedTicket for merged PR") {
    nlohmann::json node;
    node["__typename"] = "PullRequest";
    node["number"] = 7;
    node["title"] = "Add foo";
    node["state"] = "MERGED";
    node["mergedAt"] = "2026-01-03T12:00:00Z";
    node["repository"] = nlohmann::json::object();
    node["repository"]["name"] = "Smatchet";
    node["repository"]["owner"] = nlohmann::json::object();
    node["repository"]["owner"]["login"] = "alexandrosk0";

    const nlohmann::json rest = MapGraphQlNodeToRestShape(node);
    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(rest, "", "");
    CHECK(t.id == "alexandrosk0/Smatchet#7");
    CHECK(StartsWith(GetField(t, "summary"), "[PR] "));
    CHECK(GetField(t, "status") == "merged-PR");
    CHECK(t.fieldValues.count(kIsPullRequestSentinel) == 1);
}

// MapGraphQlPullRequestNodeToRestPrShape tests.

TEST_CASE("MapGraphQlPullRequestNodeToRestPrShape — MERGEABLE → true, isDraft passthrough") {
    nlohmann::json node;
    node["__typename"] = "PullRequest";
    node["headRefName"] = "feat/foo";
    node["baseRefName"] = "develop";
    node["mergeable"] = "MERGEABLE";
    node["isDraft"] = false;

    const nlohmann::json pr = MapGraphQlPullRequestNodeToRestPrShape(node);
    REQUIRE(pr.is_object());
    CHECK(pr["head"]["ref"].get<std::string>() == "feat/foo");
    CHECK(pr["base"]["ref"].get<std::string>() == "develop");
    CHECK(pr["mergeable"].is_boolean());
    CHECK(pr["mergeable"].get<bool>() == true);
    CHECK(pr["draft"].get<bool>() == false);
}

TEST_CASE("MapGraphQlPullRequestNodeToRestPrShape — CONFLICTING → false") {
    nlohmann::json node;
    node["__typename"] = "PullRequest";
    node["mergeable"] = "CONFLICTING";
    node["isDraft"] = true;
    const nlohmann::json pr = MapGraphQlPullRequestNodeToRestPrShape(node);
    CHECK(pr["mergeable"].is_boolean());
    CHECK(pr["mergeable"].get<bool>() == false);
    CHECK(pr["draft"].get<bool>() == true);
}

TEST_CASE("MapGraphQlPullRequestNodeToRestPrShape — UNKNOWN → null (computing)") {
    nlohmann::json node;
    node["__typename"] = "PullRequest";
    node["mergeable"] = "UNKNOWN";
    node["isDraft"] = false;
    const nlohmann::json pr = MapGraphQlPullRequestNodeToRestPrShape(node);
    CHECK(pr["mergeable"].is_null());
}

TEST_CASE("MapGraphQlPullRequestNodeToRestPrShape — Issue node returns null") {
    nlohmann::json node;
    node["__typename"] = "Issue";
    const nlohmann::json pr = MapGraphQlPullRequestNodeToRestPrShape(node);
    CHECK(pr.is_null());
}

TEST_CASE("MapGraphQlPullRequestNodeToRestPrShape — end-to-end PR enrichment via Enrich helper") {
    nlohmann::json node;
    node["__typename"] = "PullRequest";
    node["headRefName"] = "feat/foo";
    node["baseRefName"] = "develop";
    node["mergeable"] = "MERGEABLE";
    node["isDraft"] = false;

    const nlohmann::json prShape = MapGraphQlPullRequestNodeToRestPrShape(node);
    CachedTicket t;
    EnrichPullRequestFieldsFromJson(t, prShape);
    CHECK(GetField(t, "pr.head") == "feat/foo");
    CHECK(GetField(t, "pr.base") == "develop");
    CHECK(GetField(t, "pr.mergeable") == "true");
    CHECK(GetField(t, "pr.draft") == "false");
}

TEST_CASE("MapGraphQlPullRequestNodeToRestPrShape → Enrich — UNKNOWN encodes as 'computing'") {
    nlohmann::json node;
    node["__typename"] = "PullRequest";
    node["headRefName"] = "x";
    node["baseRefName"] = "y";
    node["mergeable"] = "UNKNOWN";
    node["isDraft"] = false;
    const nlohmann::json prShape = MapGraphQlPullRequestNodeToRestPrShape(node);
    CachedTicket t;
    EnrichPullRequestFieldsFromJson(t, prShape);
    CHECK(GetField(t, "pr.mergeable") == "computing");
}
