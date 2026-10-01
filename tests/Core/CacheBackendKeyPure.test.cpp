// CacheBackendKeyPure — the local cache is namespaced by tracker site and account (#2268): a Jira host or
// account change, another Plane workspace, GitHub repo or Linear team gets its own namespace, and the
// legacy kind keys move to the configured site once.

#include "CacheBackendKeyPure.h"
#include "JiraBackendInstancesPure.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

using smatchet::cache_keys::CacheBackendKeyKind;
using smatchet::cache_keys::DescribeCacheBackendKey;
using smatchet::cache_keys::IsHeldCacheKey;
using smatchet::cache_keys::LegacyCacheKeyRekeys;
using smatchet::cache_keys::TrackerCacheBackendKey;
using smatchet::jira_backends::AddExtra;
using smatchet::jira_backends::ApplyActiveLiveFields;
using smatchet::jira_backends::EnsureHydrated;
using smatchet::jira_backends::SelectActive;

namespace {

// Expected account suffixes: the first 12 hex digits of SHA-256 of the lower-cased email.
const char* const kHashA = "08168cd80dfd"; // a@example.com
const char* const kHashB = "e8f39b3e1382"; // b@example.com

TrackerConfig JiraConfig(const std::string& domain, const std::string& email) {
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.Domain = domain;
    cfg.Email = email;
    EnsureHydrated(cfg);
    return cfg;
}

std::string RekeyTo(const std::vector<std::pair<std::string, std::string>>& rekeys, const std::string& from) {
    for (const std::pair<std::string, std::string>& r : rekeys) {
        if (r.first == from) {
            return r.second;
        }
    }
    return std::string();
}

} // namespace

TEST_CASE("TrackerCacheBackendKey names the Jira site and a hash of the account") {
    CHECK(TrackerCacheBackendKey(JiraConfig("https://First.Atlassian.Net/", "a@example.com")) ==
          std::string("Jira@first.atlassian.net#") + kHashA);
    // Case and surrounding whitespace in the email do not change the account.
    CHECK(TrackerCacheBackendKey(JiraConfig("first.atlassian.net", "  A@Example.COM ")) ==
          std::string("Jira@first.atlassian.net#") + kHashA);
    // Another account on the same site is another namespace.
    CHECK(TrackerCacheBackendKey(JiraConfig("first.atlassian.net", "b@example.com")) ==
          std::string("Jira@first.atlassian.net#") + kHashB);
    // No email configured: the site alone.
    CHECK(TrackerCacheBackendKey(JiraConfig("first.atlassian.net", "")) == "Jira@first.atlassian.net");
    // Nothing configured (or a fixture backend): the bare kind, as before.
    CHECK(TrackerCacheBackendKey(JiraConfig("", "")) == "Jira");
}

TEST_CASE("TrackerCacheBackendKey never carries the email address") {
    const std::string key = TrackerCacheBackendKey(JiraConfig("first.atlassian.net", "secret.person@example.com"));
    CHECK(key.find("secret") == std::string::npos);
    CHECK(key.find("example.com") == std::string::npos);
    CHECK(std::count(key.begin(), key.end(), '@') == 1); // the only '@' is the kind separator
}

TEST_CASE("TrackerCacheBackendKey keys an extra Jira site by its host and inherited account") {
    TrackerConfig cfg = JiraConfig("first.atlassian.net", "a@example.com");
    JiraBackendInstance extra;
    extra.Domain = "Second.Atlassian.Net";
    REQUIRE(AddExtra(cfg, extra));
    REQUIRE(SelectActive(cfg, "second.atlassian.net"));
    ApplyActiveLiveFields(cfg);
    CHECK(TrackerCacheBackendKey(cfg) == std::string("Jira@second.atlassian.net#") + kHashA);

    extra.Domain = "[2001:db8::2]";
    extra.Email = "b@example.com";
    REQUIRE(AddExtra(cfg, extra));
    REQUIRE(SelectActive(cfg, "[2001:db8::2]"));
    ApplyActiveLiveFields(cfg);
    CHECK(TrackerCacheBackendKey(cfg) == std::string("Jira@[2001:db8::2]#") + kHashB);
}

TEST_CASE("TrackerCacheBackendKey falls back to the instance email when the live email is empty") {
    TrackerConfig cfg = JiraConfig("first.atlassian.net", "a@example.com");
    cfg.Email.clear();
    CHECK(TrackerCacheBackendKey(cfg) == std::string("Jira@first.atlassian.net#") + kHashA);
}

TEST_CASE("TrackerCacheBackendKey names the Plane workspace, GitHub repo and Linear team") {
    TrackerConfig cfg;
    cfg.TrackerType = "Plane";
    CHECK(TrackerCacheBackendKey(cfg) == "Plane");
    cfg.PlaneUrl = "https://app.plane.so";
    cfg.PlaneWorkspaceSlug = "My-Space";
    // The web-app alias maps to the API host PlaneClient actually talks to.
    CHECK(TrackerCacheBackendKey(cfg) == "Plane@api.plane.so/my-space");
    cfg.PlaneUrl = "https://plane.internal:8443/";
    CHECK(TrackerCacheBackendKey(cfg) == "Plane@plane.internal/my-space");

    cfg.TrackerType = "GitHub";
    CHECK(TrackerCacheBackendKey(cfg) == "GitHub");
    cfg.GitHubOwner = "AlexK";
    cfg.GitHubRepo = "Smatchet";
    CHECK(TrackerCacheBackendKey(cfg) == "GitHub@api.github.com/alexk/smatchet");
    cfg.GitHubBaseUrl = "https://ghe.example.com/api/v3";
    CHECK(TrackerCacheBackendKey(cfg) == "GitHub@ghe.example.com/alexk/smatchet");

    cfg.TrackerType = "Linear";
    CHECK(TrackerCacheBackendKey(cfg) == "Linear");
    cfg.LinearTeamKey = "ENG";
    CHECK(TrackerCacheBackendKey(cfg) == "Linear@api.linear.app/eng");
    // The team id wins over the key: it survives a team rename.
    cfg.LinearTeamId = "0F1E2D3C-UUID";
    CHECK(TrackerCacheBackendKey(cfg) == "Linear@api.linear.app/0f1e2d3c-uuid");
    cfg.LinearBaseUrl.clear();
    CHECK(TrackerCacheBackendKey(cfg) == "Linear@api.linear.app/0f1e2d3c-uuid");
}

TEST_CASE("LegacyCacheKeyRekeys moves every legacy kind key to its configured site") {
    TrackerConfig cfg = JiraConfig("first.atlassian.net", "a@example.com");
    JiraBackendInstance extra;
    extra.Domain = "https://Second.Atlassian.Net";
    REQUIRE(AddExtra(cfg, extra));
    cfg.PlaneUrl = "https://api.plane.so";
    cfg.PlaneWorkspaceSlug = "ws";
    cfg.GitHubOwner = "o";
    cfg.GitHubRepo = "r";
    // Whichever tracker is active, every configured one migrates.
    cfg.TrackerType = "Plane";

    const std::vector<std::pair<std::string, std::string>> rekeys = LegacyCacheKeyRekeys(cfg);
    CHECK(RekeyTo(rekeys, "Jira") == std::string("Jira@first.atlassian.net#") + kHashA);
    CHECK(RekeyTo(rekeys, "Jira:second.atlassian.net") == std::string("Jira@second.atlassian.net#") + kHashA);
    CHECK(RekeyTo(rekeys, "Plane") == "Plane@api.plane.so/ws");
    CHECK(RekeyTo(rekeys, "GitHub") == "GitHub@api.github.com/o/r");
    // Linear is not configured: its rows keep the legacy key, which is also its current key.
    CHECK(RekeyTo(rekeys, "Linear").empty());
    CHECK(rekeys.size() == 4);
}

TEST_CASE("LegacyCacheKeyRekeys agrees with TrackerCacheBackendKey after the move") {
    TrackerConfig cfg = JiraConfig("first.atlassian.net", "a@example.com");
    CHECK(RekeyTo(LegacyCacheKeyRekeys(cfg), "Jira") == TrackerCacheBackendKey(cfg));
}

TEST_CASE("LegacyCacheKeyRekeys is empty when nothing is configured") {
    TrackerConfig cfg;
    CHECK(LegacyCacheKeyRekeys(cfg).empty());
}

TEST_CASE("CacheBackendKeyKind and DescribeCacheBackendKey read site and legacy keys") {
    const std::string jira = std::string("Jira@acme.atlassian.net#") + kHashA;
    CHECK(CacheBackendKeyKind(jira) == "Jira");
    CHECK(CacheBackendKeyKind("Jira:acme.atlassian.net") == "Jira");
    CHECK(CacheBackendKeyKind("Plane") == "Plane");
    CHECK(DescribeCacheBackendKey(jira) == "Jira \xC2\xB7 acme.atlassian.net");
    CHECK(DescribeCacheBackendKey("Jira:acme.atlassian.net") == "Jira \xC2\xB7 acme.atlassian.net");
    CHECK(DescribeCacheBackendKey("Plane@api.plane.so/ws") == "Plane \xC2\xB7 api.plane.so/ws");
    CHECK(DescribeCacheBackendKey("GitHub") == "GitHub");
    CHECK(DescribeCacheBackendKey("").empty());
}

TEST_CASE("IsHeldCacheKey holds a row whose site no live pane uses") {
    const std::vector<std::string> live{"Jira@a.atlassian.net#08168cd80dfd", "Plane@api.plane.so/ws"};
    CHECK_FALSE(IsHeldCacheKey("Jira@a.atlassian.net#08168cd80dfd", live));
    CHECK_FALSE(IsHeldCacheKey("Plane@api.plane.so/ws", live));
    CHECK(IsHeldCacheKey("Jira@a.atlassian.net#e8f39b3e1382", live)); // same site, another account
    CHECK(IsHeldCacheKey("Jira", live));
    CHECK(IsHeldCacheKey("", live));
    CHECK(IsHeldCacheKey("Jira@a.atlassian.net#08168cd80dfd", std::vector<std::string>()));
}
