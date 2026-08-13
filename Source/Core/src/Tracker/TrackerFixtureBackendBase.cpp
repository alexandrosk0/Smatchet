// Shared read path for the fixture backends — see TrackerFixtureBackendBase.h for the
// contract and for what deliberately stays per-backend.

#include "Tracker/TrackerFixtureBackendBase.h"

#include "Json/BoundedJsonParse.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace smatchet {
namespace tracker_fixture {

TrackerFixtureBackendBase::TrackerFixtureBackendBase(std::string fixturePath) : fixturePath_(std::move(fixturePath)) {}

bool TrackerFixtureBackendBase::LoadFixtureJson(nlohmann::json& out) {
    std::ifstream in(fixturePath_);
    if (!in.good()) {
        loadError_ = "cannot open fixture file";
        return false;
    }
    std::stringstream buf;
    // SMATCHET_DEVIATION(rule=unbounded-file-slurp; reason=developer-authored local fixture file, small by
    // construction; owner=security-audit; revisit=2026-12-31)
    buf << in.rdbuf();
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(buf.str());
    if (parsed.is_discarded()) {
        loadError_ = "invalid JSON in fixture file";
        return false;
    }
    out = std::move(parsed);
    return true;
}

ITrackerIssueReader& TrackerFixtureBackendBase::Reader() { return *this; }
ITrackerConnectivity& TrackerFixtureBackendBase::Connectivity() { return *this; }
ITrackerFieldCatalog* TrackerFixtureBackendBase::FieldCatalog() { return nullptr; }
ITrackerIssueMutations* TrackerFixtureBackendBase::Mutations() { return this; }
ITrackerCollaboration* TrackerFixtureBackendBase::Collaboration() { return nullptr; }
ITrackerActivity* TrackerFixtureBackendBase::Activity() { return nullptr; }

std::vector<CachedTicket> TrackerFixtureBackendBase::FetchIssues(bool* outFullSyncCompleted,
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
        // Same classification FetchIssuesForKeys applies to loadError_.
        *outFetchErrorStructured = loadError_.empty() ? TrackerError::Ok() : TrackerErrorInvalidRequest(loadError_);
    }
    if (outWarning) {
        outWarning->clear();
    }
    return tickets_;
}

Result<std::vector<CachedTicket>, TrackerError>
TrackerFixtureBackendBase::FetchIssuesForKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& issueKeys,
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

TrackerError TrackerFixtureBackendBase::UpdateIssueFields(const std::string& /*issueId*/,
                                                          const nlohmann::json& /*fields*/) {
    return TrackerErrorInvalidRequest(GetTrackerType() + "FixtureBackend is read-only");
}

TrackerError TrackerFixtureBackendBase::UpdateField(const std::string& /*issueId*/, const TrackerField& /*field*/,
                                                    const std::vector<std::string>& /*values*/) {
    return TrackerErrorInvalidRequest(GetTrackerType() + "FixtureBackend is read-only");
}

Result<nlohmann::json, TrackerError>
TrackerFixtureBackendBase::BuildFieldPayload(const TrackerField& /*field*/,
                                             const std::vector<std::string>& /*values*/) {
    return Result<nlohmann::json, TrackerError>::Ok(nlohmann::json::object());
}

std::string TrackerFixtureBackendBase::ResolveDisplayValue(const std::string& /*fieldId*/,
                                                           const TrackerField* /*field*/,
                                                           const std::string& value) const {
    return value;
}

} // namespace tracker_fixture
} // namespace smatchet
