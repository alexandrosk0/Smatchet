#pragma once

// Implementation-only header shared between the four PlaneClient TUs
// (PlaneClient.cpp + PlaneIssueSearch.cpp + PlaneIssueMutation.cpp + PlaneFieldCatalog.cpp).
// Not for inclusion outside Source_Core/src/ — public surface stays in PlaneClient.h.

#include "ConfigManager.h"

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

#include <string>

namespace smatchet {
namespace plane_detail {

/// Trim leading + trailing ASCII whitespace in place.
void TrimAsciiWs(std::string& s);

/// Split URL authority into host (lowercase) and port suffix e.g. ":443" (may be empty).
void ParseHttpAuthority(const std::string& base, size_t hostStart, size_t hostEnd, std::string& outHostLower,
                        std::string& outPortSuffix);

/// Cloud Plane SPA is app.plane.so; REST is api.plane.so. Any path on app URL is UI, not API — strip to origin.
/// Host:port supported (e.g. app.plane.so:443). Self-hosted URLs pass through unchanged (except trim).
std::string NormalizePlaneApiBase(std::string base);

/// Safely extract a JSON field as a string regardless of its actual type (int, bool, null, etc.).
std::string JsonFieldToString(const nlohmann::json& obj, const char* key);

/// Resolve a project key/identifier/name to its UUID + identifier via the Plane workspace projects endpoint.
/// Returns true on success and writes `outId` / `outIdentifier`. On failure writes `*outError` (if non-null).
bool ResolvePlaneProject(const std::string& planeApi, const TrackerConfig& cfg, const std::string& projectKey,
                         const cpr::Header& headers, std::string& outId, std::string& outIdentifier,
                         std::string* outError);

} // namespace plane_detail
} // namespace smatchet
