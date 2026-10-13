#pragma once

// SearchFetchGuards — the per-backend ceilings on one paginated issue search (Quality Pillar 3: a
// pathological query must not exhaust memory). One copy, shared by the Jira / Plane / GitHub page
// loops and their test, so the test checks the values the loops actually use. Each page is also
// bounded by json_safe::kDefaultMaxBytes (Json/BoundedJsonParse.h). A loop that stops on any of
// these sets a soft warning on its fetch summary, so a cut-short result never reads as complete.

#include <cstddef>

namespace smatchet {
namespace search_guards {

/// Rows asked for per page by every backend.
constexpr int kPageSize = 100;

constexpr int kJiraMaxPages = 50;
constexpr std::size_t kJiraMaxTotalFetchBytes = 100u * 1024u * 1024u;
/// Reached only when the server returns more rows per page than kPageSize (the page cap bounds a
/// well-behaved server below it).
constexpr std::size_t kJiraMaxResultCount = 10000u;

constexpr int kPlaneMaxPages = 50;
constexpr std::size_t kPlaneMaxTotalFetchBytes = 100u * 1024u * 1024u;
constexpr std::size_t kPlaneMaxResultCount = 10000u; ///< as kJiraMaxResultCount

/// GitHub search returns at most 1000 items (10 pages of 100), the REST search API's own limit.
constexpr int kGitHubMaxPages = 10;
constexpr std::size_t kGitHubMaxTotalFetchBytes = 50u * 1024u * 1024u;
constexpr std::size_t kGitHubMaxResultCount = 5000u; ///< as kJiraMaxResultCount

} // namespace search_guards
} // namespace smatchet
