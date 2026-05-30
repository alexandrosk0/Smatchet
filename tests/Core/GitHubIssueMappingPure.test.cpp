// GitHubIssueMappingPure doctest — slice 1 of docs/plans/shipped/autonomous-debugging-no-creds.md.
//
// This test rig exercises the pure GitHub JSON → CachedTicket mapping path
// end-to-end against a fixture file loaded by tests/support/FakeGitHubFixture.h
// (no HTTP, no PAT, no cpr). The underlying pure helpers — extracted in PR12
// of github-tracker-backend.md — already have direct-call coverage in
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
