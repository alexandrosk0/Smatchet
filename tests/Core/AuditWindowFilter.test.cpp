// Bucket-A coverage for the pure audit-window filter / pagination helpers extracted from
// SmatchetUI::drawAuditWindow (Source/Core/src/Ui/SmatchetAudit_detail.cpp). No ImGui, no
// AppController — pure functions of their arguments. Goldens come from constructing real
// audit-event JSON and asserting the same filter the draw loop applies.

#include <doctest/doctest.h>

#include "Ui/SmatchetAudit_detail.h"

using SmatchetAudit::detail::ActionMatchesFilter;
using SmatchetAudit::detail::ClampRowsPerPage;
using SmatchetAudit::detail::CollectFilteredAuditIndices;
using SmatchetAudit::detail::ComputeAuditPageCount;
using SmatchetAudit::detail::ContainsNoCasePreLower;
using SmatchetAudit::detail::JsonStringValue;
using SmatchetAudit::detail::ResultMatchesFilter;

namespace {

nlohmann::json MakeEvent(const std::string& action, bool success) {
    nlohmann::json e;
    e["action"] = action;
    e["success"] = success;
    return e;
}

} // namespace

TEST_CASE("JsonStringValue stringifies by type") {
    nlohmann::json e;
    e["s"] = "hello";
    e["b"] = true;
    e["i"] = -42;
    CHECK(JsonStringValue(e, "s") == "hello");
    CHECK(JsonStringValue(e, "b") == "true");
    CHECK(JsonStringValue(e, "i") == "-42");
    CHECK(JsonStringValue(e, "missing").empty());
    CHECK(JsonStringValue(nlohmann::json(7), "any").empty());
}

TEST_CASE("ContainsNoCasePreLower honours empty needle") {
    CHECK(ContainsNoCasePreLower("abcdef", ""));
    CHECK(ContainsNoCasePreLower("abcdef", "cde"));
    CHECK_FALSE(ContainsNoCasePreLower("abcdef", "xyz"));
}

TEST_CASE("ActionMatchesFilter buckets actions") {
    CHECK(ActionMatchesFilter("issue_create", 1));
    CHECK_FALSE(ActionMatchesFilter("offline_create", 1)); // create-but-offline excluded from Creates
    CHECK(ActionMatchesFilter("status_transition", 2));
    CHECK(ActionMatchesFilter("field_edit_summary", 2));
    CHECK(ActionMatchesFilter("comment_add", 3));
    CHECK(ActionMatchesFilter("attach_file", 4));
    CHECK(ActionMatchesFilter("offline_create", 5));
    CHECK(ActionMatchesFilter("anything", 0)); // All
}

TEST_CASE("ResultMatchesFilter splits success/failure") {
    CHECK(ResultMatchesFilter(true, 0));
    CHECK(ResultMatchesFilter(false, 0));
    CHECK(ResultMatchesFilter(true, 1));
    CHECK_FALSE(ResultMatchesFilter(false, 1));
    CHECK(ResultMatchesFilter(false, 2));
    CHECK_FALSE(ResultMatchesFilter(true, 2));
}

TEST_CASE("ClampRowsPerPage clamps into [25, 1000]") {
    CHECK(ClampRowsPerPage(0) == 25);
    CHECK(ClampRowsPerPage(25) == 25);
    CHECK(ClampRowsPerPage(500) == 500);
    CHECK(ClampRowsPerPage(5000) == 1000);
}

TEST_CASE("ComputeAuditPageCount ceil-divides, min one page") {
    CHECK(ComputeAuditPageCount(0, 25) == 1);
    CHECK(ComputeAuditPageCount(1, 25) == 1);
    CHECK(ComputeAuditPageCount(25, 25) == 1);
    CHECK(ComputeAuditPageCount(26, 25) == 2);
    CHECK(ComputeAuditPageCount(100, 25) == 4);
    CHECK(ComputeAuditPageCount(10, 0) == 1); // defensive guard
}

TEST_CASE("CollectFilteredAuditIndices applies filters, search, and ordering") {
    std::vector<nlohmann::json> events;
    events.push_back(MakeEvent("issue_create", true));      // 0
    events.push_back(MakeEvent("comment_add", false));      // 1
    events.push_back(MakeEvent("status_transition", true)); // 2
    std::vector<std::string> searchLower;
    searchLower.push_back("issue_create proj-1");
    searchLower.push_back("comment_add proj-2");
    searchLower.push_back("status_transition proj-1");

    SUBCASE("no filters, oldest-first preserves order") {
        std::vector<std::size_t> got = CollectFilteredAuditIndices(events, searchLower, 0, 0, "", false);
        REQUIRE(got.size() == 3);
        CHECK(got[0] == 0);
        CHECK(got[1] == 1);
        CHECK(got[2] == 2);
    }
    SUBCASE("newest-first reverses iteration order") {
        std::vector<std::size_t> got = CollectFilteredAuditIndices(events, searchLower, 0, 0, "", true);
        REQUIRE(got.size() == 3);
        CHECK(got[0] == 2);
        CHECK(got[1] == 1);
        CHECK(got[2] == 0);
    }
    SUBCASE("action filter Creates keeps only index 0") {
        std::vector<std::size_t> got = CollectFilteredAuditIndices(events, searchLower, 1, 0, "", false);
        REQUIRE(got.size() == 1);
        CHECK(got[0] == 0);
    }
    SUBCASE("result filter Failure keeps only index 1") {
        std::vector<std::size_t> got = CollectFilteredAuditIndices(events, searchLower, 0, 2, "", false);
        REQUIRE(got.size() == 1);
        CHECK(got[0] == 1);
    }
    SUBCASE("search narrows to matching pre-lowered rows") {
        std::vector<std::size_t> got = CollectFilteredAuditIndices(events, searchLower, 0, 0, "proj-1", false);
        REQUIRE(got.size() == 2);
        CHECK(got[0] == 0);
        CHECK(got[1] == 2);
    }
    SUBCASE("index past searchLower end is skipped") {
        std::vector<std::string> shortSearch;
        shortSearch.push_back("issue_create proj-1"); // only one entry for three events
        std::vector<std::size_t> got = CollectFilteredAuditIndices(events, shortSearch, 0, 0, "", false);
        REQUIRE(got.size() == 1);
        CHECK(got[0] == 0);
    }
}
