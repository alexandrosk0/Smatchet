#include <doctest/doctest.h>

// dev-onboarding-first-run-quickstart, slice 2 — bucket-A coverage of the first-run
// tracker-setup decisions: the "is this install still being set up?" predicate (which
// reuses the already-persisted TrackerConfig::BackendHasBeenReachable latch rather than
// introducing a second flag) and the credential fingerprint that pins a green
// "Test connection" verdict to the exact values that were probed.
#include "TrackerSetupPure.h"

#include <string>

namespace {

TrackerConfig JiraCfg() {
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.Domain = "example.atlassian.net";
    cfg.Email = "dev@example.com";
    cfg.ApiToken = "token-abc";
    return cfg;
}

} // namespace

TEST_CASE("NeedsSetup: fresh install needs setup, verified install does not") {
    TrackerConfig fresh;
    fresh.TrackerType = "Jira";
    CHECK(TrackerSetupPure::NeedsSetup(fresh));

    TrackerConfig ready = JiraCfg();
    ready.BackendHasBeenReachable = true;
    CHECK_FALSE(TrackerSetupPure::NeedsSetup(ready));
}

TEST_CASE("NeedsSetup: credentials present but never reached still needs setup") {
    TrackerConfig cfg = JiraCfg();
    cfg.BackendHasBeenReachable = false;
    CHECK(TrackerSetupPure::NeedsSetup(cfg));
}

TEST_CASE("NeedsSetup: a veteran who wipes a required field falls back into setup") {
    TrackerConfig cfg = JiraCfg();
    cfg.BackendHasBeenReachable = true;
    cfg.ApiToken.clear();
    CHECK(TrackerSetupPure::NeedsSetup(cfg));
}

TEST_CASE("CredentialFieldsComplete: per-backend required fields") {
    SUBCASE("Plane needs url + workspace + key") {
        TrackerConfig cfg;
        cfg.TrackerType = "Plane";
        cfg.PlaneUrl = "https://plane.example.com";
        cfg.PlaneWorkspaceSlug = "acme";
        CHECK_FALSE(TrackerSetupPure::CredentialFieldsComplete(cfg));
        cfg.PlaneApiKey = "plane-key";
        CHECK(TrackerSetupPure::CredentialFieldsComplete(cfg));
    }
    SUBCASE("GitHub needs pat + owner + repo") {
        TrackerConfig cfg;
        cfg.TrackerType = "GitHub";
        cfg.GitHubPat = "ghp_x";
        cfg.GitHubOwner = "acme";
        CHECK_FALSE(TrackerSetupPure::CredentialFieldsComplete(cfg));
        cfg.GitHubRepo = "widgets";
        CHECK(TrackerSetupPure::CredentialFieldsComplete(cfg));
    }
    SUBCASE("Linear accepts either team id or team key") {
        TrackerConfig cfg;
        cfg.TrackerType = "Linear";
        cfg.LinearApiKey = "lin_x";
        CHECK_FALSE(TrackerSetupPure::CredentialFieldsComplete(cfg));
        cfg.LinearTeamKey = "ENG";
        CHECK(TrackerSetupPure::CredentialFieldsComplete(cfg));
    }
    SUBCASE("backend key is normalized, so a lowercased type still resolves") {
        TrackerConfig cfg = JiraCfg();
        cfg.TrackerType = "jira";
        CHECK(TrackerSetupPure::CredentialFieldsComplete(cfg));
    }
}

TEST_CASE("CredentialFingerprint: stable, and moves with the active backend's fields") {
    const TrackerConfig cfg = JiraCfg();
    const std::string fp = TrackerSetupPure::CredentialFingerprint(cfg);
    CHECK(fp.size() == 16);
    CHECK(fp == TrackerSetupPure::CredentialFingerprint(cfg));

    SUBCASE("editing an active-backend credential changes it") {
        TrackerConfig edited = cfg;
        edited.ApiToken = "token-abd";
        CHECK(TrackerSetupPure::CredentialFingerprint(edited) != fp);
    }
    SUBCASE("editing an inactive backend's credential does not") {
        TrackerConfig other = cfg;
        other.PlaneApiKey = "unrelated";
        other.GitHubPat = "unrelated";
        CHECK(TrackerSetupPure::CredentialFingerprint(other) == fp);
    }
    SUBCASE("switching backend changes it even with identical field values") {
        TrackerConfig switched = cfg;
        switched.TrackerType = "GitHub";
        CHECK(TrackerSetupPure::CredentialFingerprint(switched) != fp);
    }
    SUBCASE("field boundaries are delimited, so a shifted split is not a collision") {
        TrackerConfig a = cfg;
        a.Domain = "ab";
        a.Email = "c";
        TrackerConfig b = cfg;
        b.Domain = "a";
        b.Email = "bc";
        CHECK(TrackerSetupPure::CredentialFingerprint(a) != TrackerSetupPure::CredentialFingerprint(b));
    }
    SUBCASE("BackendHasBeenReachable is not a credential and must not perturb the digest") {
        TrackerConfig latched = cfg;
        latched.BackendHasBeenReachable = true;
        CHECK(TrackerSetupPure::CredentialFingerprint(latched) == fp);
    }
}

// Regression: the Save & Sync unlock originally cleared ReadOnlyMode alone, leaving
// NeedsSetup() (and with it the locked main menu + the grid welcome state) still reading
// first-run until the connectivity monitor independently latched the same verdict. Both
// halves must move on the one edge — that pairing is what these cases pin.
TEST_CASE("ApplyVerifiedSaveUnlock: a matching pin clears read-only AND latches reachability") {
    TrackerConfig cfg = JiraCfg();
    cfg.ReadOnlyMode = true;
    const std::string pin = TrackerSetupPure::CredentialFingerprint(cfg);
    CHECK(TrackerSetupPure::NeedsSetup(cfg));

    CHECK(TrackerSetupPure::ApplyVerifiedSaveUnlock(cfg, pin));
    CHECK_FALSE(cfg.ReadOnlyMode);
    CHECK(cfg.BackendHasBeenReachable);
    CHECK_FALSE(TrackerSetupPure::NeedsSetup(cfg));
}

TEST_CASE("ApplyVerifiedSaveUnlock: refuses every unverified path and mutates nothing") {
    SUBCASE("an empty pin means nothing was probed this session") {
        TrackerConfig cfg = JiraCfg();
        cfg.ReadOnlyMode = true;
        CHECK_FALSE(TrackerSetupPure::ApplyVerifiedSaveUnlock(cfg, std::string()));
        CHECK(cfg.ReadOnlyMode);
        CHECK_FALSE(cfg.BackendHasBeenReachable);
    }
    SUBCASE("editing a credential after the probe invalidates the pin") {
        TrackerConfig cfg = JiraCfg();
        cfg.ReadOnlyMode = true;
        const std::string pin = TrackerSetupPure::CredentialFingerprint(cfg);
        cfg.ApiToken = "token-edited-after-the-probe";
        CHECK_FALSE(TrackerSetupPure::ApplyVerifiedSaveUnlock(cfg, pin));
        CHECK(cfg.ReadOnlyMode);
        CHECK_FALSE(cfg.BackendHasBeenReachable);
    }
    SUBCASE("switching backend after the probe invalidates the pin") {
        TrackerConfig cfg = JiraCfg();
        cfg.ReadOnlyMode = true;
        const std::string pin = TrackerSetupPure::CredentialFingerprint(cfg);
        cfg.TrackerType = "GitHub";
        cfg.GitHubPat = "ghp_x";
        cfg.GitHubOwner = "acme";
        cfg.GitHubRepo = "widgets";
        CHECK_FALSE(TrackerSetupPure::ApplyVerifiedSaveUnlock(cfg, pin));
        CHECK(cfg.ReadOnlyMode);
        CHECK_FALSE(cfg.BackendHasBeenReachable);
    }
    SUBCASE("read-only already off: a deliberate read-only session is not silently latched") {
        TrackerConfig cfg = JiraCfg();
        cfg.ReadOnlyMode = false;
        CHECK_FALSE(TrackerSetupPure::ApplyVerifiedSaveUnlock(cfg, TrackerSetupPure::CredentialFingerprint(cfg)));
        CHECK_FALSE(cfg.BackendHasBeenReachable);
    }
}
