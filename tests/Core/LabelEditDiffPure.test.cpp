// LabelEditDiffPure doctest — exhaustive set-diff coverage.
// Part of docs/plans/shipped/github-tracker-backend.md.

#include "LabelEditDiffPure.h"

#include <doctest/doctest.h>

using namespace smatchet::github;

TEST_CASE("LabelEditDiffPure — empty inputs yield empty diff") {
    const auto d = ComputeLabelEditDiff({}, {});
    CHECK(d.ToAdd.empty());
    CHECK(d.ToRemove.empty());
}

TEST_CASE("LabelEditDiffPure — current empty, intended non-empty → all toAdd") {
    const auto d = ComputeLabelEditDiff({}, {"bug", "feature"});
    REQUIRE(d.ToAdd.size() == 2u);
    CHECK(d.ToAdd[0] == "bug");
    CHECK(d.ToAdd[1] == "feature");
    CHECK(d.ToRemove.empty());
}

TEST_CASE("LabelEditDiffPure — intended empty, current non-empty → all toRemove") {
    const auto d = ComputeLabelEditDiff({"bug", "feature"}, {});
    CHECK(d.ToAdd.empty());
    REQUIRE(d.ToRemove.size() == 2u);
    CHECK(d.ToRemove[0] == "bug");
    CHECK(d.ToRemove[1] == "feature");
}

TEST_CASE("LabelEditDiffPure — identical sets yield empty diff") {
    const auto d = ComputeLabelEditDiff({"bug", "p0"}, {"p0", "bug"});
    CHECK(d.ToAdd.empty());
    CHECK(d.ToRemove.empty());
}

TEST_CASE("LabelEditDiffPure — partial overlap produces correct add/remove split") {
    const auto d = ComputeLabelEditDiff({"bug", "p0"}, {"feature", "p0"});
    REQUIRE(d.ToAdd.size() == 1u);
    CHECK(d.ToAdd[0] == "feature");
    REQUIRE(d.ToRemove.size() == 1u);
    CHECK(d.ToRemove[0] == "bug");
}

TEST_CASE("LabelEditDiffPure — duplicates in either input are de-duped") {
    const auto d = ComputeLabelEditDiff({"bug", "bug"}, {"feature", "feature"});
    REQUIRE(d.ToAdd.size() == 1u);
    CHECK(d.ToAdd[0] == "feature");
    REQUIRE(d.ToRemove.size() == 1u);
    CHECK(d.ToRemove[0] == "bug");
}

TEST_CASE("LabelEditDiffPure — case-sensitive (GitHub labels are case-sensitive)") {
    const auto d = ComputeLabelEditDiff({"Bug"}, {"bug"});
    REQUIRE(d.ToAdd.size() == 1u);
    CHECK(d.ToAdd[0] == "bug");
    REQUIRE(d.ToRemove.size() == 1u);
    CHECK(d.ToRemove[0] == "Bug");
}

TEST_CASE("LabelEditDiffPure — output sorted ascending for deterministic pinning") {
    const auto d = ComputeLabelEditDiff({"z", "a", "m"}, {"c", "x"});
    REQUIRE(d.ToAdd.size() == 2u);
    CHECK(d.ToAdd[0] == "c");
    CHECK(d.ToAdd[1] == "x");
    REQUIRE(d.ToRemove.size() == 3u);
    CHECK(d.ToRemove[0] == "a");
    CHECK(d.ToRemove[1] == "m");
    CHECK(d.ToRemove[2] == "z");
}
