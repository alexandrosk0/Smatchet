#pragma once

#include "StringUtil.h"

#include <string>

// Tracker-type string -> stable backend ordinal, case-insensitively.
//
// The persisted `TrackerType` is hand-editable (`smatchet_config.json`), and
// DefaultTrackerBackendFactory has always resolved it case-insensitively via
// ToLowerAsciiCopy. The Preferences UI carried its own copy of the mapping that
// listed only two spellings per backend ("GitHub" || "github"), so "Github" fell
// through to the Jira default: the factory built a GitHubClient while the UI drew
// the Jira credential rows, and the user edited Jira fields against a GitHub
// backend. One shared rule, so the two cannot disagree again.
//
// Ordinals are the Preferences combo's item order and are persisted nowhere.

namespace smatchet {
namespace tracker {

enum BackendOrdinal { kBackendJira = 0, kBackendPlane = 1, kBackendGitHub = 2, kBackendLinear = 3 };

/// Jira (0) for an empty, unknown or malformed value — matching the factory's
/// documented fallback, which keeps stale configs booting.
inline int BackendIndexFromType(const std::string& trackerType) {
    const std::string lower = ToLowerAsciiCopy(trackerType);
    if (lower == "plane") {
        return kBackendPlane;
    }
    if (lower == "github") {
        return kBackendGitHub;
    }
    if (lower == "linear") {
        return kBackendLinear;
    }
    return kBackendJira;
}

} // namespace tracker
} // namespace smatchet
