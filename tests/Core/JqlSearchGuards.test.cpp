// Issue-search result guards (Quality Pillar 3: a pathological query must not exhaust memory). The
// Jira / Plane / GitHub page loops read their ceilings from Tracker/SearchFetchGuards.h (backed by
// the user-tunable Config/TunableLimits.h), so these checks are on the values the loops actually use, not
// on copies of them.

#include "Json/BoundedJsonParse.h"
#include "TunableLimits.h"
#include "Tracker/SearchFetchGuards.h"

#include <doctest/doctest.h>

#include <cstddef>

namespace guards = smatchet::search_guards;

TEST_SUITE("Issue search result guards") {

    TEST_CASE("each backend's result-count cap admits at least one full page") {
        // The count cap is a user-tunable result ceiling (default 500): it may be hit before the page cap,
        // in which case the loop stops with a truncation warning. It must still admit one full page, or
        // every search would be cut off on its first page.
        const std::size_t page = static_cast<std::size_t>(guards::kPageSize);
        CHECK(guards::JiraMaxResultCount() >= page);
        CHECK(guards::PlaneMaxResultCount() >= page);
        CHECK(guards::GitHubMaxResultCount() >= page);
    }

    TEST_CASE("default result-count caps are 500") {
        const smatchet::limits::LimitsConfig defaults;
        CHECK(defaults.JiraSearchMaxResults == 500);
        CHECK(defaults.PlaneSearchMaxResults == 500);
        CHECK(defaults.GitHubSearchMaxResults == 500);
    }

    TEST_CASE("each cumulative size cap admits several full pages") {
        // A per-page response is bounded by ParseBounded; the cumulative cap must allow more than one
        // maximal page, or a single large page would end every sync.
        const std::size_t perPage = smatchet::json_safe::kDefaultMaxBytes;
        CHECK(guards::JiraMaxTotalFetchBytes() > 4 * perPage);
        CHECK(guards::PlaneMaxTotalFetchBytes() > 4 * perPage);
        CHECK(guards::GitHubMaxTotalFetchBytes() > 4 * perPage);
    }

    TEST_CASE("the caps are finite") {
        CHECK(guards::JiraMaxPages() > 0);
        CHECK(guards::PlaneMaxPages() > 0);
        CHECK(guards::GitHubMaxPages() > 0);
        CHECK(guards::JiraMaxTotalFetchBytes() <= 1024u * 1024u * 1024u);
        CHECK(guards::PlaneMaxTotalFetchBytes() <= 1024u * 1024u * 1024u);
        CHECK(guards::GitHubMaxTotalFetchBytes() <= 1024u * 1024u * 1024u);
    }

} // TEST_SUITE
