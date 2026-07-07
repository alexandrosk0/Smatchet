// DR25 — Plane single-page pagination drops data. ResolvePlaneProject / ListProjects /
// FetchIssueComments now follow Plane's cursor across every page instead of reading only
// the first. The page-follow decision is driven by the pure smatchet::plane::NextPaginationCursor
// helper (reads next_page_results + next_cursor). These tests characterise that decision the way
// the production loops consume it — advance while a cursor is returned, stop when it is empty — and
// prove a realistic multi-page walk gathers rows from every page. No HTTP, no PlaneClient instance.

#include "PlaneIssueMappingPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet::plane::NextPaginationCursor;

namespace {

// Builds one Plane list-endpoint envelope: a `results` array plus the pagination signals the
// production loops read back through NextPaginationCursor.
nlohmann::json MakePlanePage(const std::vector<std::string>& ids, bool morePages, const std::string& nextCursor) {
    nlohmann::json page = nlohmann::json::object();
    nlohmann::json results = nlohmann::json::array();
    for (const auto& id : ids) {
        nlohmann::json row = nlohmann::json::object();
        row["id"] = id;
        results.push_back(row);
    }
    page["results"] = results;
    page["next_page_results"] = morePages;
    if (!nextCursor.empty()) {
        page["next_cursor"] = nextCursor;
    }
    return page;
}

} // namespace

TEST_CASE("DR25 NextPaginationCursor — non-final page yields its cursor, final page stops the loop") {
    const nlohmann::json first = MakePlanePage({"a", "b"}, true, "cursor-2");
    CHECK(NextPaginationCursor(first) == "cursor-2");

    const nlohmann::json last = MakePlanePage({"c"}, false, "");
    CHECK(NextPaginationCursor(last).empty());
}

TEST_CASE("DR25 pagination walk — every page's rows are gathered, not just the first") {
    // Three simulated pages the fixed loops would fetch in sequence. Page 3 is the last.
    std::vector<nlohmann::json> pages = {
        MakePlanePage({"p1-1", "p1-2"}, true, "c2"),
        MakePlanePage({"p2-1", "p2-2"}, true, "c3"),
        MakePlanePage({"p3-1"}, false, ""),
    };

    // Mirror the production page-follow loop: consume page N, then advance via the cursor.
    std::vector<std::string> gathered;
    std::string cursor;
    std::size_t index = 0;
    for (int page = 0; page < 100; ++page) {
        REQUIRE(index < pages.size());
        const nlohmann::json& body = pages[index];
        for (const auto& row : body["results"]) {
            gathered.push_back(row["id"].get<std::string>());
        }
        cursor = NextPaginationCursor(body);
        if (cursor.empty()) {
            break;
        }
        ++index;
    }

    // A first-page-only reader would have stopped at 2 rows; the loop must collect all 5.
    REQUIRE(gathered.size() == 5);
    CHECK(gathered.front() == "p1-1");
    CHECK(gathered.back() == "p3-1");
}

TEST_CASE("DR25 pagination — a cursor that never signals end relies on the loop page cap") {
    // A misbehaving server that always reports another page: NextPaginationCursor keeps
    // returning a token, so termination is guaranteed only by the loop's defensive cap.
    const nlohmann::json looping = MakePlanePage({"x"}, true, "same-cursor");
    CHECK(NextPaginationCursor(looping) == "same-cursor");

    int iterations = 0;
    std::string cursor;
    for (int page = 0; page < 100; ++page) {
        ++iterations;
        cursor = NextPaginationCursor(looping);
        if (cursor.empty()) {
            break;
        }
    }
    CHECK(iterations == 100);
}
