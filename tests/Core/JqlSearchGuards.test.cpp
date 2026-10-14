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

    TEST_CASE("the page cap does not bind before the result-count cap") {
        // The result-count cap is the intended soft bound; a page cap that bit first would silently
        // shrink it. Holds at the defaults and is what the page-rounded truncation logic assumes.
        const std::size_t page = static_cast<std::size_t>(guards::kPageSize);
        CHECK(static_cast<std::size_t>(guards::JiraMaxPages()) * page >= guards::JiraMaxResultCount());
        CHECK(static_cast<std::size_t>(guards::PlaneMaxPages()) * page >= guards::PlaneMaxResultCount());
        CHECK(static_cast<std::size_t>(guards::GitHubMaxPages()) * page >= guards::GitHubMaxResultCount());
    }

    TEST_CASE("membership probes get a looser ceiling than the interactive search") {
        // Keys-only reconcile and changed-since must see a whole view; a probe that hit the interactive
        // cap would always report a partial walk and never prune stale rows.
        const guards::LoopCaps probe = guards::ProbeCaps();
        CHECK(probe.maxResultCount > guards::JiraCaps().maxResultCount);
        CHECK(probe.maxResultCount > guards::PlaneCaps().maxResultCount);
        CHECK(probe.maxPages >= guards::JiraCaps().maxPages);
        CHECK(probe.maxTotalFetchBytes > guards::JiraCaps().maxTotalFetchBytes);
        CHECK(probe.maxTotalFetchBytes > guards::PlaneCaps().maxTotalFetchBytes);
        // ...and its page cap can actually reach its own result cap.
        CHECK(static_cast<std::size_t>(probe.maxPages) * static_cast<std::size_t>(guards::kPageSize) >=
              probe.maxResultCount);
    }

    TEST_CASE("the wall-clock deadline expires and is not already expired by default") {
        CHECK(guards::Deadline(0).Expired());
        CHECK_FALSE(guards::Deadline(3600).Expired());
        CHECK_FALSE(guards::StartSearchDeadline().Expired());
        CHECK(smatchet::limits::LimitsConfig().SearchMaxTotalSeconds == 120);
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
