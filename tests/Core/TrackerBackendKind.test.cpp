#include <doctest/doctest.h>

#include "Tracker/TrackerBackendKind.h"

// BackendIndexFromType is the single tracker-type -> ordinal rule shared by the
// Preferences UI and mirrored by DefaultTrackerBackendFactory's client
// selection. It exists because the UI used to carry a second, weaker copy that
// matched exactly two spellings per backend, so a hand-edited "Github" resolved
// to Jira in the UI while the factory (already case-insensitive) built a
// GitHubClient — the credential rows on screen belonged to a different backend
// than the one actually running.

using smatchet::tracker::BackendIndexFromType;
using smatchet::tracker::kBackendGitHub;
using smatchet::tracker::kBackendJira;
using smatchet::tracker::kBackendLinear;
using smatchet::tracker::kBackendPlane;

TEST_CASE("BackendIndexFromType — canonical PascalCase resolves") {
    CHECK(BackendIndexFromType("Jira") == kBackendJira);
    CHECK(BackendIndexFromType("Plane") == kBackendPlane);
    CHECK(BackendIndexFromType("GitHub") == kBackendGitHub);
    CHECK(BackendIndexFromType("Linear") == kBackendLinear);
}

TEST_CASE("BackendIndexFromType — any casing resolves (the hand-edited-config case)") {
    SUBCASE("the spelling that used to fall through to Jira") {
        // "Github" is the natural misspelling and was the live defect: neither
        // "GitHub" nor "github", so the old two-alternative match missed it.
        CHECK(BackendIndexFromType("Github") == kBackendGitHub);
        CHECK(BackendIndexFromType("GITHUB") == kBackendGitHub);
        CHECK(BackendIndexFromType("gitHub") == kBackendGitHub);
    }
    SUBCASE("the same hazard on every other backend") {
        CHECK(BackendIndexFromType("PLANE") == kBackendPlane);
        CHECK(BackendIndexFromType("pLaNe") == kBackendPlane);
        CHECK(BackendIndexFromType("LINEAR") == kBackendLinear);
        CHECK(BackendIndexFromType("lInEaR") == kBackendLinear);
        CHECK(BackendIndexFromType("JIRA") == kBackendJira);
    }
}

TEST_CASE("BackendIndexFromType — unknown/empty falls back to Jira, as the factory does") {
    CHECK(BackendIndexFromType("") == kBackendJira);
    CHECK(BackendIndexFromType("Bugzilla") == kBackendJira);
    CHECK(BackendIndexFromType("   ") == kBackendJira);
    // No trimming by design: the factory does not trim either, so a padded value
    // must resolve the same way in both places rather than diverge again.
    CHECK(BackendIndexFromType(" github ") == kBackendJira);
}
