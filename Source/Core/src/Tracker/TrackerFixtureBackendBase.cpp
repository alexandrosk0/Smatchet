// Shared read path for the fixture backends — see TrackerFixtureBackendBase.h for the
// contract and for what deliberately stays per-backend.

#include "Tracker/TrackerFixtureBackendBase.h"

#include "ITrackerBackendFactory.h"
#include "Json/BoundedJsonParse.h"
#include "Logger.h"

#include <cstdint>
#include <fstream>
#include <utility>

namespace smatchet {
namespace tracker_fixture {

namespace {

class FixtureBackendFactory : public ITrackerBackendFactory {
  public:
    FixtureBackendFactory(std::string fixturePath, std::string backendName, FixtureBackendMaker make)
        : fixturePath_(std::move(fixturePath)), backendName_(std::move(backendName)), make_(std::move(make)) {}

    std::unique_ptr<ITrackerBackend> Create(const std::string& trackerType, const TrackerConfig& /*cfg*/) override {
        if (!trackerType.empty() && trackerType != backendName_) {
            LOG_WARN("%sFixtureBackendFactory: requested type '%s' but serving '%s' fixture from %s",
                     backendName_.c_str(), trackerType.c_str(), backendName_.c_str(), fixturePath_.c_str());
        }
        std::unique_ptr<TrackerFixtureBackendBase> backend = make_ ? make_(fixturePath_) : nullptr;
        if (!backend) {
            LOG_ERROR("%sFixtureBackendFactory: no backend built for fixture '%s'", backendName_.c_str(),
                      fixturePath_.c_str());
            return nullptr;
        }
        if (!backend->LoadError().empty()) {
            LOG_ERROR("%sFixtureBackend: failed to load fixture '%s': %s", backendName_.c_str(), fixturePath_.c_str(),
                      backend->LoadError().c_str());
        } else {
            LOG_INFO("%sFixtureBackend: loaded fixture '%s'", backendName_.c_str(), fixturePath_.c_str());
        }
        return std::unique_ptr<ITrackerBackend>(std::move(backend));
    }

  private:
    std::string fixturePath_;
    std::string backendName_;
    FixtureBackendMaker make_;
};

} // namespace

std::unique_ptr<ITrackerBackendFactory> MakeFixtureBackendFactory(std::string fixturePath, std::string backendName,
                                                                  FixtureBackendMaker make) {
    return std::make_unique<FixtureBackendFactory>(std::move(fixturePath), std::move(backendName), std::move(make));
}

TrackerFixtureBackendBase::TrackerFixtureBackendBase(std::string fixturePath) : fixturePath_(std::move(fixturePath)) {}

bool TrackerFixtureBackendBase::LoadFixtureJson(nlohmann::json& out) {
    // Binary, so the size from tellg is exactly what read() returns on every platform.
    std::ifstream in(fixturePath_, std::ios::binary);
    if (!in.is_open()) {
        loadError_ = "cannot open fixture file: " + fixturePath_;
        return false;
    }
    // Stop at the parse's byte cap: reading a larger file whole would only be thrown away.
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > smatchet::json_safe::kDefaultMaxBytes) {
        loadError_ = "fixture file too large or unreadable: " + fixturePath_;
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size > 0 && !in.read(&bytes[0], size)) {
        loadError_ = "cannot read fixture file: " + fixturePath_;
        return false;
    }
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(bytes);
    if (parsed.is_discarded()) {
        loadError_ = "invalid JSON in fixture file: " + fixturePath_;
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
