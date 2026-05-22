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
