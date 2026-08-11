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

/// KNOWN GAP, deliberately preserved: `TrackerConfig::NewIssueInheritFieldIdsGitHub` exists and the
/// Preferences UI both loads and saves it ("New issue: inherit fields from last row (GitHub)"), but
/// no draft path has ever read it — GitHub falls through to the default list below, so that setting
/// is accepted, persisted and silently ignored. Behaviour is kept identical here rather than fixed
/// in passing, because changing what a GitHub draft inherits is a user-visible change that deserves
/// its own decision. Add the `kBackendGitHub` branch when that decision is made.
inline const std::vector<std::string>& NewIssueInheritFieldIdsFor(const TrackerConfig& cfg) {
    const int kind = BackendIndexFromType(cfg.TrackerType);
    if (kind == kBackendPlane) {
        return cfg.NewIssueInheritFieldIdsPlane;
    }
    if (kind == kBackendLinear) {
        return cfg.NewIssueInheritFieldIdsLinear;
    }
    return cfg.NewIssueInheritFieldIds;
}

} // namespace tracker
} // namespace smatchet
