#include "CacheBackendKeyPure.h"

#include "JiraBackendInstancesPure.h"
#include "Sha256.h"
#include "StringUtil.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace smatchet {
namespace cache_keys {
namespace {

// 48 bits of the account digest: enough to tell a user's accounts apart, short enough to read in a log.
constexpr std::size_t kAccountHashHexChars = 12;
// The hosts each client talks to when its base URL is left empty (or given as the web-app alias).
constexpr const char* kPlaneCloudApiHost = "api.plane.so";
constexpr const char* kPlaneCloudAppHost = "app.plane.so";
constexpr const char* kGitHubDefaultApiHost = "api.github.com";
constexpr const char* kLinearDefaultApiHost = "api.linear.app";
// Separates the kind from the site in a label; UTF-8 middle dot.
constexpr const char* kLabelSeparator = " \xC2\xB7 ";

std::string LowerTrimmed(const std::string& s) { return ToLowerAsciiCopy(TrimCopyAsciiWhitespace(s)); }

std::string HostOr(const std::string& url, const char* fallback) {
    const std::string host = jira_backends::NormalizeJiraHost(url);
    return host.empty() ? std::string(fallback) : host;
}

std::string JiraKey(const std::string& domain, const std::string& email) {
    const std::string host = jira_backends::NormalizeJiraHost(domain);
    if (host.empty()) {
        return "Jira";
    }
    std::string key = "Jira@" + host;
    const std::string account = LowerTrimmed(email);
    if (!account.empty()) {
        key += '#';
        key += hashing::Sha256Hex(account).substr(0, kAccountHashHexChars);
    }
    return key;
}

std::string PlaneKey(const TrackerConfig& cfg) {
    std::string host = jira_backends::NormalizeJiraHost(cfg.PlaneUrl);
    if (host == kPlaneCloudAppHost) {
        host = kPlaneCloudApiHost; // PlaneClient sends app.plane.so traffic to the API host
    }
    const std::string workspace = LowerTrimmed(cfg.PlaneWorkspaceSlug);
    if (host.empty() || workspace.empty()) {
        return "Plane";
    }
    return "Plane@" + host + "/" + workspace;
}

std::string GitHubKey(const TrackerConfig& cfg) {
    const std::string owner = LowerTrimmed(cfg.GitHubOwner);
    const std::string repo = LowerTrimmed(cfg.GitHubRepo);
    if (owner.empty() || repo.empty()) {
        return "GitHub";
    }
    return "GitHub@" + HostOr(cfg.GitHubBaseUrl, kGitHubDefaultApiHost) + "/" + owner + "/" + repo;
}

std::string LinearKey(const TrackerConfig& cfg) {
    // The team UUID is stable across renames; the team key is the fallback when no id is configured.
    std::string team = LowerTrimmed(cfg.LinearTeamId);
    if (team.empty()) {
        team = LowerTrimmed(cfg.LinearTeamKey);
    }
    if (team.empty()) {
        return "Linear";
    }
    return "Linear@" + HostOr(cfg.LinearBaseUrl, kLinearDefaultApiHost) + "/" + team;
}

// The account a Jira site is used with: the live (resolved) email, else the instance's own, else the
// first instance's, which an extra site inherits when it has none.
std::string JiraAccountEmail(const TrackerConfig& cfg, const std::string& domain) {
    if (!TrimCopyAsciiWhitespace(cfg.Email).empty()) {
        return cfg.Email;
    }
    const JiraBackendInstance* inst = jira_backends::FindByHost(cfg, domain);
    if (inst != nullptr && !inst->Email.empty()) {
        return inst->Email;
    }
    return cfg.JiraBackends.empty() ? std::string() : cfg.JiraBackends[0].Email;
}

std::string LiveJiraDomain(const TrackerConfig& cfg) {
    if (!cfg.Domain.empty()) {
        return cfg.Domain;
    }
    if (!cfg.ActiveJiraDomain.empty()) {
        return cfg.ActiveJiraDomain;
    }
    return cfg.JiraBackends.empty() ? std::string() : cfg.JiraBackends[0].Domain;
}

} // namespace

std::string TrackerCacheBackendKey(const TrackerConfig& cfg) {
    const std::string kind = ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType);
    if (kind == "Plane") {
        return PlaneKey(cfg);
    }
    if (kind == "GitHub") {
        return GitHubKey(cfg);
    }
    if (kind == "Linear") {
        return LinearKey(cfg);
    }
    if (kind != "Jira") {
        return kind;
    }
    const std::string domain = LiveJiraDomain(cfg);
    return JiraKey(domain, JiraAccountEmail(cfg, domain));
}

std::vector<std::pair<std::string, std::string>> LegacyCacheKeyRekeys(const TrackerConfig& cfg) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto add = [&out](const std::string& from, const std::string& to) {
        if (to != from) {
            out.emplace_back(from, to);
        }
    };
    // Legacy "Jira" was the first site; "Jira:<host>" an extra one, keyed by its normalized host.
    const bool hydrated = !cfg.JiraBackends.empty();
    const std::string firstDomain = hydrated ? cfg.JiraBackends[0].Domain : cfg.Domain;
    const std::string firstEmail = hydrated ? cfg.JiraBackends[0].Email : cfg.Email;
    add("Jira", JiraKey(firstDomain, firstEmail));
    for (std::size_t i = 1; i < cfg.JiraBackends.size(); ++i) {
        const JiraBackendInstance& extra = cfg.JiraBackends[i];
        const std::string host = jira_backends::NormalizeJiraHost(extra.Domain);
        if (!host.empty()) {
            add("Jira:" + host, JiraKey(extra.Domain, extra.Email.empty() ? firstEmail : extra.Email));
        }
    }
    add("Plane", PlaneKey(cfg));
    add("GitHub", GitHubKey(cfg));
    add("Linear", LinearKey(cfg));
    return out;
}

std::string CacheBackendKeyKind(const std::string& key) {
    const std::string::size_type sep = key.find_first_of("@:");
    return sep == std::string::npos ? key : key.substr(0, sep);
}

std::string DescribeCacheBackendKey(const std::string& key) {
    const std::string::size_type sep = key.find_first_of("@:");
    if (sep == std::string::npos) {
        return key;
    }
    std::string site = key.substr(sep + 1);
    const std::string::size_type account = site.find('#');
    if (account != std::string::npos) {
        site.resize(account);
    }
    const std::string kind = key.substr(0, sep);
    return site.empty() ? kind : kind + kLabelSeparator + site;
}

bool IsHeldCacheKey(const std::string& rowKey, const std::vector<std::string>& liveKeys) {
    return rowKey.empty() || std::find(liveKeys.begin(), liveKeys.end(), rowKey) == liveKeys.end();
}

} // namespace cache_keys
} // namespace smatchet
