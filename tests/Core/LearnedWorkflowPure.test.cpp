// LearnedWorkflowPure bucket-A tests — the key and JSON (de)serialisation behind the workflow the
// status combo remembers from earlier online use (Quality Pillar 6). Pure: no I/O.

#include "LearnedWorkflowPure.h"
#include "Tracker/TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using smatchet::workflow::BuildLearnedTransitionsKey;
using smatchet::workflow::ParseTransitionTargets;
using smatchet::workflow::SerializeTransitionTargets;

namespace {

TrackerFieldOption Option(const std::string& id, const std::string& value) {
    TrackerFieldOption opt;
    opt.Id = id;
    opt.Value = value;
    return opt;
}

} // namespace

TEST_CASE("LearnedWorkflowPure — key joins project, issue type and from status") {
    CHECK(BuildLearnedTransitionsKey("PROJ", "bug", "1") == "PROJ|bug|1");
}

TEST_CASE("LearnedWorkflowPure — key is empty when any part is empty (nothing to learn under)") {
    CHECK(BuildLearnedTransitionsKey("", "bug", "1").empty());
    CHECK(BuildLearnedTransitionsKey("PROJ", "", "1").empty());
    CHECK(BuildLearnedTransitionsKey("PROJ", "bug", "").empty());
}

TEST_CASE("LearnedWorkflowPure — serialize then parse round-trips ids, names and order") {
    const std::vector<TrackerFieldOption> in = {Option("2", "In Progress"), Option("3", "Done \xE2\x9C\x93")};
    std::vector<TrackerFieldOption> out;
    REQUIRE(ParseTransitionTargets(SerializeTransitionTargets(in), out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].Id == "2");
    CHECK(out[0].Value == "In Progress");
    CHECK(out[1].Id == "3");
    CHECK(out[1].Value == "Done \xE2\x9C\x93");
}

TEST_CASE("LearnedWorkflowPure — an empty list round-trips to an empty array") {
    std::vector<TrackerFieldOption> out = {Option("x", "stale")};
    REQUIRE(ParseTransitionTargets(SerializeTransitionTargets({}), out));
    CHECK(out.empty());
}

TEST_CASE("LearnedWorkflowPure — invalid UTF-8 in a name is replaced, never thrown") {
    const std::vector<TrackerFieldOption> in = {Option("2", std::string("Bad \xFF name"))};
    std::string json;
    CHECK_NOTHROW(json = SerializeTransitionTargets(in));
    std::vector<TrackerFieldOption> out;
    REQUIRE(ParseTransitionTargets(json, out));
    REQUIRE(out.size() == 1);
    CHECK(out[0].Id == "2");
}

TEST_CASE("LearnedWorkflowPure — malformed JSON and non-arrays are rejected and clear the output") {
    std::vector<TrackerFieldOption> out = {Option("x", "stale")};
    CHECK_FALSE(ParseTransitionTargets("[{\"id\":", out));
    CHECK(out.empty());
    out.push_back(Option("x", "stale"));
    CHECK_FALSE(ParseTransitionTargets("{\"id\":\"2\"}", out));
    CHECK(out.empty());
    CHECK_FALSE(ParseTransitionTargets("", out));
}

TEST_CASE("LearnedWorkflowPure — a name-only entry is kept; entries with neither id nor name are skipped") {
    std::vector<TrackerFieldOption> out;
    REQUIRE(ParseTransitionTargets(R"([{"name":"Done"},{"id":""},{"other":1},"text",{"id":7,"name":"Blocked"}])", out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].Id.empty());
    CHECK(out[0].Value == "Done");
    CHECK(out[1].Id == "7"); // a numeric id reads as its decimal text
    CHECK(out[1].Value == "Blocked");
}

TEST_CASE("LearnedWorkflowPure — deeply nested input is rejected by the bounded parser") {
    std::string deep(5000, '[');
    deep += std::string(5000, ']');
    std::vector<TrackerFieldOption> out;
    CHECK_FALSE(ParseTransitionTargets(deep, out));
    CHECK(out.empty());
}
