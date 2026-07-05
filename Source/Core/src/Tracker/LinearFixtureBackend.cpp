// Slice 4 of docs/plans/linear-tracker-backend.md — see
// LinearFixtureBackend.h for the contract.

#include "LinearFixtureBackend.h"

#include "ITrackerBackendFactory.h"
#include "Json/BoundedJsonParse.h"
#include "LinearIssueMappingPure.h"
#include "Logger.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
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

// SMATCHET_DEVIATION(rule=duplication; reason=per-backend fixture-factory boilerplate deliberately mirrors the Plane sibling — the standing per-backend *Client/fixture exemption class; folding them would couple independent tracker backends; owner=security-audit; revisit=2026-12-31)
class LinearFixtureBackendFactory : public ITrackerBackendFactory {
  public:
    explicit LinearFixtureBackendFactory(std::string fixturePath) : fixturePath_(std::move(fixturePath)) {}

    std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType, const TrackerConfig& /*cfg*/) override {
        if (!trackerType.empty() && trackerType != "Linear") {
            LOG_WARN("LinearFixtureBackendFactory: requested type '%s' but serving 'Linear' fixture from %s",
                     trackerType.c_str(), fixturePath_.c_str());
        }
        auto backend = std::make_unique<LinearFixtureBackend>(fixturePath_);
        if (!backend->LoadError().empty()) {
            LOG_ERROR("LinearFixtureBackend: failed to load fixture '%s': %s", fixturePath_.c_str(),
                      backend->LoadError().c_str());
        } else {
            LOG_INFO("LinearFixtureBackend: loaded fixture '%s'", fixturePath_.c_str());
        }
        return std::unique_ptr<ITrackerBackend>(backend.release());
    }

  private:
    std::string fixturePath_;
};

} // namespace

ITrackerIssueReader& LinearFixtureBackend::Reader() { return *this; }
ITrackerConnectivity& LinearFixtureBackend::Connectivity() { return *this; }
ITrackerFieldCatalog* LinearFixtureBackend::FieldCatalog() { return nullptr; }
ITrackerIssueMutations* LinearFixtureBackend::Mutations() { return this; }
ITrackerCollaboration* LinearFixtureBackend::Collaboration() { return nullptr; }
ITrackerActivity* LinearFixtureBackend::Activity() { return nullptr; }

LinearFixtureBackend::LinearFixtureBackend(const std::string& fixturePath) : fixturePath_(fixturePath) {
    std::ifstream in(fixturePath);
    if (!in.good()) {
        loadError_ = "cannot open fixture file";
        return;
    }
    std::stringstream buf;
    // SMATCHET_DEVIATION(rule=unbounded-file-slurp; reason=developer-authored local fixture file, small by construction; owner=security-audit; revisit=2026-12-31)
    buf << in.rdbuf();
    // Bounded parse: the fixture path is env-var-selectable (SMATCHET_TEST_LINEAR_BACKEND_FIXTURE),
    // so cap depth/nodes/bytes rather than trust wherever it points.
    const nlohmann::json j = smatchet::json_safe::ParseBoundedOrDiscarded(buf.str());
    if (j.is_discarded()) {
        loadError_ = "invalid JSON in fixture file";
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

std::vector<CachedTicket> LinearFixtureBackend::FetchIssues(bool* outFullSyncCompleted,
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

Result<std::vector<CachedTicket>, TrackerError>
LinearFixtureBackend::FetchIssuesForKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& issueKeys,
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

TrackerError LinearFixtureBackend::UpdateIssueFields(const std::string& /*issueId*/, const nlohmann::json& /*fields*/) {
    return TrackerErrorInvalidRequest("LinearFixtureBackend is read-only");
}

TrackerError LinearFixtureBackend::UpdateField(const std::string& /*issueId*/, const TrackerField& /*field*/,
                                               const std::vector<std::string>& /*values*/) {
    return TrackerErrorInvalidRequest("LinearFixtureBackend is read-only");
}

Result<nlohmann::json, TrackerError>
LinearFixtureBackend::BuildFieldPayload(const TrackerField& /*field*/, const std::vector<std::string>& /*values*/) {
    return Result<nlohmann::json, TrackerError>::Ok(nlohmann::json::object());
}

std::unique_ptr<ITrackerBackendFactory> MakeLinearFixtureBackendFactory(const std::string& fixturePath) {
    return std::make_unique<LinearFixtureBackendFactory>(fixturePath);
}

} // namespace linear
} // namespace smatchet
