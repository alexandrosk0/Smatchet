#include "JiraBackendInstancesPure.h"

#include "StringUtil.h"

#include <cstddef>
#include <iterator>

namespace smatchet {
namespace jira_backends {
namespace {

std::string StripScheme(const std::string& raw) {
    std::string s = TrimCopyAsciiWhitespace(raw);
    if (s.size() >= 8 && ToLowerAsciiCopy(s.substr(0, 8)) == "https://") {
        s.erase(0, 8);
    } else if (s.size() >= 7 && ToLowerAsciiCopy(s.substr(0, 7)) == "http://") {
        s.erase(0, 7);
    }
    return s;
}

} // namespace

std::string NormalizeJiraHost(const std::string& domain) {
    std::string s = StripScheme(domain);
    const std::string::size_type slash = s.find('/');
    if (slash != std::string::npos) {
        s.resize(slash);
    }
    const std::string::size_type colon = s.find(':');
    if (colon != std::string::npos) {
        s.resize(colon);
    }
    while (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return ToLowerAsciiCopy(s);
}

bool HostsMatch(const std::string& a, const std::string& b) {
    const std::string ha = NormalizeJiraHost(a);
    const std::string hb = NormalizeJiraHost(b);
    return !ha.empty() && ha == hb;
}

void EnsureHydrated(TrackerConfig& cfg) {
    if (!cfg.JiraBackends.empty()) {
        return;
    }
    JiraBackendInstance first;
    first.Domain = cfg.Domain;
    first.Email = cfg.Email;
    first.ApiToken = cfg.ApiToken;
    cfg.JiraBackends.push_back(std::move(first));
}

void AdoptLoadedExtras(TrackerConfig& cfg) {
    std::vector<JiraBackendInstance> extras = std::move(cfg.JiraBackends);
    cfg.JiraBackends.clear();
    EnsureHydrated(cfg);
    cfg.JiraBackends[0].Domain = cfg.Domain;
    cfg.JiraBackends[0].Email = cfg.Email;
    cfg.JiraBackends[0].ApiToken = cfg.ApiToken;
    for (std::size_t i = 0; i < extras.size(); ++i) {
        AddExtra(cfg, extras[i]);
    }
    ApplyActiveLiveFields(cfg);
}

void PrepareForPersist(TrackerConfig& cfg) {
    EnsureHydrated(cfg);
    cfg.Domain = cfg.JiraBackends[0].Domain;
    cfg.Email = cfg.JiraBackends[0].Email;
    cfg.ApiToken = cfg.JiraBackends[0].ApiToken;
}

const JiraBackendInstance* FindByHost(const TrackerConfig& cfg, const std::string& domain) {
    const std::string want = NormalizeJiraHost(domain);
    if (want.empty()) {
        return nullptr;
    }
    for (const JiraBackendInstance& inst : cfg.JiraBackends) {
        if (NormalizeJiraHost(inst.Domain) == want) {
            return &inst;
        }
    }
    return nullptr;
}

void ApplyActiveLiveFields(TrackerConfig& cfg) {
    EnsureHydrated(cfg);
    const std::string want =
        cfg.ActiveJiraDomain.empty() ? cfg.JiraBackends[0].Domain : cfg.ActiveJiraDomain;
    const JiraBackendInstance* inst = FindByHost(cfg, want);
    if (!inst) {
        inst = &cfg.JiraBackends[0];
        cfg.ActiveJiraDomain = cfg.JiraBackends[0].Domain;
    }
    cfg.Domain = inst->Domain;
    cfg.Email = inst->Email.empty() ? cfg.JiraBackends[0].Email : inst->Email;
    cfg.ApiToken = inst->ApiToken.empty() ? cfg.JiraBackends[0].ApiToken : inst->ApiToken;
}

bool SelectActive(TrackerConfig& cfg, const std::string& domain) {
    EnsureHydrated(cfg);
    const JiraBackendInstance* inst = FindByHost(cfg, domain);
    if (!inst) {
        return false;
    }
    cfg.ActiveJiraDomain = inst->Domain;
    ApplyActiveLiveFields(cfg);
    return true;
}

bool AddExtra(TrackerConfig& cfg, const JiraBackendInstance& inst) {
    EnsureHydrated(cfg);
    const std::string host = NormalizeJiraHost(inst.Domain);
    if (host.empty()) {
        return false;
    }
    if (FindByHost(cfg, inst.Domain) != nullptr) {
        return false;
    }
    JiraBackendInstance copy = inst;
    copy.Domain = TrimCopyAsciiWhitespace(inst.Domain);
    copy.Email = TrimCopyAsciiWhitespace(inst.Email);
    copy.ApiToken = TrimCopyAsciiWhitespace(inst.ApiToken);
    cfg.JiraBackends.push_back(std::move(copy));
    return true;
}

bool RemoveExtraAt(TrackerConfig& cfg, std::size_t extraIndex) {
    EnsureHydrated(cfg);
    const std::size_t idx = extraIndex + 1;
    if (idx >= cfg.JiraBackends.size()) {
        return false;
    }
    const bool removingActive = HostsMatch(cfg.JiraBackends[idx].Domain, cfg.ActiveJiraDomain) ||
                                HostsMatch(cfg.JiraBackends[idx].Domain, cfg.Domain);
    cfg.JiraBackends.erase(cfg.JiraBackends.begin() + static_cast<std::ptrdiff_t>(idx));
    if (removingActive) {
        cfg.ActiveJiraDomain = cfg.JiraBackends[0].Domain;
        ApplyActiveLiveFields(cfg);
    }
    return true;
}

std::string TrackerCacheBackendKey(const TrackerConfig& cfg) {
    const std::string kind = ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType);
    if (kind != "Jira") {
        return kind;
    }
    if (cfg.JiraBackends.empty()) {
        return "Jira";
    }
    const std::string live = NormalizeJiraHost(cfg.Domain.empty() ? cfg.ActiveJiraDomain : cfg.Domain);
    const std::string first = NormalizeJiraHost(cfg.JiraBackends[0].Domain);
    if (live.empty() || live == first) {
        return "Jira";
    }
    return std::string("Jira:") + live;
}

} // namespace jira_backends
} // namespace smatchet
