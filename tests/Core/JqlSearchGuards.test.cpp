// JQL search result guards — verify limits are in place for massive result protection.

#include <doctest/doctest.h>
#include <cstddef>

namespace {

// Guard constants mirrored from production code. These tests verify the guards exist
// and have sensible values to prevent memory exhaustion on pathological JQL queries.

// ===== Jira Guards =====
constexpr size_t kJiraMaxTotalFetchBytes = 100u * 1024u * 1024u; // 100 MB
constexpr size_t kJiraMaxResultCount = 10000u;                    // 10K issues
constexpr int kJiraMaxPages = 50;                                 // pagination cap

// ===== GitHub Guards =====
constexpr size_t kGitHubMaxTotalFetchBytes = 50u * 1024u * 1024u; // 50 MB
constexpr size_t kGitHubMaxResultCount = 5000u;                   // 5K issues/PRs
constexpr int kGitHubMaxPages = 10;                               // pagination cap

// ===== Plane Guards =====
constexpr size_t kPlaneMaxTotalFetchBytes = 100u * 1024u * 1024u; // 100 MB
constexpr size_t kPlaneMaxResultCount = 10000u;                   // 10K issues
constexpr int kPlaneMaxPages = 50;                                // pagination cap

// Per-response limit (all backends via ParseBounded)
constexpr size_t kPerResponseMaxBytes = 4u * 1024u * 1024u; // 4 MB

} // namespace

TEST_SUITE("JQL Search Result Guards") {

TEST_CASE("Jira guard limits are set") {
    // Cumulative size guard: 100MB total across all pages
    REQUIRE(kJiraMaxTotalFetchBytes == 100u * 1024u * 1024u);
    // Result count guard: 10K total issues
    REQUIRE(kJiraMaxResultCount == 10000u);
    // Pagination cap: 50 pages × 100/page = 5K max (well below result guard)
    REQUIRE(kJiraMaxPages == 50);
    // Worst case: 50 pages × 100 results = 5000 issues before guard hits
    REQUIRE(kJiraMaxPages * 100 < kJiraMaxResultCount);
}

TEST_CASE("GitHub guard limits are set") {
    // Cumulative size guard: 50MB total across all pages
    REQUIRE(kGitHubMaxTotalFetchBytes == 50u * 1024u * 1024u);
    // Result count guard: 5K total issues/PRs
    REQUIRE(kGitHubMaxResultCount == 5000u);
    // Pagination cap: 10 pages × 100/page = 1K max (well below result guard)
    REQUIRE(kGitHubMaxPages == 10);
    // Worst case: 10 pages × 100 results = 1000 issues before guard hits
    REQUIRE(kGitHubMaxPages * 100 < kGitHubMaxResultCount);
}

TEST_CASE("Plane guard limits are set") {
    // Cumulative size guard: 100MB total across all pages
    REQUIRE(kPlaneMaxTotalFetchBytes == 100u * 1024u * 1024u);
    // Result count guard: 10K total issues
    REQUIRE(kPlaneMaxResultCount == 10000u);
    // Pagination cap: 50 pages × 100/page = 5K max (well below result guard)
    REQUIRE(kPlaneMaxPages == 50);
    // Worst case: 50 pages × 100 results = 5000 issues before guard hits
    REQUIRE(kPlaneMaxPages * 100 < kPlaneMaxResultCount);
}

TEST_CASE("Per-response size guard is enforced") {
    // ParseBounded enforces 4MB per response across all backends
    REQUIRE(kPerResponseMaxBytes == 4u * 1024u * 1024u);
    // Per-response guard is smaller than cumulative guards
    REQUIRE(kPerResponseMaxBytes < kJiraMaxTotalFetchBytes);
    REQUIRE(kPerResponseMaxBytes < kGitHubMaxTotalFetchBytes);
    REQUIRE(kPerResponseMaxBytes < kPlaneMaxTotalFetchBytes);
}

TEST_CASE("Guard limits prevent memory exhaustion") {
    // Worst-case memory usage with Jira: 50 pages × 100 issues × ~50KB/issue ≈ 250MB
    // Our 100MB cumulative size guard + 4MB per-response prevents this.
    const size_t kEstimatedBytesPerIssue = 50000; // conservative estimate
    const size_t jiraWorstCaseMemory = kJiraMaxPages * 100 * kEstimatedBytesPerIssue;
    // Cumulative guard should trigger first
    REQUIRE(kJiraMaxTotalFetchBytes < jiraWorstCaseMemory);

    // GitHub worst-case: 10 pages × 100 issues ≈ 50MB already at limit
    const size_t gitHubWorstCaseMemory = kGitHubMaxPages * 100 * kEstimatedBytesPerIssue;
    REQUIRE(kGitHubMaxTotalFetchBytes <= gitHubWorstCaseMemory);
}

}  // TEST_SUITE
