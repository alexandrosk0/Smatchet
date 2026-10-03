// Slice 4 of docs/plans/linear-tracker-backend.md — see
// LinearFixtureBackend.h for the contract.

#include "LinearFixtureBackend.h"

#include "ITrackerBackendFactory.h"
#include "LinearIssueMappingPure.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace smatchet {
namespace linear {

namespace {

// Locate the issue `nodes` array inside a parsed fixture. Accepts the live
// GraphQL response shape (`data.issues.nodes`), the text-search variant
// (`data.issueSearch.nodes`), an un-enveloped `issues.nodes`, or a minimal
// top-level `nodes`. Returns nullptr when none is present.
const nlohmann::json* FindNodesArray(const nlohmann::json& root) {
    auto nodesUnder = [](const nlohmann::json& obj, const char* key) -> const nlohmann::json* {
        if (obj.is_object() && obj.contains(key) && obj[key].is_object() && obj[key].contains("nodes") &&
            obj[key]["nodes"].is_array()) {
            return &obj[key]["nodes"];
        }
        return nullptr;
    };
    if (root.is_object() && root.contains("data") && root["data"].is_object()) {
        const nlohmann::json& data = root["data"];
        if (const nlohmann::json* n = nodesUnder(data, "issues")) {
            return n;
        }
        if (const nlohmann::json* n = nodesUnder(data, "issueSearch")) {
            return n;
        }
    }
    if (const nlohmann::json* n = nodesUnder(root, "issues")) {
        return n;
    }
    if (root.is_object() && root.contains("nodes") && root["nodes"].is_array()) {
        return &root["nodes"];
    }
    return nullptr;
}

} // namespace

LinearFixtureBackend::LinearFixtureBackend(const std::string& fixturePath)
    : smatchet::tracker_fixture::TrackerFixtureBackendBase(fixturePath) {
    nlohmann::json j;
    if (!LoadFixtureJson(j)) {
        return;
    }
    const nlohmann::json* nodes = FindNodesArray(j);
    if (nodes == nullptr) {
        loadError_ = "fixture missing issue nodes array (expected data.issues.nodes or top-level nodes)";
        return;
    }
    tickets_ = MapLinearIssueNodesToTickets(*nodes);
}

TrackerReachabilityProbeResult LinearFixtureBackend::ProbeReachability(const TrackerConfig& /*cfg*/) {
    TrackerReachabilityProbeResult out;
    if (!loadError_.empty()) {
        out.Kind = TrackerReachabilityProbeKind::ServiceUnavailable;
        out.Diagnostic = loadError_;
        return out;
    }
    out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
    out.Diagnostic = "LinearFixtureBackend (no network)";
    return out;
}

std::unique_ptr<ITrackerBackendFactory> MakeLinearFixtureBackendFactory(const std::string& fixturePath) {
    return tracker_fixture::MakeFixtureBackendFactoryFor<LinearFixtureBackend>(fixturePath, "Linear");
}

} // namespace linear
} // namespace smatchet
