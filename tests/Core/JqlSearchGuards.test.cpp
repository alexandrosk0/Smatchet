// Issue-search result guards (Quality Pillar 3: a pathological query must not exhaust memory). The
// Jira / Plane / GitHub page loops read their ceilings from Tracker/SearchFetchGuards.h, so these
// checks are on the values the loops actually use, not on copies of them.

#include "Json/BoundedJsonParse.h"
#include "Tracker/SearchFetchGuards.h"

#include <doctest/doctest.h>

#include <cstddef>

namespace guards = smatchet::search_guards;

TEST_SUITE("Issue search result guards") {

    TEST_CASE("each backend's page cap stays below its result-count cap") {
        // A well-behaved server (at most kPageSize rows per page) hits the page cap first; the count cap
        // only stops a server that returns more rows per page than asked for.
        CHECK(static_cast<std::size_t>(guards::kJiraMaxPages * guards::kPageSize) < guards::kJiraMaxResultCount);
        CHECK(static_cast<std::size_t>(guards::kPlaneMaxPages * guards::kPageSize) < guards::kPlaneMaxResultCount);
        CHECK(static_cast<std::size_t>(guards::kGitHubMaxPages * guards::kPageSize) < guards::kGitHubMaxResultCount);
    }

    TEST_CASE("each cumulative size cap admits several full pages") {
        // A per-page response is bounded by ParseBounded; the cumulative cap must allow more than one
        // maximal page, or a single large page would end every sync.
        const std::size_t perPage = smatchet::json_safe::kDefaultMaxBytes;
        CHECK(guards::kJiraMaxTotalFetchBytes > 4 * perPage);
        CHECK(guards::kPlaneMaxTotalFetchBytes > 4 * perPage);
        CHECK(guards::kGitHubMaxTotalFetchBytes > 4 * perPage);
    }

    TEST_CASE("the caps are finite") {
        CHECK(guards::kJiraMaxPages > 0);
        CHECK(guards::kPlaneMaxPages > 0);
        CHECK(guards::kGitHubMaxPages > 0);
        CHECK(guards::kJiraMaxTotalFetchBytes <= 1024u * 1024u * 1024u);
        CHECK(guards::kPlaneMaxTotalFetchBytes <= 1024u * 1024u * 1024u);
        CHECK(guards::kGitHubMaxTotalFetchBytes <= 1024u * 1024u * 1024u);
    }

} // TEST_SUITE
