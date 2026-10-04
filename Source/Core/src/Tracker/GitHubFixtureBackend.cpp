#include "GitHubFixtureBackend.h"

#include "GitHubIssueSearchMapping.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace smatchet {
namespace github {

GitHubFixtureBackend::GitHubFixtureBackend(const std::string& fixturePath, const std::string& ownerHint,
                                           const std::string& repoHint, bool includePullRequests)
    : smatchet::tracker_fixture::TrackerFixtureBackendBase(fixturePath), ownerHint_(ownerHint), repoHint_(repoHint),
      includePullRequests_(includePullRequests) {
    // The shared loader caps the read and the parse: the fixture path is env-var-selectable
    // (SMATCHET_TEST_GITHUB_BACKEND_FIXTURE), so nothing is trusted about wherever it points.
    nlohmann::json parsed;
    if (!LoadFixtureJson(parsed)) {
        LOG_ERROR("GitHubFixtureBackend: %s", loadError_.c_str());
        return;
    }
    if (!parsed.is_object()) {
        loadError_ = std::string("Fixture JSON root is not an object: ") + fixturePath_;
        LOG_ERROR("GitHubFixtureBackend: %s", loadError_.c_str());
        return;
    }
    // Accept two shapes:
    //   - Full GraphQL response: { "data": { "search": { "nodes": [...] } } }
    //   - Minimal: { "nodes": [...] }
    const nlohmann::json* nodesPtr = nullptr;
    if (parsed.contains("data") && parsed["data"].is_object() && parsed["data"].contains("search") &&
        parsed["data"]["search"].is_object() && parsed["data"]["search"].contains("nodes")) {
        nodesPtr = &parsed["data"]["search"]["nodes"];
    } else if (parsed.contains("nodes")) {
        nodesPtr = &parsed["nodes"];
    }
    if (nodesPtr == nullptr || !nodesPtr->is_array()) {
        loadError_ = std::string("Fixture JSON missing 'nodes' array (expected either data.search.nodes "
                                 "or top-level nodes): ") +
                     fixturePath_;
        LOG_ERROR("GitHubFixtureBackend: %s", loadError_.c_str());
        return;
    }
    tickets_ = MapGraphQlNodesToTickets(*nodesPtr, ownerHint_, repoHint_, includePullRequests_);
    LOG_INFO("GitHubFixtureBackend: loaded %zu ticket(s) from %s (includePRs=%d)", tickets_.size(),
             fixturePath_.c_str(), includePullRequests_ ? 1 : 0);
}

TrackerReachabilityProbeResult GitHubFixtureBackend::ProbeReachability(const TrackerConfig& /*cfg*/) {
    TrackerReachabilityProbeResult r;
    if (!loadError_.empty()) {
        r.Kind = TrackerReachabilityProbeKind::ServiceUnavailable;
        r.Diagnostic = loadError_;
        return r;
    }
    r.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
    r.Diagnostic = std::string("fixture: ") + fixturePath_;
    return r;
}

TrackerError GitHubFixtureBackend::UpdateIssueFields(const std::string& issueId, const nlohmann::json& /*fields*/) {
    LOG_INFO("GitHubFixtureBackend::UpdateIssueFields no-op on fixture backend (issueId=%s)", issueId.c_str());
    return TrackerError::Ok();
}

TrackerError GitHubFixtureBackend::UpdateField(const std::string& issueId, const TrackerField& field,
                                               const std::vector<std::string>& /*values*/) {
    LOG_INFO("GitHubFixtureBackend::UpdateField no-op on fixture backend (issueId=%s field=%s)", issueId.c_str(),
             field.Id.c_str());
    return TrackerError::Ok();
}

Result<nlohmann::json, TrackerError> GitHubFixtureBackend::BuildFieldPayload(const TrackerField& /*field*/,
                                                                             const std::vector<std::string>& values) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& v : values) {
        arr.push_back(v);
    }
    nlohmann::json outPayload = nlohmann::json::object();
    outPayload["values"] = std::move(arr);
    return Result<nlohmann::json, TrackerError>::Ok(std::move(outPayload));
}

} // namespace github
} // namespace smatchet
