#include <doctest/doctest.h>

#include "Tracker/NewIssueInheritFields.h"

// NewIssueInheritFieldIdsFor is the single "which per-backend inherit list does a
// new-issue draft copy?" rule. Three surfaces build that draft — the grid header's
// "+ New Issue", the inactive-row cell, and the offline create path — and each used
// to carry its own copy of the same ternary chain, each comparing the hand-editable
// TrackerType exactly. A config saying "plane" therefore inherited the JIRA list on
// all three while the backend factory (already case-insensitive) ran a PlaneClient.

using smatchet::tracker::NewIssueInheritFieldIdsFor;

namespace {

/// A config whose four inherit lists are distinguishable by content, so a wrong
/// branch is visible as the wrong list rather than as an empty vector.
TrackerConfig MakeConfig() {
    TrackerConfig cfg;
    cfg.NewIssueInheritFieldIds = {"jira-field"};
    cfg.NewIssueInheritFieldIdsPlane = {"plane-field"};
    cfg.NewIssueInheritFieldIdsGitHub = {"github-field"};
    cfg.NewIssueInheritFieldIdsLinear = {"linear-field"};
    return cfg;
}

} // namespace

TEST_CASE("NewIssueInheritFieldIdsFor — canonical PascalCase picks the per-backend list") {
    TrackerConfig cfg = MakeConfig();

    cfg.TrackerType = "Plane";
    CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsPlane);

    cfg.TrackerType = "Linear";
    CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsLinear);

    cfg.TrackerType = "Jira";
    CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIds);
}

TEST_CASE("NewIssueInheritFieldIdsFor — casing does not change the answer") {
    TrackerConfig cfg = MakeConfig();

    // The live defect: smatchet.json is hand-editable, and every one of these
    // spellings resolved to the JIRA list under the old exact-match chain.
    SUBCASE("Plane") {
        cfg.TrackerType = "plane";
        CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsPlane);
        cfg.TrackerType = "PLANE";
        CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsPlane);
        cfg.TrackerType = "pLaNe";
        CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsPlane);
    }
    SUBCASE("Linear") {
        cfg.TrackerType = "linear";
        CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsLinear);
        cfg.TrackerType = "LINEAR";
        CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsLinear);
    }
}

TEST_CASE("NewIssueInheritFieldIdsFor — unknown and empty types fall back to the JIRA list") {
    TrackerConfig cfg = MakeConfig();

    cfg.TrackerType = "";
    CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIds);
    cfg.TrackerType = "Bugzilla";
    CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIds);
}

TEST_CASE("NewIssueInheritFieldIdsFor — GitHub reads the GitHub list") {
    // Regression: GitHub used to fall through to the Jira list, so the Preferences row
    // "New issue: inherit fields from last row (GitHub)" was loaded and saved but never
    // consumed by any of the three draft paths.
    TrackerConfig cfg = MakeConfig();
    cfg.TrackerType = "GitHub";
    CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsGitHub);
    CHECK(NewIssueInheritFieldIdsFor(cfg) != cfg.NewIssueInheritFieldIds);
    CHECK(&NewIssueInheritFieldIdsFor(cfg) == &cfg.NewIssueInheritFieldIdsGitHub);

    // The hand-editable TrackerType resolves case-insensitively here too.
    cfg.TrackerType = "github";
    CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsGitHub);
    cfg.TrackerType = "GITHUB";
    CHECK(NewIssueInheritFieldIdsFor(cfg) == cfg.NewIssueInheritFieldIdsGitHub);
}

TEST_CASE("NewIssueInheritFieldIdsFor — returns a reference into the config, not a copy") {
    // The three call sites bind the result to a `const std::vector<std::string>&`
    // and pass it on; a by-value return would silently make that a dangling-free
    // but per-call allocation on a UI click path.
    TrackerConfig cfg = MakeConfig();
    cfg.TrackerType = "Plane";
    CHECK(&NewIssueInheritFieldIdsFor(cfg) == &cfg.NewIssueInheritFieldIdsPlane);
}
