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

TEST_CASE("TrimTrackerCredentialFields — trims extra Jira backend rows") {
    TrackerConfig cfg;
    JiraBackendInstance extra;
    extra.Domain = "  other.atlassian.net ";
    extra.Email = " extra@example.com ";
    extra.ApiToken = " tok-extra ";
    cfg.JiraBackends.push_back(extra);
    TrimTrackerCredentialFields(cfg);
    CHECK(cfg.JiraBackends[0].Domain == "other.atlassian.net");
    CHECK(cfg.JiraBackends[0].Email == "extra@example.com");
    CHECK(cfg.JiraBackends[0].ApiToken == "tok-extra");
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

// Audit #80 — what the user pasted into a site field is stored in the plain form the base-URL builders expect,
// even when the caret never left the field (Enter saves the form). The tolerant reading itself is covered by
// TrackerInputNormalisePure.test.cpp; this pins how Save applies it.
using SmatchetPreferencesUiDetail::NormaliseTrackerSiteFields;

TEST_CASE("NormaliseTrackerSiteFields — a pasted Jira board URL is stored as the host") {
    TrackerConfig cfg;
    cfg.Domain = "https://acme.atlassian.net/jira/software/projects/PROJ/boards/1";
    JiraBackendInstance extra;
    extra.Domain = "https://other.atlassian.net/browse/X-1";
    cfg.JiraBackends.push_back(extra);
    NormaliseTrackerSiteFields(cfg);
    CHECK(cfg.Domain == "acme.atlassian.net");
    CHECK(cfg.JiraBackends[0].Domain == "other.atlassian.net");
}

TEST_CASE("NormaliseTrackerSiteFields — a Plane app URL gives the API base and the workspace") {
    TrackerConfig cfg;
    cfg.PlaneUrl = "https://app.plane.so/acme/projects/1234/issues/";
    NormaliseTrackerSiteFields(cfg);
    CHECK(cfg.PlaneUrl == "https://api.plane.so");
    CHECK(cfg.PlaneWorkspaceSlug == "acme");
}

TEST_CASE("NormaliseTrackerSiteFields — a workspace the user already set is never overwritten") {
    TrackerConfig cfg;
    cfg.PlaneUrl = "https://app.plane.so/acme/projects";
    cfg.PlaneWorkspaceSlug = "mine";
    NormaliseTrackerSiteFields(cfg);
    CHECK(cfg.PlaneWorkspaceSlug == "mine");
}

TEST_CASE("NormaliseTrackerSiteFields — a GitHub repository URL fills the base, owner and repo") {
    TrackerConfig cfg;
    cfg.GitHubBaseUrl = "https://github.com/octo/repo";
    NormaliseTrackerSiteFields(cfg);
    CHECK(cfg.GitHubBaseUrl == "https://api.github.com");
    CHECK(cfg.GitHubOwner == "octo");
    CHECK(cfg.GitHubRepo == "repo");

    TrackerConfig set;
    set.GitHubBaseUrl = "https://github.com/octo/repo";
    set.GitHubOwner = "mine";
    set.GitHubRepo = "keep";
    NormaliseTrackerSiteFields(set);
    CHECK(set.GitHubOwner == "mine");
    CHECK(set.GitHubRepo == "keep");
}

TEST_CASE("NormaliseTrackerSiteFields — owner/repo typed into Owner splits, and unreadable text is left as typed") {
    TrackerConfig cfg;
    cfg.GitHubOwner = "octo/repo";
    NormaliseTrackerSiteFields(cfg);
    CHECK(cfg.GitHubOwner == "octo");
    CHECK(cfg.GitHubRepo == "repo");

    TrackerConfig bad;
    bad.Domain = "acme$atlassian.net";
    bad.PlaneUrl = "ftp://plane.example";
    NormaliseTrackerSiteFields(bad);
    CHECK(bad.Domain == "acme$atlassian.net");
    CHECK(bad.PlaneUrl == "ftp://plane.example");
}

TEST_CASE("NormaliseTrackerSiteFields — values already in plain form and empty fields are untouched") {
    TrackerConfig cfg;
    cfg.Domain = "company.atlassian.net";
    cfg.PlaneUrl = "https://api.plane.so";
    cfg.GitHubBaseUrl = "https://api.github.com";
    NormaliseTrackerSiteFields(cfg);
    CHECK(cfg.Domain == "company.atlassian.net");
    CHECK(cfg.PlaneUrl == "https://api.plane.so");
    CHECK(cfg.GitHubBaseUrl == "https://api.github.com");
    CHECK(cfg.PlaneWorkspaceSlug.empty());
    CHECK(cfg.GitHubOwner.empty());

    TrackerConfig empty;
    NormaliseTrackerSiteFields(empty);
    CHECK(empty.Domain.empty());
    CHECK(empty.GitHubBaseUrl.empty());
}
