#include "JiraClient.h"

#include "Json/BoundedJsonParse.h"
#include "JqlProjectScope.h"
#include "Logger.h"
#include "TrackerHttpClient.h"
#include "TrackerHttpUtils.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

ITrackerIssueReader& JiraClient::Reader() { return *this; }
ITrackerConnectivity& JiraClient::Connectivity() { return *this; }
ITrackerFieldCatalog* JiraClient::FieldCatalog() { return this; }
ITrackerIssueMutations* JiraClient::Mutations() { return this; }
ITrackerCollaboration* JiraClient::Collaboration() { return this; }
ITrackerActivity* JiraClient::Activity() { return this; }

std::string JiraClient::ExtractProjectFromQuery(const std::string& query) const {
    return JqlProjectScope::ExtractSingleProject(query);
}

TrackerReachabilityProbeResult JiraClient::ProbeReachability(const TrackerConfig& cfg) {
    TrackerReachabilityProbeResult out;
    std::string authErr;
    if (!EnsureTrackerAuthConfig(cfg, authErr)) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = authErr.empty() ? std::string("Missing Tracker domain or API token.") : authErr;
        return out;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string url = base + "/rest/api/3/myself";
    const cpr::Header headers = BuildTrackerHeaders(cfg, false);
    const cpr::Response resp =
        TrackerGetLogged("JiraClient", url, headers, kTrackerProbeConnectTimeoutMs, kTrackerProbeOverallTimeoutMs);

    // BACKLOG_CODE_REVIEW.md §B2 (item 15): route through the shared reachability-probe classifier
    // (also used by PlaneClient) so the HTTP-status → probe-kind matrix lives in one place instead
    // of a per-backend hand-rolled ladder. Behaviour-preserving for every common status — pinned by
    // tests/Core/JiraClientHttp.test.cpp.
    return ClassifyReachabilityProbe(resp);
}

namespace {

constexpr std::int64_t kJiraListProjectsTtlSeconds = 300; // 5 minutes
constexpr size_t kJiraListProjectsCap = 200;

std::int64_t NowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

void JiraClient::InvalidateListProjectsCache() {
    std::lock_guard<std::mutex> lock(listProjectsMutex_);
    cachedProjects_.clear();
    cachedProjectsAtUnix_ = 0;
}

std::vector<RemoteProject> JiraClient::ListProjects() {
    // Fast path: serve from cache when still warm.
    {
        std::lock_guard<std::mutex> lock(listProjectsMutex_);
        const std::int64_t now = NowUnixSeconds();
        if (!cachedProjects_.empty() && (now - cachedProjectsAtUnix_) < kJiraListProjectsTtlSeconds) {
            return cachedProjects_;
        }
    }

    // Need credentials + base URL — pull current config snapshot.
    const TrackerConfig cfg = ConfigManager::Load();
    std::string authErr;
    if (!EnsureTrackerAuthConfig(cfg, authErr)) {
        LOG_WARN("JiraClient::ListProjects: auth/config missing: %s", authErr.c_str());
        return {};
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string url = base + "/rest/api/3/project";
    const cpr::Header headers = BuildTrackerHeaders(cfg);

    const cpr::Response resp = TrackerGetLogged("JiraClient", url, headers);
    if (resp.status_code != 200) {
        LOG_WARN("JiraClient::ListProjects: HTTP %ld on %s", resp.status_code, url.c_str());
        return {};
    }

    std::vector<RemoteProject> projects;
    try {
        // Bounded parse — the try can't catch a depth-bomb's destructor-time SIGSEGV
        // (audit: unbounded-recursion-DoS). Discarded on failure → !is_array() path below.
        const nlohmann::json j = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
        if (!j.is_array()) {
            LOG_WARN("JiraClient::ListProjects: unexpected response shape (not an array).");
            return {};
        }
        projects.reserve(j.size() < kJiraListProjectsCap ? j.size() : kJiraListProjectsCap);
        for (const auto& p : j) {
            if (!p.is_object()) {
                continue;
            }
            RemoteProject rp;
            if (p.contains("id") && p["id"].is_string()) {
                rp.id = p["id"].get<std::string>();
            }
            if (p.contains("key") && p["key"].is_string()) {
                rp.key = p["key"].get<std::string>();
            }
            if (p.contains("name") && p["name"].is_string()) {
                rp.displayName = p["name"].get<std::string>();
            }
            // Skip entries that have neither key nor id — nothing actionable.
            if (rp.id.empty() && rp.key.empty()) {
                continue;
            }
            projects.push_back(std::move(rp));
            if (projects.size() >= kJiraListProjectsCap) {
                LOG_INFO("JiraClient::ListProjects: capping at 200 entries");
                break;
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient::ListProjects: parse error: %s", ex.what());
        return {};
    } catch (...) {
        LOG_WARN("JiraClient::ListProjects: parse error (unknown)");
        return {};
    }

    {
        std::lock_guard<std::mutex> lock(listProjectsMutex_);
        cachedProjects_ = projects;
        cachedProjectsAtUnix_ = NowUnixSeconds();
    }
    return projects;
}

std::string JiraClient::BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const {
    if (cfg.Domain.empty() || issueKey.empty()) {
        return std::string();
    }
    return NormalizeBaseUrl(cfg.Domain) + "/browse/" + issueKey;
}
