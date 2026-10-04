// Slice 2 of docs/plans/shipped/autonomous-debugging-no-creds.md — see
// PlaneFixtureBackend.h for the contract.

#include "PlaneFixtureBackend.h"

#include "ITrackerBackendFactory.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace smatchet {
namespace plane {

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
    return tracker_fixture::MakeFixtureBackendFactoryFor<PlaneFixtureBackend>(fixturePath, "Plane");
}

} // namespace plane
} // namespace smatchet
