#include "Ui/PaneBackendTargetsPure.h"

#include "Config/JiraBackendInstancesPure.h"
#include "Ui/SmatchetBackendDisplay.h"

#include <cstddef>

namespace smatchet {
namespace pane_targets {
namespace {

// Separates the kind from the site in a row label; UTF-8 middle dot, as in DescribeCacheBackendKey.
constexpr const char* kSiteSeparator = " \xC2\xB7 ";

std::string RowLabel(const std::string& backendKey, const std::string& site) {
    std::string label = "New " + BackendDisplayName(backendKey); // P2-M18
    if (!site.empty()) {
        label += kSiteSeparator;
        label += site;
    }
    label += " pane";
    return label;
}

// A Jira site the picker can open: it names a host and has a token of its own or inherits the first
// site's (an extra site with an empty token uses instance 0's at resolve time).
bool JiraSiteUsable(const TrackerConfig& cfg, const JiraBackendInstance& site) {
    if (jira_backends::NormalizeJiraHost(site.Domain).empty()) {
        return false;
    }
    return !site.ApiToken.empty() || !cfg.JiraBackends[0].ApiToken.empty();
}

void AppendSingleJiraTarget(std::vector<NewPaneTarget>& out) {
    // One site: the row it always had, no site in the label and nothing to switch.
    NewPaneTarget t;
    t.BackendKey = "Jira";
    t.Label = RowLabel(t.BackendKey, std::string());
    out.push_back(t);
}

// `liveCredentialed`: the live (active) site has a host and token. Two or more usable sites get one
// row each even when it does not (the active site and the first site may both lack a token while
// two other sites carry their own); the single-site row keeps the old credential gate.
void AppendJiraTargets(const TrackerConfig& cfg, bool liveCredentialed, std::vector<NewPaneTarget>& out) {
    std::vector<const JiraBackendInstance*> sites;
    // Fewer than two entries (an empty list is not hydrated yet) is one site either way.
    if (cfg.JiraBackends.size() >= 2) {
        for (std::size_t i = 0; i < cfg.JiraBackends.size(); ++i) {
            if (JiraSiteUsable(cfg, cfg.JiraBackends[i])) {
                sites.push_back(&cfg.JiraBackends[i]);
            }
        }
    }
    if (sites.size() < 2) {
        if (liveCredentialed) {
            AppendSingleJiraTarget(out);
        }
        return;
    }
    // Same resolution as drawViewsJiraDomainPicker: the live origin when no site was ever picked.
    const std::string active = cfg.ActiveJiraDomain.empty() ? cfg.Domain : cfg.ActiveJiraDomain;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        NewPaneTarget t;
        t.BackendKey = "Jira";
        if (!jira_backends::HostsMatch(sites[i]->Domain, active)) {
            t.SwitchToJiraDomain = sites[i]->Domain;
        }
        t.Label = RowLabel(t.BackendKey, jira_backends::NormalizeJiraHost(sites[i]->Domain));
        out.push_back(t);
    }
}

} // namespace

std::vector<NewPaneTarget> BuildNewPaneTargets(const TrackerConfig& cfg) {
    std::vector<NewPaneTarget> out;
    const std::vector<std::string>& keys = ConfigManager::KnownBackendKeys();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const bool credentialed = ConfigManager::BackendCredentialsPresent(cfg, keys[i]);
        if (keys[i] == "Jira") {
            AppendJiraTargets(cfg, credentialed, out);
            continue;
        }
        if (!credentialed) {
            continue;
        }
        NewPaneTarget t;
        t.BackendKey = keys[i];
        t.Label = RowLabel(t.BackendKey, std::string());
        out.push_back(t);
    }
    return out;
}

} // namespace pane_targets
} // namespace smatchet
