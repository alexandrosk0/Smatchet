// Slice 2 of docs/design/archive/autonomous-debugging-no-creds.md — see
// PlaneFixtureBackend.h for the contract.

#include "PlaneFixtureBackend.h"

#include "ITrackerBackendFactory.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <utility>

namespace smatchet {
namespace plane {

namespace {

class PlaneFixtureBackendFactory : public ITrackerBackendFactory {
  public:
    explicit PlaneFixtureBackendFactory(std::string fixturePath) : fixturePath_(std::move(fixturePath)) {}

    std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType) override {
        if (!trackerType.empty() && trackerType != "Plane") {
            LOG_WARN("PlaneFixtureBackendFactory: requested type '%s' but serving 'Plane' fixture from %s",
                     trackerType.c_str(), fixturePath_.c_str());
        }
        auto backend = std::make_unique<PlaneFixtureBackend>(fixturePath_);
        if (!backend->LoadError().empty()) {
            LOG_ERROR("PlaneFixtureBackend: failed to load fixture '%s': %s", fixturePath_.c_str(),
                      backend->LoadError().c_str());
        } else {
            LOG_INFO("PlaneFixtureBackend: loaded fixture '%s'", fixturePath_.c_str());
        }
        return std::unique_ptr<ITrackerBackend>(backend.release());
    }

  private:
    std::string fixturePath_;
};

} // namespace

ITrackerIssueReader& PlaneFixtureBackend::Reader() { return *this; }
ITrackerConnectivity& PlaneFixtureBackend::Connectivity() { return *this; }
ITrackerFieldCatalog* PlaneFixtureBackend::FieldCatalog() { return nullptr; }
ITrackerIssueMutations* PlaneFixtureBackend::Mutations() { return this; }
ITrackerCollaboration* PlaneFixtureBackend::Collaboration() { return nullptr; }

PlaneFixtureBackend::PlaneFixtureBackend(const std::string& fixturePath) : fixturePath_(fixturePath) {
    std::ifstream in(fixturePath);
    if (!in.good()) {
        loadError_ = "cannot open fixture file";
        return;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(buf.str(), nullptr, false);
    if (j.is_discarded()) {
        loadError_ = "invalid JSON in fixture file";
        return;
    }

    std::string projectIdentifier;
    if (j.is_object() && j.contains("project_identifier") && j["project_identifier"].is_string()) {
        projectIdentifier = j["project_identifier"].get<std::string>();
    }

    std::vector<UserDisplayLookup> users;
    if (j.is_object() && j.contains("users") && j["users"].is_array()) {
        for (const auto& u : j["users"]) {
            if (!u.is_object())
                continue;
            UserDisplayLookup lookup;
            if (u.contains("account_id") && u["account_id"].is_string()) {
                lookup.AccountId = u["account_id"].get<std::string>();
            }
            if (u.contains("display_name") && u["display_name"].is_string()) {
                lookup.DisplayName = u["display_name"].get<std::string>();
            }
            users.push_back(std::move(lookup));
        }
    }

    nlohmann::json results = nlohmann::json::array();
    if (j.is_object() && j.contains("list_response") && j["list_response"].is_object()) {
        const auto& lr = j["list_response"];
        if (lr.contains("results")) {
            results = lr["results"];
        }
    }

    tickets_ = MapPlaneWorkItemsArrayToCachedTickets(results, projectIdentifier, users, nullptr);
}

TrackerReachabilityProbeResult PlaneFixtureBackend::ProbeReachability(const TrackerConfig& /*cfg*/) {
    TrackerReachabilityProbeResult out;
    out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
    out.Diagnostic = "PlaneFixtureBackend (no network)";
    return out;
}

std::vector<CachedTicket> PlaneFixtureBackend::FetchIssues(bool* outFullSyncCompleted,
                                                           const TrackerConfig* /*configOverride*/,
                                                           const ViewsStore* /*viewsOverride*/,
                                                           std::string* outFetchError, std::string* outWarning) {
    if (outFullSyncCompleted)
        *outFullSyncCompleted = loadError_.empty();
    if (outFetchError)
        *outFetchError = loadError_;
    if (outWarning)
        outWarning->clear();
    return tickets_;
}

bool PlaneFixtureBackend::FetchIssuesForKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& issueKeys,
                                             const ViewsStore& /*views*/, std::vector<CachedTicket>& outTickets,
                                             std::string& outError) {
    outTickets.clear();
    if (!loadError_.empty()) {
        outError = loadError_;
        return false;
    }
    outError.clear();
    for (const auto& key : issueKeys) {
        for (const auto& t : tickets_) {
            if (t.id == key) {
                outTickets.push_back(t);
                break;
            }
        }
    }
    return true;
}

bool PlaneFixtureBackend::UpdateIssueFields(const std::string& /*issueId*/, const nlohmann::json& /*fields*/,
                                            std::string& outError) {
    outError = "PlaneFixtureBackend is read-only";
    return false;
}

bool PlaneFixtureBackend::UpdateField(const std::string& /*issueId*/, const TrackerField& /*field*/,
                                      const std::vector<std::string>& /*values*/, std::string& outError) {
    outError = "PlaneFixtureBackend is read-only";
    return false;
}

bool PlaneFixtureBackend::BuildFieldPayload(const TrackerField& /*field*/, const std::vector<std::string>& /*values*/,
                                            nlohmann::json& outPayload, std::string& /*outError*/) {
    outPayload = nlohmann::json::object();
    return true;
}

std::unique_ptr<ITrackerBackendFactory> MakePlaneFixtureBackendFactory(const std::string& fixturePath) {
    return std::make_unique<PlaneFixtureBackendFactory>(fixturePath);
}

} // namespace plane
} // namespace smatchet
