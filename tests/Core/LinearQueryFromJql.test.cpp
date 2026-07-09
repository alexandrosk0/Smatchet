// LinearQueryFromJql doctest — JQL subset -> Linear IssueFilter.
// Slice 2 of docs/plans/linear-tracker-backend.md.

#include "LinearQueryFromJql.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

using namespace smatchet::linear;

TEST_CASE("TranslateJqlToLinearFilter — empty input is Ok with no filter") {
    JqlToLinearResult r = TranslateJqlToLinearFilter("");
    CHECK(r.Ok);
    CHECK_FALSE(r.HasFilter());
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToLinearFilter — assignee currentUser + named") {
    JqlToLinearResult me = TranslateJqlToLinearFilter("assignee = currentUser()");
    CHECK(me.Filter["assignee"]["isMe"]["eq"] == true);

    JqlToLinearResult named = TranslateJqlToLinearFilter("assignee = \"jdoe\"");
    CHECK(named.Filter["assignee"]["name"]["containsIgnoreCase"] == "jdoe");
}

TEST_CASE("TranslateJqlToLinearFilter — status eq / neq / in") {
    CHECK(TranslateJqlToLinearFilter("status = \"In Progress\"").Filter["state"]["name"]["eq"] == "In Progress");
    CHECK(TranslateJqlToLinearFilter("status != Done").Filter["state"]["name"]["neq"] == "Done");

    JqlToLinearResult in = TranslateJqlToLinearFilter("status in (\"Todo\", \"In Progress\")");
    REQUIRE(in.Filter["state"]["name"]["in"].is_array());
    CHECK(in.Filter["state"]["name"]["in"].size() == 2);
    CHECK(in.Filter["state"]["name"]["in"][0] == "Todo");
    CHECK(in.Filter["state"]["name"]["in"][1] == "In Progress");

    // 2-char quoted operands ("" and '') unquote to empty (pins the >= 2 unquote bound).
    CHECK(TranslateJqlToLinearFilter("status = \"\"").Filter["state"]["name"]["eq"] == "");
    CHECK(TranslateJqlToLinearFilter("status = ''").Filter["state"]["name"]["eq"] == "");
}

TEST_CASE("TranslateJqlToLinearFilter — labels eq / in") {
    CHECK(TranslateJqlToLinearFilter("labels = bug").Filter["labels"]["name"]["eq"] == "bug");
    JqlToLinearResult in = TranslateJqlToLinearFilter("labels in (bug, p1)");
    CHECK(in.Filter["labels"]["name"]["in"].size() == 2);
}

TEST_CASE("TranslateJqlToLinearFilter — priority name -> 0-4 int") {
    CHECK(TranslateJqlToLinearFilter("priority = High").Filter["priority"]["eq"] == 2);
    CHECK(TranslateJqlToLinearFilter("priority = Urgent").Filter["priority"]["eq"] == 1);
    JqlToLinearResult in = TranslateJqlToLinearFilter("priority in (Urgent, Low)");
    REQUIRE(in.Filter["priority"]["in"].is_array());
    CHECK(in.Filter["priority"]["in"][0] == 1);
    CHECK(in.Filter["priority"]["in"][1] == 4);

    JqlToLinearResult bad = TranslateJqlToLinearFilter("priority = Bogus");
    CHECK_FALSE(bad.HasFilter());
    CHECK(bad.Warning.find("priority") != std::string::npos);
}

TEST_CASE("TranslateJqlToLinearFilter — project / team / text") {
    CHECK(TranslateJqlToLinearFilter("project = \"Q3 Roadmap\"").Filter["project"]["name"]["eq"] == "Q3 Roadmap");
    CHECK(TranslateJqlToLinearFilter("team = ENG").Filter["team"]["key"]["eq"] == "ENG");

    JqlToLinearResult txt = TranslateJqlToLinearFilter("text ~ \"crash on launch\"");
    CHECK(txt.SearchTerm == "crash on launch");
    CHECK_FALSE(txt.HasFilter());
}

TEST_CASE("TranslateJqlToLinearFilter — AND combines into one filter (implicit AND)") {
    JqlToLinearResult r =
        TranslateJqlToLinearFilter("assignee = currentUser() AND status = \"Open\" AND priority = High");
    CHECK(r.Filter["assignee"]["isMe"]["eq"] == true);
    CHECK(r.Filter["state"]["name"]["eq"] == "Open");
    CHECK(r.Filter["priority"]["eq"] == 2);
    CHECK(r.Warning.empty());
}

TEST_CASE("TranslateJqlToLinearFilter — ORDER BY stripped with warning") {
    JqlToLinearResult r = TranslateJqlToLinearFilter("status = Open ORDER BY updated DESC");
    CHECK(r.Filter["state"]["name"]["eq"] == "Open");
    CHECK(r.Warning.find("ORDER BY") != std::string::npos);
}

TEST_CASE("TranslateJqlToLinearFilter — OR and unknown fields warn + drop") {
    JqlToLinearResult orq = TranslateJqlToLinearFilter("status = Open OR status = Done");
    CHECK(orq.Warning.find("OR") != std::string::npos);
    CHECK_FALSE(orq.HasFilter());

    JqlToLinearResult unk = TranslateJqlToLinearFilter("sprint = 5");
    CHECK(unk.Warning.find("unsupported field") != std::string::npos);
    CHECK_FALSE(unk.HasFilter());

    // A recognized AND clause survives even when another is dropped.
    JqlToLinearResult mixed = TranslateJqlToLinearFilter("status = Open AND sprint = 5");
    CHECK(mixed.Filter["state"]["name"]["eq"] == "Open");
    CHECK(mixed.Warning.find("unsupported field") != std::string::npos);
}
