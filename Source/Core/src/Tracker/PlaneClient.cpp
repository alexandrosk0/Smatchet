#include "PlaneClient.h"
#include "PlaneClient_Internal.h"

#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "StringUtil.h"
#include "TrackerHttpClient.h"
#include "TrackerHttpUtils.h"

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace smatchet {
namespace plane_detail {

void TrimAsciiWs(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.erase(0, 1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.pop_back();
    }
}

void ParseHttpAuthority(const std::string& base, size_t hostStart, size_t hostEnd, std::string& outHostLower,
                        std::string& outPortSuffix) {
    const std::string auth = base.substr(hostStart, hostEnd - hostStart);
    outPortSuffix.clear();
    outHostLower = auth;
    const size_t colon = auth.find(':');
    if (colon != std::string::npos) {
        outHostLower = auth.substr(0, colon);
        outPortSuffix = auth.substr(colon);
    }
    std::transform(outHostLower.begin(), outHostLower.end(), outHostLower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
}

std::string NormalizePlaneApiBase(std::string base) {
    TrimAsciiWs(base);
    while (!base.empty() && (base.back() == '/' || base.back() == '\\')) {
        base.pop_back();
    }
    const auto scheme = base.find("://");
    if (scheme == std::string::npos) {
        return base;
    }
    const size_t hostStart = scheme + 3;
    size_t hostEnd = base.find('/', hostStart);
    if (hostEnd == std::string::npos) {
        hostEnd = base.size();
    }
    std::string hostLower;
    std::string portSuffix;
    ParseHttpAuthority(base, hostStart, hostEnd, hostLower, portSuffix);
    const std::string schemePrefix = base.substr(0, hostStart); // "https://"
    if (hostLower == "app.plane.so") {
        return schemePrefix + std::string("api.plane.so") + portSuffix;
    }
    if (hostLower == "api.plane.so") {
        return schemePrefix + std::string("api.plane.so") + portSuffix;
    }
    return base;
}

std::string JsonFieldToString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null())
        return {};
    const auto& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<int64_t>());
    if (v.is_number_float())
        return std::to_string(v.get<double>());
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    return v.dump(); // fallback: JSON serialization
}

bool ResolvePlaneProject(const std::string& planeApi, const TrackerConfig& cfg, const std::string& projectKey,
                         const cpr::Header& headers, std::string& outId, std::string& outIdentifier,
                         std::string* outError) {
    const std::string projectsUrl = planeApi + "/api/v1/workspaces/" + cfg.PlaneWorkspaceSlug + "/projects/";
    cpr::Parameters params;
    params.Add({"per_page", "100"});
    auto response = TrackerGetLogged("PlaneClient", projectsUrl, headers, params);
    if (response.status_code != 200) {
        const std::string err = "Plane API error " + std::to_string(response.status_code) + " resolving project '" +
                                projectKey + "': " + response.text.substr(0, 300);
        if (outError)
            *outError = err;
        return false;
    }

    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    const nlohmann::json j = smatchet::json_safe::ParseBoundedOrDiscarded(StripUtf8BomCopy(response.text));
    if (j.is_discarded()) {
        const std::string err =
            "Plane project list returned invalid JSON while resolving project '" + projectKey + "'.";
        if (outError)
            *outError = err;
        return false;
    }

    const auto projects = (j.is_object() && j.contains("results")) ? j["results"] : j;
    if (!projects.is_array()) {
        const std::string err =
            "Plane project list response has no results array while resolving project '" + projectKey + "'.";
        if (outError)
            *outError = err;
        return false;
    }

    std::string available;
    for (const auto& p : projects) {
        const std::string id = JsonFieldToString(p, "id");
        const std::string identifier = JsonFieldToString(p, "identifier");
        const std::string name = JsonFieldToString(p, "name");
        if (id == projectKey || identifier == projectKey || name == projectKey) {
            outId = id;
            outIdentifier = identifier;
            return true;
        }
        if (available.size() < 220) {
            if (!available.empty())
                available += ", ";
            available += identifier.empty() ? name : identifier;
        }
    }

    const std::string err = "Plane project '" + projectKey + "' was not found in workspace '" + cfg.PlaneWorkspaceSlug +
                            "'. Available project identifiers/names: " + available;
    if (outError)
        *outError = err;
    return false;
}

} // namespace plane_detail
} // namespace smatchet

ITrackerIssueReader& PlaneClient::Reader() { return *this; }
ITrackerConnectivity& PlaneClient::Connectivity() { return *this; }
ITrackerFieldCatalog* PlaneClient::FieldCatalog() { return this; }
ITrackerIssueMutations* PlaneClient::Mutations() { return this; }
ITrackerCollaboration* PlaneClient::Collaboration() { return this; }
ITrackerActivity* PlaneClient::Activity() { return this; }

PlaneClient::PlaneClient() : planeProjectId_(""), planeProjectIdentifier_("") {}

PlaneClient::~PlaneClient() {}

std::unordered_map<std::string, std::string> PlaneClient::BuildPlaneHeaders(const TrackerConfig& cfg) {
    return {{"Accept", "application/json"}, {"Content-Type", "application/json"}, {"x-api-key", cfg.PlaneApiKey}};
}
