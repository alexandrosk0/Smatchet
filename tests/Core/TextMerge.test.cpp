#include <doctest/doctest.h>

#include "TextMerge.h"

#include <string>

using TextMerge::ThreeWayMerge;

TEST_CASE("TextMerge::ThreeWayMerge identity fast paths return clean") {
    SUBCASE("mine == theirs") {
        const auto r = ThreeWayMerge("base", "edited", "edited");
        CHECK(r.IsClean);
        CHECK(r.Text == "edited");
    }
    SUBCASE("mine == base — take theirs") {
        const auto r = ThreeWayMerge("base", "base", "edited");
        CHECK(r.IsClean);
        CHECK(r.Text == "edited");
    }
    SUBCASE("theirs == base — take mine") {
        const auto r = ThreeWayMerge("base", "edited", "base");
        CHECK(r.IsClean);
        CHECK(r.Text == "edited");
    }
}

TEST_CASE("TextMerge::ThreeWayMerge applies non-overlapping changes from both sides") {
    const std::string base   = "alpha\nbeta\ngamma";
    const std::string mine   = "ALPHA\nbeta\ngamma";
    const std::string theirs = "alpha\nbeta\nGAMMA";

    const auto r = ThreeWayMerge(base, mine, theirs);
    CHECK(r.IsClean);
    CHECK(r.Text == "ALPHA\nbeta\nGAMMA");
}

TEST_CASE("TextMerge::ThreeWayMerge collapses identical concurrent edit") {
    const std::string base   = "one\ntwo\nthree";
    const std::string edited = "one\nTWO\nthree";

    const auto r = ThreeWayMerge(base, edited, edited);
    CHECK(r.IsClean);
    CHECK(r.Text == "one\nTWO\nthree");
}

TEST_CASE("TextMerge::ThreeWayMerge emits conflict markers on competing edits") {
    const std::string base   = "one\ntwo\nthree";
    const std::string mine   = "one\nMINE\nthree";
    const std::string theirs = "one\nTHEIRS\nthree";

    const auto r = ThreeWayMerge(base, mine, theirs);
    CHECK_FALSE(r.IsClean);
    CHECK(r.Text.find("<<<<<<< mine") != std::string::npos);
    CHECK(r.Text.find("MINE") != std::string::npos);
    CHECK(r.Text.find("=======") != std::string::npos);
    CHECK(r.Text.find("THEIRS") != std::string::npos);
    CHECK(r.Text.find(">>>>>>> theirs") != std::string::npos);
}

TEST_CASE("TextMerge::ThreeWayMerge flags a consumed-range overlap as non-clean") {
    // Regression (DR1): an insertion hunk whose base range is consumed by a
    // replacement on the other side must NOT be silently dropped and reported
    // clean — that loses the user's queued edit and the offline queue would PUT
    // it as a clean merge. The competing edits must surface as a conflict.
    SUBCASE("append vs whole-line replace") {
        const auto r = ThreeWayMerge("a", "a\nx", "A");
        CHECK_FALSE(r.IsClean);
    }
    SUBCASE("whole-line replace vs append (symmetric)") {
        const auto r = ThreeWayMerge("a", "M", "a\ny");
        CHECK_FALSE(r.IsClean);
    }
}

TEST_CASE("TextMerge::ThreeWayMerge handles empty inputs cleanly") {
    SUBCASE("all empty") {
        const auto r = ThreeWayMerge("", "", "");
        CHECK(r.IsClean);
        CHECK(r.Text == "");
    }
    SUBCASE("base empty, mine adds, theirs empty") {
        const auto r = ThreeWayMerge("", "added\n", "");
        CHECK(r.IsClean);
        CHECK(r.Text.find("added") != std::string::npos);
    }
}
