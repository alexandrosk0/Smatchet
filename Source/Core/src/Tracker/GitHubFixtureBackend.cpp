#include "GitHubFixtureBackend.h"

#include "GitHubIssueSearchMapping.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>

namespace smatchet {
namespace github {

namespace {

// Read the full file at `path` into `out`. Returns true on success; on failure
// `out` carries a diagnostic.
bool ReadFileToString(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        out = std::string("Failed to open fixture file: ") + path;
        return false;
    }
    std::ostringstream ss;
    // SMATCHET_DEVIATION(rule=unbounded-file-slurp; reason=developer-authored local fixture file, small by construction; owner=security-audit; revisit=2026-12-31)
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

} // namespace

ITrackerIssueReader& GitHubFixtureBackend::Reader() { return *this; }
ITrackerConnectivity& GitHubFixtureBackend::Connectivity() { return *this; }
ITrackerFieldCatalog* GitHubFixtureBackend::FieldCatalog() { return nullptr; }
ITrackerIssueMutations* GitHubFixtureBackend::Mutations() { return this; }
ITrackerCollaboration* GitHubFixtureBackend::Collaboration() { return nullptr; }
ITrackerActivity* GitHubFixtureBackend::Activity() { return nullptr; }

GitHubFixtureBackend::GitHubFixtureBackend(const std::string& fixturePath, const std::string& ownerHint,
                                           const std::string& repoHint, bool includePullRequests)
    : fixturePath_(fixturePath), ownerHint_(ownerHint), repoHint_(repoHint), includePullRequests_(includePullRequests) {
    std::string contents;
    if (!ReadFileToString(fixturePath_, contents)) {
        loadError_ = contents;
        LOG_ERROR("GitHubFixtureBackend: %s", loadError_.c_str());
        return;
    }
    // Bounded parse: the fixture path is env-var-selectable (SMATCHET_TEST_GITHUB_BACKEND_FIXTURE),
    // so cap depth/nodes/bytes rather than trust wherever it points.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(contents);
    if (parsed.is_discarded() || !parsed.is_object()) {
        loadError_ = std::string("Fixture JSON parse failed or root is not an object: ") + fixturePath_;
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

std::vector<CachedTicket> GitHubFixtureBackend::FetchIssues(bool* outFullSyncCompleted,
                                                            const TrackerConfig* /*configOverride*/,
                                                            const ViewsStore* /*viewsOverride*/,
                                                            std::string* outFetchError, std::string* outWarning,
                                                            TrackerError* outFetchErrorStructured) {
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = loadError_.empty();
    }
    if (outFetchError) {
        *outFetchError = loadError_;
    }
    if (outFetchErrorStructured) {
        // Same classification this fixture's FetchIssuesForKeys applies to loadError_.
        *outFetchErrorStructured = loadError_.empty() ? TrackerError::Ok() : TrackerErrorInvalidRequest(loadError_);
    }
    if (outWarning) {
        outWarning->clear();
    }
    return tickets_;
}

Result<std::vector<CachedTicket>, TrackerError>
GitHubFixtureBackend::FetchIssuesForKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& issueKeys,
                                         const ViewsStore& /*views*/) {
    if (!loadError_.empty()) {
        return Result<std::vector<CachedTicket>, TrackerError>::Err(TrackerErrorInvalidRequest(loadError_));
    }
    std::vector<CachedTicket> outTickets;
    for (const auto& key : issueKeys) {
        for (const auto& t : tickets_) {
            if (t.id == key) {
                outTickets.push_back(t);
                break;
            }
        }
    }
    return Result<std::vector<CachedTicket>, TrackerError>::Ok(std::move(outTickets));
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

std::string GitHubFixtureBackend::ResolveDisplayValue(const std::string& /*fieldId*/, const TrackerField* /*field*/,
                                                      const std::string& value) const {
    return value;
}

} // namespace github
} // namespace smatchet
