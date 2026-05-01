#include "JiraClient.h"

#include "JiraHttpUtils.h"

#include <sstream>
#include <string>

JiraReachabilityProbeResult JiraClient::ProbeReachability(const JiraConfig& cfg) {
    JiraReachabilityProbeResult out;
    std::string authErr;
    if (!EnsureJiraAuthConfig(cfg, authErr)) {
        out.Kind = JiraReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = authErr.empty() ? std::string("Missing Jira domain or API token.") : authErr;
        return out;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string url = base + "/rest/api/3/myself";
    const cpr::Header headers = BuildJiraHeaders(cfg, false);
    const cpr::Response resp = JiraGetLogged(url, headers, kJiraProbeConnectTimeoutMs, kJiraProbeOverallTimeoutMs);

    const long sc = resp.status_code;
    if (sc == 200) {
        out.Kind = JiraReachabilityProbeKind::AuthenticatedReachable;
        out.Diagnostic = "HTTP 200";
        return out;
    }

    if (sc == 401 || sc == 403) {
        out.Kind = JiraReachabilityProbeKind::ReachableAuthOrConfigError;
        std::ostringstream oss;
        oss << "HTTP " << sc;
        out.Diagnostic = oss.str();
        return out;
    }

    if (sc >= 500 && sc < 600) {
        out.Kind = JiraReachabilityProbeKind::ServiceUnavailable;
        std::ostringstream oss;
        oss << "HTTP " << sc;
        out.Diagnostic = oss.str();
        return out;
    }

    if (sc > 0 && sc < 500) {
        out.Kind = JiraReachabilityProbeKind::ReachableAuthOrConfigError;
        std::ostringstream oss;
        oss << "HTTP " << sc;
        out.Diagnostic = oss.str();
        return out;
    }

    std::string composed = std::string("HTTP ") + std::to_string(static_cast<int>(sc));
    if (!resp.error.message.empty()) {
        composed += " ";
        composed += resp.error.message;
    }
    if (IsJiraTransportErrorText(composed)) {
        out.Kind = JiraReachabilityProbeKind::TransportDown;
    } else {
        out.Kind = JiraReachabilityProbeKind::ReachableAuthOrConfigError;
    }
    out.Diagnostic = composed;
    return out;
}

std::string JiraClient::BuildBrowseUrl(const JiraConfig& cfg, const std::string& issueKey) const {
    if (cfg.Domain.empty() || issueKey.empty()) {
        return std::string();
    }
    return NormalizeBaseUrl(cfg.Domain) + "/browse/" + issueKey;
}
