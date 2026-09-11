#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"
#include "JiraBackendInstancesPure.h"

#include <doctest/doctest.h>

using smatchet::jira_backends::AddExtra;
using smatchet::jira_backends::EnsureHydrated;
using smatchet::jira_backends::HostsMatch;
using smatchet::jira_backends::SelectActive;

TEST_CASE("ConfigManager Save persists live Domain when extras are absent") {
    smatchet_tests::TestEnvGuard env;
    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = "Jira";
    cfg.Domain = "http://127.0.0.1:9";
    cfg.Email = "loop@example.com";
    cfg.ApiToken = "tok-loop";
    ConfigManager::Save(cfg);
    ConfigManager::InvalidateCache();
    const TrackerConfig out = ConfigManager::Load();
    CHECK(out.Domain == "http://127.0.0.1:9");
    CHECK(out.Email == "loop@example.com");
    CHECK(out.ApiToken == "tok-loop");
    REQUIRE_FALSE(out.JiraBackends.empty());
    CHECK(out.JiraBackends[0].Domain == "http://127.0.0.1:9");
}

TEST_CASE("ConfigManager Save/Load extras inherit empty credentials and keep first at top-level keys") {
    smatchet_tests::TestEnvGuard env;

    TrackerConfig in;
    in.TrackerType = "Jira";
    in.Domain = "first.atlassian.net";
    in.Email = "a@example.com";
    in.ApiToken = "tok-a";
    EnsureHydrated(in);
    JiraBackendInstance extra;
    extra.Domain = "second.atlassian.net";
    REQUIRE(AddExtra(in, extra));
    REQUIRE(SelectActive(in, "second.atlassian.net"));
    ConfigManager::Save(in);
    ConfigManager::InvalidateCache();
    const TrackerConfig out = ConfigManager::Load();

    CHECK(out.Domain == "second.atlassian.net");
    CHECK(out.Email == "a@example.com");
    CHECK(out.ApiToken == "tok-a");
    REQUIRE(out.JiraBackends.size() == 2);
    CHECK(out.JiraBackends[0].Domain == "first.atlassian.net");
    CHECK(out.JiraBackends[0].Email == "a@example.com");
    CHECK(out.JiraBackends[0].ApiToken == "tok-a");
    CHECK(out.JiraBackends[1].Domain == "second.atlassian.net");
    CHECK(out.JiraBackends[1].Email.empty());
    CHECK(out.JiraBackends[1].ApiToken.empty());
    CHECK(HostsMatch(out.ActiveJiraDomain, "second.atlassian.net"));
}
