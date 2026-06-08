#include <doctest/doctest.h>

#include "SmatchetPreferencesUi_detail.h"

#include <string>

// Issue #979 (secondary root cause) — bucket-A coverage for the pure credential-trim
// helper applied by onPreferencesSaveAndSync. A trailing space in the Jira email
// ("user@gmail.com ", observed on a real on-disk config) made Atlassian reject basic
// auth with a persistent 401. SmatchetPreferencesUi_detail.h defines the helper
// `inline`, so the doctest rig needs no extra .cpp link.

using SmatchetPreferencesUiDetail::TrimTrackerCredentialFields;

TEST_CASE("TrimTrackerCredentialFields — strips leading + trailing whitespace on every credential field" *
          doctest::test_suite("[high-risk]")) {
    TrackerConfig cfg;
    cfg.Domain = "  company.atlassian.net ";
    cfg.Email = "user@gmail.com "; // the observed live-bug value (trailing space → Jira 401)
    cfg.ApiToken = "\ttok-123\n";
    cfg.PlaneUrl = " https://api.plane.so ";
    cfg.PlaneWorkspaceSlug = " my-workspace";
    cfg.PlaneApiKey = "plane-key\r\n";
    cfg.GitHubBaseUrl = " https://api.github.com ";
    cfg.GitHubPat = " ghp_abc123 ";
    cfg.GitHubOwner = " owner ";
    cfg.GitHubRepo = "repo ";

    TrimTrackerCredentialFields(cfg);

    CHECK(cfg.Domain == "company.atlassian.net");
    CHECK(cfg.Email == "user@gmail.com");
    CHECK(cfg.ApiToken == "tok-123");
    CHECK(cfg.PlaneUrl == "https://api.plane.so");
    CHECK(cfg.PlaneWorkspaceSlug == "my-workspace");
    CHECK(cfg.PlaneApiKey == "plane-key");
    CHECK(cfg.GitHubBaseUrl == "https://api.github.com");
    CHECK(cfg.GitHubPat == "ghp_abc123");
    CHECK(cfg.GitHubOwner == "owner");
    CHECK(cfg.GitHubRepo == "repo");
}

TEST_CASE("TrimTrackerCredentialFields — interior whitespace and clean values are untouched") {
    TrackerConfig cfg;
    cfg.Domain = "company.atlassian.net";
    cfg.Email = "first.last@company.com";
    cfg.GitHubRepo = "my repo"; // interior space preserved (trim is edges-only)

    TrimTrackerCredentialFields(cfg);

    CHECK(cfg.Domain == "company.atlassian.net");
    CHECK(cfg.Email == "first.last@company.com");
    CHECK(cfg.GitHubRepo == "my repo");
}

TEST_CASE("TrimTrackerCredentialFields — whitespace-only fields collapse to empty") {
    TrackerConfig cfg;
    cfg.GitHubBaseUrl = "   "; // collapses to empty → caller's api.github.com default applies
    cfg.GitHubPat = "\t\n";

    TrimTrackerCredentialFields(cfg);

    CHECK(cfg.GitHubBaseUrl.empty());
    CHECK(cfg.GitHubPat.empty());
}
