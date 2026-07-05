// GitHubIssueMappingPure doctest — slice 1 of docs/plans/shipped/autonomous-debugging-no-creds.md.
//
// This test rig exercises the pure GitHub JSON → CachedTicket mapping path
// end-to-end against a fixture file loaded by tests/support/FakeGitHubFixture.h
// (no HTTP, no PAT, no cpr). The underlying pure helpers from
// github-tracker-backend.md already have direct-call coverage in
// GitHubIssueSearchMapping.test.cpp. This file adds the slice-1 V1.1 cases:
//
//   * basic field mapping (key/summary/status/labels/assignee/author/created)
//   * PR-vs-issue filter (includePullRequests=false drops PR rows)
//   * assignee + state + labels coverage on a PR row
//   * pagination boundary (empty nodes → empty tickets; full fixture → all rows)
//
// Deviation from slice-1 spec: no new GitHubIssueMappingPure.{h,cpp} TU is
// created — the pure helpers already live in GitHubIssueSearchMapping.h /
// GitHubClientHelpers.h / GitHubQueryFromJql.h under those sibling-consistent
// names. The slice's intent — "fixture-driven coverage of the pure mapper" —
// is delivered by this test + the FakeGitHubFixture loader, reusing the
// existing extracted helpers. See PR description § Deviations for the full
// rationale.

#include "FakeGitHubFixture.h"
#include "GitHubIssueSearchMapping.h"
#include "LocalCacheManager.h" // CachedTicket

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <vector>

namespace {

// Path resolution — use the CMake-defined SMATCHET_TESTS_REPO_ROOT macro
// (see tests/CMakeLists.txt ~line 200) so the fixture path is cwd-independent.
// Same pattern as slice 3's P4Annotate*E2E.test.cpp.
std::string ResolveFixturePath() {
    return std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures/github/basic-search.json";
}

std::string ResolveMissingFixturePath() {
    return std::string(SMATCHET_TESTS_REPO_ROOT) + "/tests/fixtures/github/does-not-exist.json";
}

std::string GetField(const CachedTicket& t, const std::string& key) {
    const auto it = t.fieldValues.find(key);
    return it == t.fieldValues.end() ? std::string() : it->second;
}

bool HasTicketWithId(const std::vector<CachedTicket>& tickets, const std::string& id) {
    for (const auto& t : tickets) {
        if (t.id == id) {
            return true;
        }
    }
    return false;
}

const CachedTicket* FindTicket(const std::vector<CachedTicket>& tickets, const std::string& id) {
    for (const auto& t : tickets) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("Slice 1 V1.1 — basic field mapping from fixture") {
    smatchet_tests::FakeGitHubFixture fix(ResolveFixturePath());
    REQUIRE(fix.LoadError().empty());
    const auto& tickets = fix.Tickets();
    // Three nodes in the fixture, includePullRequests defaults to true.
    REQUIRE(tickets.size() == 3);

    const CachedTicket* issue = FindTicket(tickets, "alexandrosk0/Smatchet#101");
    REQUIRE(issue != nullptr);
    CHECK(GetField(*issue, "summary") == "Grid scroll jitter at 144 Hz");
    CHECK(GetField(*issue, "status") == "open");
    CHECK(GetField(*issue, "labels") == "bug, perf");
    CHECK(GetField(*issue, "author") == "alexandrosk0");
    CHECK(GetField(*issue, "reporter") == "alexandrosk0");
    CHECK(GetField(*issue, "assignee") == "alice");
    CHECK(GetField(*issue, "milestone") == "0.9");
    CHECK(GetField(*issue, "created") == "2026-05-20T09:00:00Z");
    CHECK(GetField(*issue, "updated") == "2026-05-22T11:30:00Z");
    // Plain issue carries no PR sentinel + no PR-only fields populated.
    CHECK(issue->fieldValues.count(smatchet::github::kIsPullRequestSentinel) == 0);
    CHECK(GetField(*issue, "pr.head").empty());
}

TEST_CASE("Slice 1 V1.1 — PR-vs-issue filter drops PR rows when includePullRequests=false") {
    smatchet_tests::FakeGitHubFixture fix(ResolveFixturePath(), std::string(), std::string(),
                                          /*includePullRequests=*/false);
    REQUIRE(fix.LoadError().empty());
    const auto& tickets = fix.Tickets();
    REQUIRE(tickets.size() == 1);
    CHECK(tickets.front().id == "alexandrosk0/Smatchet#101");
    // No PR rows leaked through.
    CHECK_FALSE(HasTicketWithId(tickets, "alexandrosk0/Smatchet#207"));
    CHECK_FALSE(HasTicketWithId(tickets, "alexandrosk0/Smatchet#188"));
}

TEST_CASE("Slice 1 V1.1 — assignee + state + labels coverage on PR rows") {
    smatchet_tests::FakeGitHubFixture fix(ResolveFixturePath());
    REQUIRE(fix.LoadError().empty());
    const auto& tickets = fix.Tickets();

    // Open PR: [PR] prefix on summary, state=open, head/base refs, mergeable=true, draft=false.
    const CachedTicket* openPr = FindTicket(tickets, "alexandrosk0/Smatchet#207");
    REQUIRE(openPr != nullptr);
    CHECK(GetField(*openPr, "summary") == "[PR] feat: deterministic GitHub backend");
    CHECK(GetField(*openPr, "status") == "open");
    CHECK(GetField(*openPr, "assignee") == "bob");
    CHECK(GetField(*openPr, "labels") == "tracker");
    CHECK(GetField(*openPr, "pr.head") == "slice-1-github-fake-backend");
    CHECK(GetField(*openPr, "pr.base") == "develop");
    CHECK(GetField(*openPr, "pr.mergeable") == "true");
    CHECK(GetField(*openPr, "pr.draft") == "false");
    // Sentinel was stripped by MapGraphQlNodesToTickets before return.
    CHECK(openPr->fieldValues.count(smatchet::github::kIsPullRequestSentinel) == 0);

    // Merged PR: status=merged-PR (CLOSED + mergedAt set in GraphQL → REST closed + merged_at).
    const CachedTicket* mergedPr = FindTicket(tickets, "alexandrosk0/Smatchet#188");
    REQUIRE(mergedPr != nullptr);
    CHECK(GetField(*mergedPr, "summary") == "[PR] fix: merge-watcher CR-NONE fallthrough");
    CHECK(GetField(*mergedPr, "status") == "merged-PR");
    CHECK(GetField(*mergedPr, "assignee").empty());
    CHECK(GetField(*mergedPr, "labels") == "bug");
    // mergeable=UNKNOWN in GraphQL → REST null → "computing" downstream.
    CHECK(GetField(*mergedPr, "pr.mergeable") == "computing");
}

TEST_CASE("Slice 1 V1.1 — pagination boundary: empty nodes → empty tickets") {
    // Inline minimal fixture exercising the empty-page short-circuit. Direct
    // call to MapGraphQlNodesToTickets to avoid creating a separate file.
    nlohmann::json emptyNodes = nlohmann::json::array();
    const auto tickets = smatchet::github::MapGraphQlNodesToTickets(emptyNodes, "owner", "repo",
                                                                    /*includePullRequests=*/true);
    CHECK(tickets.empty());
}

TEST_CASE("Slice 1 V1.1 — pagination boundary: malformed / non-array input is tolerant") {
    // Live caller protects against non-array search.nodes via outFetchError;
    // the pure mapper itself returns empty on any non-array, no throw.
    nlohmann::json notAnArray = nlohmann::json::object();
    notAnArray["oops"] = "not an array";
    const auto tickets = smatchet::github::MapGraphQlNodesToTickets(notAnArray, "owner", "repo", true);
    CHECK(tickets.empty());

    nlohmann::json nullJson;
    const auto tickets2 = smatchet::github::MapGraphQlNodesToTickets(nullJson, "owner", "repo", true);
    CHECK(tickets2.empty());
}

TEST_CASE("Slice 1 V1.1 — fixture load failure surfaces a diagnostic instead of crashing") {
    smatchet_tests::FakeGitHubFixture missing(ResolveMissingFixturePath());
    CHECK_FALSE(missing.LoadError().empty());
    CHECK(missing.Tickets().empty());
    // ConfigureFakeClient() refuses to build a client when the fixture failed to load.
    CHECK(missing.ConfigureFakeClient() == nullptr);
}

TEST_CASE("Slice 1 V1.1 — FakeGitHubFixture wires tickets into a scripted FakeTrackerClient") {
    smatchet_tests::FakeGitHubFixture fix(ResolveFixturePath());
    REQUIRE(fix.LoadError().empty());
    auto client = fix.ConfigureFakeClient();
    REQUIRE(client != nullptr);
    CHECK(client->GetTrackerType() == "GitHub");
    bool fullSync = false;
    std::string fetchError;
    std::string warning;
    const auto fetched = client->FetchIssues(&fullSync, nullptr, nullptr, &fetchError, &warning);
    CHECK(fullSync);
    CHECK(fetchError.empty());
    CHECK(warning.empty());
    CHECK(fetched.size() == 3);
    CHECK(client->FetchIssuesCallCount() == 1);
}

// --- Slice 0 WS2 of multi-grid-tabs — null / missing-relation / empty-optional
// mapping edges on the REST-shape mapper + the PR enrichment + the commit mapper.
// Characterization only: these pin CURRENT behaviour ahead of the multi-pane
// refactor; a surprising result here is documented, not "fixed".

TEST_CASE("WS2 edge — explicit-null assignee/user/body map to empty strings") {
    nlohmann::json issue;
    issue["number"] = 41;
    issue["title"] = "Null relations";
    issue["state"] = "open";
    issue["assignee"] = nullptr;
    issue["user"] = nullptr;
    issue["body"] = nullptr;

    const CachedTicket t = smatchet::github::MapIssueOrPullRequestJsonToCachedTicket(issue, "owner", "repo");
    CHECK(t.id == "owner/repo#41");
    CHECK(GetField(t, "assignee").empty());
    CHECK(GetField(t, "reporter").empty());
    CHECK(GetField(t, "author").empty());
    CHECK(GetField(t, "description").empty());
}

TEST_CASE("WS2 edge — null milestone leaves the key ABSENT while null assignee writes empty (characterized)") {
    // CHARACTERIZATION: the mapper is asymmetric — `assignee` is always written
    // (empty string on null) but `milestone` is only written when the relation is
    // a present object, so a null milestone leaves fieldValues without the key.
    // Grid code must treat absent-key and empty-string as equivalent. Pinned.
    nlohmann::json issue;
    issue["number"] = 42;
    issue["title"] = "Milestone asymmetry";
    issue["state"] = "open";
    issue["assignee"] = nullptr;
    issue["milestone"] = nullptr;

    const CachedTicket t = smatchet::github::MapIssueOrPullRequestJsonToCachedTicket(issue, "owner", "repo");
    CHECK(t.fieldValues.count("assignee") == 1);
    CHECK(t.fieldValues.count("milestone") == 0);
}

TEST_CASE("WS2 edge — label entries that are null or nameless are skipped without separator artifacts") {
    nlohmann::json issue;
    issue["number"] = 43;
    issue["title"] = "Label holes";
    issue["state"] = "open";
    issue["labels"] = nlohmann::json::array({nullptr, {{"color", "red"}}, {{"name", "bug"}}, {{"name", "ui"}}});

    const CachedTicket t = smatchet::github::MapIssueOrPullRequestJsonToCachedTicket(issue, "owner", "repo");
    CHECK(GetField(t, "labels") == "bug, ui");
}

TEST_CASE("WS2 edge — PR enrichment: null mergeable → 'computing', missing draft → empty") {
    nlohmann::json issue;
    issue["number"] = 44;
    issue["title"] = "PR nulls";
    issue["state"] = "open";
    issue["pull_request"] = nlohmann::json::object();

    CachedTicket t = smatchet::github::MapIssueOrPullRequestJsonToCachedTicket(issue, "owner", "repo");

    nlohmann::json prDetail;
    prDetail["head"] = nullptr; // missing-relation branch refs
    prDetail["base"] = {{"ref", "develop"}};
    prDetail["mergeable"] = nullptr; // GitHub still computing
    // no "draft" key at all
    smatchet::github::EnrichPullRequestFieldsFromJson(t, prDetail);

    CHECK(GetField(t, "pr.head").empty());
    CHECK(GetField(t, "pr.base") == "develop");
    CHECK(GetField(t, "pr.mergeable") == "computing");
    CHECK(GetField(t, "pr.draft").empty());
}

TEST_CASE("WS2 edge — PR enrichment with a non-object payload writes no pr.* keys at all") {
    nlohmann::json issue;
    issue["number"] = 45;
    issue["title"] = "PR no detail";
    issue["state"] = "open";
    issue["pull_request"] = nlohmann::json::object();

    CachedTicket t = smatchet::github::MapIssueOrPullRequestJsonToCachedTicket(issue, "owner", "repo");
    smatchet::github::EnrichPullRequestFieldsFromJson(t, nlohmann::json()); // null payload

    CHECK(t.fieldValues.count("pr.head") == 0);
    CHECK(t.fieldValues.count("pr.base") == 0);
    CHECK(t.fieldValues.count("pr.mergeable") == 0);
    CHECK(t.fieldValues.count("pr.draft") == 0);
}

TEST_CASE("WS2 edge — commit mapper tolerates a missing nested commit object") {
    nlohmann::json commit;
    commit["sha"] = "abcdef1234567890";
    // no nested "commit", no author/committer/parents/verification

    const CachedTicket t = smatchet::github::MapCommitJsonToCachedTicket(commit, "owner", "repo");
    CHECK(t.id == "owner/repo@abcdef1234567890");
    CHECK(GetField(t, "github.kind") == "commit");
    CHECK(GetField(t, "status") == "commit");
    CHECK(GetField(t, "summary") == "[commit abcdef1] "); // empty subject — characterized
    CHECK(GetField(t, "author").empty());
    CHECK(GetField(t, "commit.parents").empty());
    CHECK(GetField(t, "commit.verified").empty());
}

TEST_CASE("WS2 edge — commit mapper: null top-level author falls back to the git author name") {
    nlohmann::json commit;
    commit["sha"] = "1234567deadbeef";
    commit["author"] = nullptr; // GitHub couldn't resolve the email to an account
    commit["commit"] = {{"message", "fix: a thing\n\nbody"},
                        {"author", {{"name", "Git Author"}, {"date", "2026-06-01T00:00:00Z"}}}};

    const CachedTicket t = smatchet::github::MapCommitJsonToCachedTicket(commit, "owner", "repo");
    CHECK(GetField(t, "author") == "Git Author");
    CHECK(GetField(t, "reporter") == "Git Author");
    CHECK(GetField(t, "summary") == "[commit 1234567] fix: a thing");
    CHECK(GetField(t, "created") == "2026-06-01T00:00:00Z");
}

TEST_CASE("WS2 edge — GraphQL node with empty assignees.nodes maps to an empty assignee") {
    nlohmann::json node;
    node["__typename"] = "Issue";
    node["number"] = 46;
    node["title"] = "No assignees";
    node["state"] = "OPEN";
    node["assignees"] = {{"nodes", nlohmann::json::array()}};

    const auto tickets =
        smatchet::github::MapGraphQlNodesToTickets(nlohmann::json::array({node}), "owner", "repo", true);
    REQUIRE(tickets.size() == 1);
    CHECK(GetField(tickets[0], "assignee").empty());
}
