// Slice 2 of docs/plans/shipped/autonomous-debugging-no-creds.md — see
// PlaneFixtureBackend.h for the contract.

#include "PlaneFixtureBackend.h"

#include "ITrackerBackendFactory.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace smatchet {
namespace plane {

namespace {

class PlaneFixtureBackendFactory : public ITrackerBackendFactory {
  public:
    explicit PlaneFixtureBackendFactory(std::string fixturePath) : fixturePath_(std::move(fixturePath)) {}

    std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType, const TrackerConfig& /*cfg*/) override {
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

PlaneFixtureBackend::PlaneFixtureBackend(const std::string& fixturePath)
    : smatchet::tracker_fixture::TrackerFixtureBackendBase(fixturePath) {
    nlohmann::json j;
    if (!LoadFixtureJson(j)) {
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

std::unique_ptr<ITrackerBackendFactory> MakePlaneFixtureBackendFactory(const std::string& fixturePath) {
    return std::make_unique<PlaneFixtureBackendFactory>(fixturePath);
}

} // namespace plane
} // namespace smatchet
