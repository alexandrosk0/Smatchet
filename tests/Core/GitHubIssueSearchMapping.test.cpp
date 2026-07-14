// GitHubIssueSearchMapping doctest — pure JSON → CachedTicket mapping + per-PR field
// enrichment helper (the mapping slice of docs/plans/shipped/github-tracker-backend.md).

#include "GitHubIssueSearchMapping.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

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

// Strategy C — GraphQL node → REST shape adapter tests.

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

// ---- Latency-fix tests — per-page streaming helper ----

namespace {
nlohmann::json MakeIssueNode(int number, const std::string& title) {
    nlohmann::json n;
    n["__typename"] = "Issue";
    n["number"] = number;
    n["title"] = title;
    n["state"] = "OPEN";
    n["repository"] = nlohmann::json::object();
    n["repository"]["name"] = "Smatchet";
    n["repository"]["owner"] = nlohmann::json::object();
    n["repository"]["owner"]["login"] = "alexandrosk0";
    return n;
}

nlohmann::json MakePrNode(int number, const std::string& title) {
    nlohmann::json n = MakeIssueNode(number, title);
    n["__typename"] = "PullRequest";
    n["headRefName"] = "feat/x";
    n["baseRefName"] = "develop";
    n["mergeable"] = "MERGEABLE";
    n["isDraft"] = false;
    return n;
}
} // namespace

TEST_CASE("MapGraphQlNodesToTickets — maps issues only, no sentinel residue, when includePRs=false") {
    nlohmann::json nodes = nlohmann::json::array();
    nodes.push_back(MakeIssueNode(1, "Issue one"));
    nodes.push_back(MakePrNode(2, "PR two")); // should be filtered out
    nodes.push_back(MakeIssueNode(3, "Issue three"));

    const std::vector<CachedTicket> mapped = MapGraphQlNodesToTickets(nodes, "alexandrosk0", "Smatchet", false);
    CHECK(mapped.size() == 2);
    CHECK(mapped[0].id == "alexandrosk0/Smatchet#1");
    CHECK(mapped[1].id == "alexandrosk0/Smatchet#3");
    for (const auto& t : mapped) {
        CHECK(t.fieldValues.count(kIsPullRequestSentinel) == 0);
    }
}

TEST_CASE("MapGraphQlNodesToTickets — includePRs=true keeps PR nodes with [PR] prefix") {
    nlohmann::json nodes = nlohmann::json::array();
    nodes.push_back(MakeIssueNode(1, "Issue one"));
    nodes.push_back(MakePrNode(2, "PR two"));

    const std::vector<CachedTicket> mapped = MapGraphQlNodesToTickets(nodes, "alexandrosk0", "Smatchet", true);
    CHECK(mapped.size() == 2);
    CHECK(GetField(mapped[0], "summary") == "Issue one");
    CHECK(StartsWith(GetField(mapped[1], "summary"), "[PR] "));
    // pr.* fields populated on the PR row.
    CHECK(GetField(mapped[1], "pr.head") == "feat/x");
    CHECK(GetField(mapped[1], "pr.base") == "develop");
    // Sentinel stripped on both rows.
    CHECK(mapped[0].fieldValues.count(kIsPullRequestSentinel) == 0);
    CHECK(mapped[1].fieldValues.count(kIsPullRequestSentinel) == 0);
}

TEST_CASE("MapGraphQlNodesToTickets — non-array input yields empty vector") {
    nlohmann::json notAnArray = nlohmann::json::object();
    const std::vector<CachedTicket> mapped = MapGraphQlNodesToTickets(notAnArray, "o", "r", true);
    CHECK(mapped.empty());
}

TEST_CASE("MapGraphQlNodesToTickets — skips malformed entries silently") {
    nlohmann::json nodes = nlohmann::json::array();
    nodes.push_back(MakeIssueNode(1, "Good"));
    nodes.push_back(nlohmann::json("scalar-not-object")); // skipped — not an object
    nodes.push_back(nlohmann::json::object()); // skipped — missing __typename treated as Issue but no number → mapper
                                               // may throw; either way not the first slot
    nodes.push_back(MakeIssueNode(2, "Also good"));

    const std::vector<CachedTicket> mapped = MapGraphQlNodesToTickets(nodes, "o", "r", true);
    // Must include at least the two well-formed Issues; malformed entries swallow silently.
    CHECK(mapped.size() >= 2);
    bool sawOne = false, sawTwo = false;
    for (const auto& t : mapped) {
        if (GetField(t, "summary") == "Good")
            sawOne = true;
        if (GetField(t, "summary") == "Also good")
            sawTwo = true;
    }
    CHECK(sawOne);
    CHECK(sawTwo);
}

TEST_CASE("MapGraphQlNodesToTickets — accumulating 4 simulated pages preserves order + total count") {
    // Simulates the per-page emission contract: a worker calls
    // MapGraphQlNodesToTickets once per page and accumulates the results in
    // the order pages arrived. The total + ordering must match what the
    // legacy all-at-once path would have produced.
    std::vector<CachedTicket> accumulated;
    int pageCallbackInvocations = 0;
    int isLastTrueCount = 0;
    auto simulateOnPage = [&](const std::vector<CachedTicket>& page, bool isLast) {
        ++pageCallbackInvocations;
        if (isLast)
            ++isLastTrueCount;
        accumulated.insert(accumulated.end(), page.begin(), page.end());
    };

    const int kPagesToSimulate = 4;
    for (int p = 0; p < kPagesToSimulate; ++p) {
        nlohmann::json nodes = nlohmann::json::array();
        // 2 issues per page → 8 total.
        nodes.push_back(MakeIssueNode(p * 2 + 1, std::string("page ") + std::to_string(p) + " issue 1"));
        nodes.push_back(MakeIssueNode(p * 2 + 2, std::string("page ") + std::to_string(p) + " issue 2"));
        const std::vector<CachedTicket> mapped = MapGraphQlNodesToTickets(nodes, "o", "r", true);
        CHECK(mapped.size() == 2);
        const bool isLast = (p == kPagesToSimulate - 1);
        simulateOnPage(mapped, isLast);
    }

    CHECK(pageCallbackInvocations == kPagesToSimulate);
    CHECK(isLastTrueCount == 1);
    CHECK(accumulated.size() == 8);
    // Order preserved (page 0 issues come before page 3 issues). The mapper
    // resolves owner/repo from the node's repository sub-object when present,
    // so the synthetic owner ("alexandrosk0") + repo ("Smatchet") from
    // MakeIssueNode win over the ownerHint/repoHint arguments.
    CHECK(accumulated.front().id == "alexandrosk0/Smatchet#1");
    CHECK(accumulated.back().id == "alexandrosk0/Smatchet#8");
}

// ---- github-commit-tracker-rows — github.kind stamping ----

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — stamps github.kind=issue on plain issues") {
    nlohmann::json issue;
    issue["number"] = 1;
    issue["title"] = "t";
    issue["state"] = "open";
    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
    CHECK(GetField(t, "github.kind") == "issue");
}

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — stamps github.kind=pull_request on PRs") {
    nlohmann::json issue;
    issue["number"] = 2;
    issue["title"] = "t";
    issue["state"] = "open";
    issue["pull_request"] = nlohmann::json::object();
    issue["pull_request"]["merged_at"] = nullptr;
    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
    CHECK(GetField(t, "github.kind") == "pull_request");
}

// ---- issue body is not clipped below GitHub's max body length (gh#1018) ----

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — 5KB body survives in full (regression gh#1018)") {
    // gh#1018's body was 4490 bytes — above the old 4096 cap, so its tail
    // (the `## Status` checklist) was silently chopped. Pin that any body up
    // to GitHub's documented max is now delivered whole.
    const std::string head(4000, 'a');
    const std::string tail = "## Status\n- [x] done";
    nlohmann::json issue;
    issue["number"] = 1018;
    issue["title"] = "Long body";
    issue["state"] = "open";
    issue["body"] = head + tail;

    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
    const std::string desc = GetField(t, "description");
    CHECK(desc.size() == head.size() + tail.size());
    CHECK(StartsWith(desc, head));
    // The tail (previously clipped) is present.
    CHECK(desc.compare(desc.size() - tail.size(), tail.size(), tail) == 0);
}

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — body at GitHub max (64KB) survives whole") {
    const std::string body(65536, 'x');
    nlohmann::json issue;
    issue["number"] = 1;
    issue["title"] = "t";
    issue["state"] = "open";
    issue["body"] = body;
    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
    CHECK(GetField(t, "description").size() == 65536);
}

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — pathological over-max body is bounded") {
    const std::string body(70000, 'y');
    nlohmann::json issue;
    issue["number"] = 1;
    issue["title"] = "t";
    issue["state"] = "open";
    issue["body"] = body;
    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
    CHECK(GetField(t, "description").size() == 65536);
}

// ---- github-commit-tracker-rows — commit JSON → CachedTicket mapping ----

namespace {
nlohmann::json MakeCommitJson() {
    nlohmann::json c;
    c["sha"] = "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678";
    c["html_url"] = "https://github.com/alexandrosk0/Smatchet/commit/a1b2c3d4e5f60718293a4b5c6d7e8f9012345678";
    c["commit"] = nlohmann::json::object();
    c["commit"]["message"] = "Fix the thing\n\nLonger body paragraph explaining why.";
    c["commit"]["author"] = nlohmann::json::object();
    c["commit"]["author"]["name"] = "Jane Dev";
    c["commit"]["author"]["email"] = "jane@example.com";
    c["commit"]["author"]["date"] = "2026-05-01T10:00:00Z";
    c["commit"]["committer"] = nlohmann::json::object();
    c["commit"]["committer"]["name"] = "Jane Dev";
    c["commit"]["committer"]["email"] = "jane@example.com";
    c["commit"]["committer"]["date"] = "2026-05-01T11:00:00Z";
    c["commit"]["verification"] = nlohmann::json::object();
    c["commit"]["verification"]["verified"] = true;
    c["author"] = nlohmann::json::object();
    c["author"]["login"] = "janedev";
    nlohmann::json parents = nlohmann::json::array();
    nlohmann::json p1 = nlohmann::json::object();
    p1["sha"] = "0011223344556677889900112233445566778899";
    parents.push_back(p1);
    c["parents"] = parents;
    return c;
}
} // namespace

TEST_CASE("MapCommitJsonToCachedTicket — full payload maps all commit columns") {
    const CachedTicket t = MapCommitJsonToCachedTicket(MakeCommitJson(), "alexandrosk0", "Smatchet");
    CHECK(t.id == "alexandrosk0/Smatchet@a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
    CHECK(GetField(t, "key") == t.id);
    CHECK(GetField(t, "github.kind") == "commit");
    CHECK(GetField(t, "summary") == "[commit a1b2c3d] Fix the thing");
    CHECK(StartsWith(GetField(t, "description"), "Fix the thing"));
    CHECK(GetField(t, "status") == "commit");
    // GitHub login preferred over git author name.
    CHECK(GetField(t, "author") == "janedev");
    CHECK(GetField(t, "reporter") == "janedev");
    CHECK(GetField(t, "created") == "2026-05-01T10:00:00Z");
    CHECK(GetField(t, "updated") == "2026-05-01T11:00:00Z");
    CHECK(GetField(t, "commit.sha") == "a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
    CHECK(GetField(t, "commit.short_sha") == "a1b2c3d");
    CHECK(GetField(t, "commit.author_name") == "Jane Dev");
    CHECK(GetField(t, "commit.author_email") == "jane@example.com");
    CHECK(GetField(t, "commit.committer_name") == "Jane Dev");
    CHECK(GetField(t, "commit.committer_email") == "jane@example.com");
    CHECK(GetField(t, "commit.url") ==
          "https://github.com/alexandrosk0/Smatchet/commit/a1b2c3d4e5f60718293a4b5c6d7e8f9012345678");
    CHECK(GetField(t, "commit.parents") == "0011223");
    CHECK(GetField(t, "commit.verified") == "true");
}

TEST_CASE("MapCommitJsonToCachedTicket — falls back to git author name when no GitHub login") {
    nlohmann::json c = MakeCommitJson();
    c.erase("author"); // no resolved GitHub user
    const CachedTicket t = MapCommitJsonToCachedTicket(c, "o", "r");
    CHECK(GetField(t, "author") == "Jane Dev");
    CHECK(GetField(t, "reporter") == "Jane Dev");
}

TEST_CASE("MapCommitJsonToCachedTicket — tolerant of missing nested objects (Pillar 3)") {
    nlohmann::json c;
    c["sha"] = "abcdef1234567890";
    // no `commit`, `author`, `parents`, `verification`
    const CachedTicket t = MapCommitJsonToCachedTicket(c, "o", "r");
    CHECK(t.id == "o/r@abcdef1234567890");
    CHECK(GetField(t, "github.kind") == "commit");
    CHECK(GetField(t, "summary") == "[commit abcdef1] ");
    CHECK(GetField(t, "description").empty());
    CHECK(GetField(t, "author").empty());
    CHECK(GetField(t, "created").empty());
    CHECK(GetField(t, "commit.author_name").empty());
    CHECK(GetField(t, "commit.parents").empty());
    CHECK(GetField(t, "commit.verified").empty());
}

TEST_CASE("MapCommitJsonToCachedTicket — multiple parents join as short SHAs") {
    nlohmann::json c = MakeCommitJson();
    nlohmann::json parents = nlohmann::json::array();
    nlohmann::json p1 = nlohmann::json::object();
    p1["sha"] = "1111111aaaaaaa";
    nlohmann::json p2 = nlohmann::json::object();
    p2["sha"] = "2222222bbbbbbb";
    parents.push_back(p1);
    parents.push_back(p2);
    c["parents"] = parents;
    const CachedTicket t = MapCommitJsonToCachedTicket(c, "o", "r");
    CHECK(GetField(t, "commit.parents") == "1111111, 2222222");
}

TEST_CASE("MapCommitJsonToCachedTicket — unverified commit encodes verified=false") {
    nlohmann::json c = MakeCommitJson();
    c["commit"]["verification"]["verified"] = false;
    const CachedTicket t = MapCommitJsonToCachedTicket(c, "o", "r");
    CHECK(GetField(t, "commit.verified") == "false");
}

// ---- issue-comments PR-A — read-only `comments` count field on issue/PR rows ----

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — REST `comments` integer maps to fieldValues[\"comments\"]") {
    nlohmann::json issue;
    issue["number"] = 42;
    issue["title"] = "Has comments";
    issue["state"] = "open";
    issue["comments"] = 5;

    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
    CHECK(GetField(t, "comments") == "5");
}

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — absent `comments` maps to \"0\"") {
    nlohmann::json issue;
    issue["number"] = 43;
    issue["title"] = "No comment count";
    issue["state"] = "open";
    // no `comments` field

    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
    CHECK(GetField(t, "comments") == "0");
}

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — non-integer `comments` maps to \"0\" (Pillar 3)") {
    SUBCASE("null comments") {
        nlohmann::json issue;
        issue["number"] = 44;
        issue["title"] = "Null count";
        issue["state"] = "open";
        issue["comments"] = nullptr;
        const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
        CHECK(GetField(t, "comments") == "0");
    }
    SUBCASE("string comments") {
        nlohmann::json issue;
        issue["number"] = 45;
        issue["title"] = "String count";
        issue["state"] = "open";
        issue["comments"] = "lots";
        const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
        CHECK(GetField(t, "comments") == "0");
    }
    SUBCASE("float comments") {
        nlohmann::json issue;
        issue["number"] = 46;
        issue["title"] = "Float count";
        issue["state"] = "open";
        issue["comments"] = 2.5;
        const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r");
        CHECK(GetField(t, "comments") == "0");
    }
}

TEST_CASE("MapGraphQlNodeToRestShape — comments{totalCount} flattens to integer `comments`, round-trips to count") {
    nlohmann::json node;
    node["__typename"] = "Issue";
    node["number"] = 7;
    node["title"] = "GraphQL with comments";
    node["state"] = "OPEN";
    node["comments"] = nlohmann::json::object();
    node["comments"]["totalCount"] = 7;
    node["repository"] = nlohmann::json::object();
    node["repository"]["name"] = "Smatchet";
    node["repository"]["owner"] = nlohmann::json::object();
    node["repository"]["owner"]["login"] = "alexandrosk0";

    const nlohmann::json rest = MapGraphQlNodeToRestShape(node);
    REQUIRE(rest.contains("comments"));
    REQUIRE(rest["comments"].is_number_integer());
    CHECK(rest["comments"].get<int>() == 7);

    // Round-trip through the REST mapper → fieldValues["comments"] == "7".
    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(rest, "", "");
    CHECK(GetField(t, "comments") == "7");
}

TEST_CASE("MapGraphQlNodeToRestShape — absent comments does not crash; downstream count is \"0\"") {
    nlohmann::json node;
    node["__typename"] = "Issue";
    node["number"] = 8;
    node["title"] = "GraphQL no comments";
    node["state"] = "OPEN";
    // no `comments` sub-object
    node["repository"] = nlohmann::json::object();
    node["repository"]["name"] = "Smatchet";
    node["repository"]["owner"] = nlohmann::json::object();
    node["repository"]["owner"]["login"] = "alexandrosk0";

    nlohmann::json rest;
    CHECK_NOTHROW(rest = MapGraphQlNodeToRestShape(node));
    // No flat integer `comments` emitted when the GraphQL sub-object is absent.
    CHECK(rest.find("comments") == rest.end());

    // Downstream REST mapper defaults the count to "0".
    const CachedTicket t = MapIssueOrPullRequestJsonToCachedTicket(rest, "", "");
    CHECK(GetField(t, "comments") == "0");
}

// security deep-audit target 12 — the GitHub issue/PR, commit, and GraphQL
// mappers parse JSON straight off a possibly-malicious GitHub API or a MITM, so
// every hostile or type-confused shape must map to a safe default rather than
// throw or over-read. nlohmann member access on a type-confused node throws
// type_error, so a missing guard would surface as an uncaught throw right here.
// The cases below lock the no-throw contract across all four entry points.

TEST_CASE("MapIssueOrPullRequestJsonToCachedTicket — hostile / type-confused input never throws" *
          doctest::test_suite("[high-risk]")) {
    CHECK_NOTHROW(MapIssueOrPullRequestJsonToCachedTicket(nlohmann::json(nullptr), "o", "r"));
    CHECK_NOTHROW(MapIssueOrPullRequestJsonToCachedTicket(nlohmann::json::array(), "o", "r"));
    CHECK_NOTHROW(MapIssueOrPullRequestJsonToCachedTicket(nlohmann::json(7), "o", "r"));

    nlohmann::json issue;
    issue["number"] = "not-a-number";
    issue["title"] = nlohmann::json::array({1, 2});
    issue["state"] = 99;
    issue["body"] = nlohmann::json::object();
    issue["repository_url"] = false;
    issue["user"] = "scalar-not-object";
    issue["assignee"] = 5;
    issue["labels"] = "scalar-not-array";
    issue["milestone"] = nlohmann::json::array();
    issue["pull_request"] = 42;
    issue["created_at"] = nlohmann::json::object();
    issue["comments"] = nlohmann::json::array();
    CHECK_NOTHROW(MapIssueOrPullRequestJsonToCachedTicket(issue, "o", "r"));

    // labels array whose ELEMENTS are the wrong type -- the mapper reads each
    // entry's name member only after a per-element object guard.
    nlohmann::json badLabels;
    badLabels["number"] = 3;
    badLabels["labels"] = nlohmann::json::array({1, "str", nullptr, nlohmann::json::object()});
    CHECK_NOTHROW(MapIssueOrPullRequestJsonToCachedTicket(badLabels, "o", "r"));
}

TEST_CASE("MapCommitJsonToCachedTicket — hostile / type-confused input never throws" *
          doctest::test_suite("[high-risk]")) {
    CHECK_NOTHROW(MapCommitJsonToCachedTicket(nlohmann::json(nullptr), "o", "r"));
    CHECK_NOTHROW(MapCommitJsonToCachedTicket(nlohmann::json::array(), "o", "r"));
    CHECK_NOTHROW(MapCommitJsonToCachedTicket(nlohmann::json("scalar"), "o", "r"));

    nlohmann::json commit;
    commit["sha"] = 12345;
    commit["commit"] = "scalar-not-object";
    commit["author"] = nlohmann::json::array();
    commit["committer"] = 7;
    commit["parents"] = "scalar-not-array";
    commit["html_url"] = nlohmann::json::object();
    CHECK_NOTHROW(MapCommitJsonToCachedTicket(commit, "o", "r"));

    // Nested commit sub-objects with wrong-typed members and a parents array of
    // scalars -- every read is type-guarded before the member access.
    nlohmann::json nested;
    nested["sha"] = "abc";
    nested["commit"] = nlohmann::json::object();
    nested["commit"]["message"] = nlohmann::json::array({1, 2});
    nested["commit"]["author"] = 99;
    nested["commit"]["verification"] = "scalar";
    nested["parents"] = nlohmann::json::array({5, "str", nullptr});
    CHECK_NOTHROW(MapCommitJsonToCachedTicket(nested, "o", "r"));
}

TEST_CASE("MapGraphQlNodeToRestShape / PrShape — hostile node never throws" * doctest::test_suite("[high-risk]")) {
    nlohmann::json rest;
    CHECK_NOTHROW(rest = MapGraphQlNodeToRestShape(nlohmann::json(nullptr)));
    CHECK_NOTHROW(rest = MapGraphQlNodeToRestShape(nlohmann::json::array()));
    CHECK_NOTHROW(rest = MapGraphQlNodeToRestShape(nlohmann::json(7)));

    nlohmann::json node;
    node["__typename"] = 42;
    node["number"] = "not-a-number";
    node["title"] = nlohmann::json::array();
    node["state"] = nlohmann::json::object();
    node["repository"] = "scalar-not-object";
    node["author"] = 5;
    node["assignees"] = "scalar-not-array";
    node["labels"] = 9;
    node["milestone"] = true;
    node["comments"] = "scalar-not-object";
    CHECK_NOTHROW(rest = MapGraphQlNodeToRestShape(node));

    nlohmann::json prNode;
    CHECK_NOTHROW(rest = MapGraphQlPullRequestNodeToRestPrShape(nlohmann::json(nullptr)));
    prNode["__typename"] = "PullRequest";
    prNode["headRefName"] = 1;
    prNode["baseRefName"] = nlohmann::json::array();
    prNode["mergeable"] = nlohmann::json::object();
    prNode["isDraft"] = "not-a-bool";
    CHECK_NOTHROW(rest = MapGraphQlPullRequestNodeToRestPrShape(prNode));
}

TEST_CASE("MapGraphQlNodesToTickets — hostile node array skips bad rows, never throws" *
          doctest::test_suite("[high-risk]")) {
    std::vector<CachedTicket> tickets;
    CHECK_NOTHROW(tickets = MapGraphQlNodesToTickets(nlohmann::json(nullptr), "o", "r", true));
    CHECK_NOTHROW(tickets = MapGraphQlNodesToTickets(nlohmann::json::object(), "o", "r", true));
    CHECK_NOTHROW(tickets = MapGraphQlNodesToTickets(nlohmann::json("scalar"), "o", "r", true));

    nlohmann::json nodes = nlohmann::json::array();
    nodes.push_back(nlohmann::json(nullptr));
    nodes.push_back(nlohmann::json(42));
    nodes.push_back("scalar");
    nodes.push_back(nlohmann::json::array());
    nlohmann::json broken;
    broken["__typename"] = 7;
    broken["number"] = nlohmann::json::array();
    nodes.push_back(broken);
    nlohmann::json good;
    good["__typename"] = "Issue";
    good["number"] = 11;
    good["title"] = "valid graphql row";
    good["state"] = "OPEN";
    good["repository"] = nlohmann::json::object();
    good["repository"]["name"] = "Smatchet";
    good["repository"]["owner"] = nlohmann::json::object();
    good["repository"]["owner"]["login"] = "alexandrosk0";
    nodes.push_back(good);

    CHECK_NOTHROW(tickets = MapGraphQlNodesToTickets(nodes, "o", "r", true));
    // The one well-formed Issue node maps through; every hostile node is skipped.
    REQUIRE(tickets.size() == 1);
    CHECK(tickets[0].id == "alexandrosk0/Smatchet#11");
}

TEST_CASE("AppendGitHubReposAsRemoteProjects — /user/repos page → owner/repo picker rows") {
    using smatchet::github::AppendGitHubReposAsRemoteProjects;
    std::vector<RemoteProject> out;
    SUBCASE("well-formed repos map id/key/displayName; malformed rows are skipped") {
        const nlohmann::json page = nlohmann::json::array({
            nlohmann::json{{"id", 42}, {"full_name", "acme/widgets"}},
            nlohmann::json{{"id", 7}, {"full_name", "acme/tools"}, {"private", true}},
            nlohmann::json{{"id", 9}},                                   // no full_name
            nlohmann::json{{"id", 10}, {"full_name", "no-slash"}},       // not owner/repo shaped
            nlohmann::json{{"id", 11}, {"full_name", "/leading-slash"}}, // empty owner
            nlohmann::json{{"full_name", "acme/no-id"}},                 // no id
            nlohmann::json{{"id", 0}, {"full_name", "acme/zero-id"}},    // non-positive id
            nlohmann::json(nullptr),                                     // hostile element
        });
        AppendGitHubReposAsRemoteProjects(page, out);
        REQUIRE(out.size() == 2);
        CHECK(out[0].id == "42");
        CHECK(out[0].key == "acme/widgets"); // feeds IssueDraft::ProjectKey's owner/repo split
        CHECK(out[0].displayName == "acme/widgets");
        CHECK(out[1].key == "acme/tools");
    }
    SUBCASE("appends across pages; a non-array page is a no-op") {
        out.push_back(RemoteProject());
        AppendGitHubReposAsRemoteProjects(nlohmann::json::object(), out);
        AppendGitHubReposAsRemoteProjects(nlohmann::json(nullptr), out);
        CHECK(out.size() == 1);
        AppendGitHubReposAsRemoteProjects(nlohmann::json::array({nlohmann::json{{"id", 1}, {"full_name", "a/b"}}}),
                                          out);
        CHECK(out.size() == 2);
    }
}
