#pragma once

#include "Config/ConfigManager.h"
#include "Tracker/TrackerBackendKind.h"

#include <string>
#include <vector>

// Which per-backend "inherit fields from the last row" list a new-issue draft copies.
//
// Three surfaces build that draft — the grid header's "+ New Issue", the inactive-row cell, and the
// offline create path — and each carried its own copy of the same ternary chain. Each also compared
// the hand-editable `TrackerType` exactly, so a config saying "plane" inherited the JIRA list. One
// rule, classified once, through the same case-insensitive mapping the backend factory uses.

namespace smatchet {
namespace tracker {

/// GitHub reads its own list like every other backend. It used to fall through to the Jira list,
/// which made "New issue: inherit fields from last row (GitHub)" a setting the Preferences UI
/// loaded and saved but no draft path consumed.
inline const std::vector<std::string>& NewIssueInheritFieldIdsFor(const TrackerConfig& cfg) {
    const int kind = BackendIndexFromType(cfg.TrackerType);
    if (kind == kBackendPlane) {
        return cfg.NewIssueInheritFieldIdsPlane;
    }
    if (kind == kBackendGitHub) {
        return cfg.NewIssueInheritFieldIdsGitHub;
    }
    if (kind == kBackendLinear) {
        return cfg.NewIssueInheritFieldIdsLinear;
    }
    return cfg.NewIssueInheritFieldIds;
}

} // namespace tracker
} // namespace smatchet
