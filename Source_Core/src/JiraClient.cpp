#include "JiraClient.h"

#include "JiraHttpUtils.h"

#include <sstream>
#include <string>

TrackerReachabilityProbeResult JiraClient::ProbeReachability(const JiraConfig& cfg) {
    TrackerReachabilityProbeResult out;
    std::string authErr;
    if (!EnsureJiraAuthConfig(cfg, authErr)) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = authErr.empty() ? std::string("Missing Jira domain or API token.") : authErr;
        return out;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string url = base + "/rest/api/3/myself";
    const cpr::Header headers = BuildJiraHeaders(cfg, false);
    const cpr::Response resp = JiraGetLogged(url, headers, kJiraProbeConnectTimeoutMs, kJiraProbeOverallTimeoutMs);

    const long sc = resp.status_code;
    if (sc == 200) {
        out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
        out.Diagnostic = "HTTP 200";
        return out;
    }

    if (sc == 401 || sc == 403) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        std::ostringstream oss;
        oss << "HTTP " << sc;
        out.Diagnostic = oss.str();
        return out;
    }

    if (sc >= 500 && sc < 600) {
        out.Kind = TrackerReachabilityProbeKind::ServiceUnavailable;
        std::ostringstream oss;
        oss << "HTTP " << sc;
        out.Diagnostic = oss.str();
        return out;
    }

    if (sc > 0 && sc < 500) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        std::ostringstream oss;
        oss << "HTTP " << sc;
        out.Diagnostic = oss.str();
        return out;
    }

    out.Kind = TrackerReachabilityProbeKind::TransportDown;
    out.Diagnostic = resp.error.message.empty() ? std::string("Unknown network error") : resp.error.message;
    return out;
}

std::string JiraClient::BuildBrowseUrl(const JiraConfig& cfg, const std::string& issueKey) const {
    if (cfg.Domain.empty() || issueKey.empty()) {
        return std::string();
    }
    return NormalizeBaseUrl(cfg.Domain) + "/browse/" + issueKey;
}
