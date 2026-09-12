#include "ConfigManager_Internal.h"
#include "JiraBackendInstancesPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet::config_detail::SanitizeHeaderBoundConfigKeys;
using smatchet::jira_backends::AddExtra;
using smatchet::jira_backends::AdoptLoadedExtras;
using smatchet::jira_backends::ApplyActiveLiveFields;
using smatchet::jira_backends::EnsureHydrated;
using smatchet::jira_backends::HostsMatch;
using smatchet::jira_backends::NormalizeJiraHost;
using smatchet::jira_backends::PrepareForPersist;
using smatchet::jira_backends::RemoveExtraAt;
using smatchet::jira_backends::ReplaceExtras;
using smatchet::jira_backends::SelectActive;
using smatchet::jira_backends::TrackerCacheBackendKey;

TEST_CASE("NormalizeJiraHost strips scheme, path, port, and case") {
    CHECK(NormalizeJiraHost("https://Foo.Atlassian.Net/wiki") == "foo.atlassian.net");
    CHECK(NormalizeJiraHost("http://bar.example.com:8080/jira") == "bar.example.com");
    CHECK(NormalizeJiraHost("  Company.Atlassian.Net.  ") == "company.atlassian.net");
    CHECK(NormalizeJiraHost("") == "");
    CHECK(HostsMatch("https://a.atlassian.net/x", "A.Atlassian.Net"));
    CHECK_FALSE(HostsMatch("a.atlassian.net", "b.atlassian.net"));
    CHECK_FALSE(HostsMatch("", ""));
}

TEST_CASE("NormalizeJiraHost keeps bracketed IPv6 authority") {
    CHECK(NormalizeJiraHost("https://[2001:db8::1]:8443/jira") == "[2001:db8::1]");
    CHECK(NormalizeJiraHost("http://[::1]/jira") == "[::1]");
    CHECK(HostsMatch("https://[2001:DB8::1]:443", "[2001:db8::1]"));
    CHECK_FALSE(HostsMatch("https://[2001:db8::1]", "https://[2001:db8::2]"));
}

TEST_CASE("empty extra email/token inherit from the first instance") {
    TrackerConfig cfg;
    cfg.Domain = "first.atlassian.net";
    cfg.Email = "a@example.com";
    cfg.ApiToken = "tok-a";
    EnsureHydrated(cfg);
    JiraBackendInstance extra;
    extra.Domain = "second.atlassian.net";
    REQUIRE(AddExtra(cfg, extra));
    REQUIRE(SelectActive(cfg, "second.atlassian.net"));
    CHECK(cfg.Domain == "second.atlassian.net");
    CHECK(cfg.Email == "a@example.com");
    CHECK(cfg.ApiToken == "tok-a");
    REQUIRE(cfg.JiraBackends.size() == 2);
    CHECK(cfg.JiraBackends[1].Email.empty());
    CHECK(cfg.JiraBackends[1].ApiToken.empty());
}

TEST_CASE("extra email/token override first instance when set") {
    TrackerConfig cfg;
    cfg.Domain = "first.atlassian.net";
    cfg.Email = "a@example.com";
    cfg.ApiToken = "tok-a";
    EnsureHydrated(cfg);
    JiraBackendInstance extra;
    extra.Domain = "second.atlassian.net";
    extra.Email = "b@example.com";
    extra.ApiToken = "tok-b";
    REQUIRE(AddExtra(cfg, extra));
    REQUIRE(SelectActive(cfg, "https://second.atlassian.net"));
    CHECK(cfg.Email == "b@example.com");
    CHECK(cfg.ApiToken == "tok-b");
}

TEST_CASE("AddExtra rejects empty and duplicate hosts") {
    TrackerConfig cfg;
    cfg.Domain = "first.atlassian.net";
    EnsureHydrated(cfg);
    JiraBackendInstance empty;
    CHECK_FALSE(AddExtra(cfg, empty));
    JiraBackendInstance dup;
    dup.Domain = "https://first.atlassian.net";
    CHECK_FALSE(AddExtra(cfg, dup));
    JiraBackendInstance extra;
    extra.Domain = "second.atlassian.net";
    REQUIRE(AddExtra(cfg, extra));
    CHECK_FALSE(AddExtra(cfg, extra));
    CHECK(cfg.JiraBackends.size() == 2);
}

TEST_CASE("SelectActive unknown host fails; remove of active extra falls back to first") {
    TrackerConfig cfg;
    cfg.Domain = "first.atlassian.net";
    cfg.Email = "a@example.com";
    cfg.ApiToken = "tok-a";
    EnsureHydrated(cfg);
    JiraBackendInstance extra;
    extra.Domain = "second.atlassian.net";
    extra.Email = "b@example.com";
    extra.ApiToken = "tok-b";
    REQUIRE(AddExtra(cfg, extra));
    CHECK_FALSE(SelectActive(cfg, "missing.atlassian.net"));
    REQUIRE(SelectActive(cfg, "second.atlassian.net"));
    CHECK(cfg.Email == "b@example.com");
    REQUIRE(RemoveExtraAt(cfg, 0));
    CHECK(cfg.JiraBackends.size() == 1);
    CHECK(HostsMatch(cfg.Domain, "first.atlassian.net"));
    CHECK(cfg.Email == "a@example.com");
    CHECK_FALSE(RemoveExtraAt(cfg, 0));
}

TEST_CASE("AdoptLoadedExtras prepends first instance so persist keeps extras") {
    TrackerConfig cfg;
    cfg.Domain = "first.atlassian.net";
    cfg.Email = "a@example.com";
    cfg.ApiToken = "tok-a";
    JiraBackendInstance extra;
    extra.Domain = "second.atlassian.net";
    cfg.JiraBackends.push_back(extra);
    AdoptLoadedExtras(cfg);
    REQUIRE(cfg.JiraBackends.size() == 2);
    CHECK(cfg.JiraBackends[0].Domain == "first.atlassian.net");
    CHECK(cfg.JiraBackends[1].Domain == "second.atlassian.net");
    CHECK(cfg.Domain == "first.atlassian.net");
    PrepareForPersist(cfg);
    CHECK(cfg.Domain == "first.atlassian.net");
    REQUIRE(cfg.JiraBackends.size() == 2);
    CHECK(cfg.JiraBackends[1].Domain == "second.atlassian.net");
}

TEST_CASE("ReplaceExtras remaps ActiveJiraDomain onto the renamed extra") {
    TrackerConfig cfg;
    cfg.Domain = "first.atlassian.net";
    EnsureHydrated(cfg);
    JiraBackendInstance extra;
    extra.Domain = "second.atlassian.net";
    REQUIRE(AddExtra(cfg, extra));
    REQUIRE(SelectActive(cfg, "second.atlassian.net"));
    JiraBackendInstance renamed;
    renamed.Domain = "third.atlassian.net";
    std::vector<JiraBackendInstance> extras;
    extras.push_back(renamed);
    REQUIRE(ReplaceExtras(cfg, extras));
    CHECK(HostsMatch(cfg.ActiveJiraDomain, "third.atlassian.net"));
    CHECK(cfg.Domain == "second.atlassian.net");
    extras.clear();
    REQUIRE(ReplaceExtras(cfg, extras));
    CHECK(HostsMatch(cfg.ActiveJiraDomain, "first.atlassian.net"));
}

TEST_CASE("ReplaceExtras rejects a duplicate extra host") {
    TrackerConfig cfg;
    cfg.Domain = "first.atlassian.net";
    EnsureHydrated(cfg);
    JiraBackendInstance dup;
    dup.Domain = "first.atlassian.net";
    std::vector<JiraBackendInstance> extras;
    extras.push_back(dup);
    CHECK_FALSE(ReplaceExtras(cfg, extras));
    CHECK(cfg.JiraBackends.size() == 1);
}

TEST_CASE("PrepareForPersist copies live first-instance Domain onto JiraBackends 0") {
    TrackerConfig cfg;
    cfg.Domain = "old.atlassian.net";
    cfg.Email = "old@example.com";
    cfg.ApiToken = "tok-old";
    EnsureHydrated(cfg);
    cfg.Domain = "http://127.0.0.1:9";
    cfg.Email = "loop@example.com";
    cfg.ApiToken = "tok-loop";
    PrepareForPersist(cfg);
    CHECK(cfg.JiraBackends[0].Domain == "http://127.0.0.1:9");
    CHECK(cfg.JiraBackends[0].Email == "loop@example.com");
    CHECK(cfg.JiraBackends[0].ApiToken == "tok-loop");
    CHECK(cfg.Domain == "http://127.0.0.1:9");
}

TEST_CASE("PrepareForPersist writes first instance onto live Domain, not the extra") {
    TrackerConfig cfg;
    cfg.Domain = "first.atlassian.net";
    cfg.Email = "a@example.com";
    cfg.ApiToken = "tok-a";
    EnsureHydrated(cfg);
    JiraBackendInstance extra;
    extra.Domain = "second.atlassian.net";
    extra.Email = "b@example.com";
    extra.ApiToken = "tok-b";
    REQUIRE(AddExtra(cfg, extra));
    REQUIRE(SelectActive(cfg, "second.atlassian.net"));
    CHECK(cfg.Domain == "second.atlassian.net");
    PrepareForPersist(cfg);
    CHECK(cfg.Domain == "first.atlassian.net");
    CHECK(cfg.Email == "a@example.com");
    CHECK(cfg.ApiToken == "tok-a");
    CHECK(cfg.ActiveJiraDomain == "second.atlassian.net");
}

TEST_CASE("TrackerCacheBackendKey is Jira for first and Jira:<host> for extras") {
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.Domain = "first.atlassian.net";
    EnsureHydrated(cfg);
    CHECK(TrackerCacheBackendKey(cfg) == "Jira");
    JiraBackendInstance extra;
    extra.Domain = "Second.Atlassian.Net";
    REQUIRE(AddExtra(cfg, extra));
    REQUIRE(SelectActive(cfg, "second.atlassian.net"));
    CHECK(TrackerCacheBackendKey(cfg) == "Jira:second.atlassian.net");
    extra.Domain = "[2001:db8::2]";
    cfg.ActiveJiraDomain = cfg.JiraBackends[0].Domain;
    ApplyActiveLiveFields(cfg);
    REQUIRE(AddExtra(cfg, extra));
    REQUIRE(SelectActive(cfg, "[2001:db8::2]"));
    CHECK(TrackerCacheBackendKey(cfg) == "Jira:[2001:db8::2]");
    cfg.TrackerType = "Plane";
    CHECK(TrackerCacheBackendKey(cfg) == "Plane");
}

TEST_CASE("SanitizeHeaderBoundConfigKeys strips CR/LF from jira_backends[].domain") {
    nlohmann::json j;
    j["jira_backends"] = nlohmann::json::array();
    nlohmann::json one = nlohmann::json::object();
    one["domain"] = "evil.example.com\r\nX-Injected: 1";
    j["jira_backends"].push_back(std::move(one));
    SanitizeHeaderBoundConfigKeys(j);
    CHECK(j["jira_backends"][0]["domain"].get<std::string>() == "evil.example.comX-Injected: 1");
}
