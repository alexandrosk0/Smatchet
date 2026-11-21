#pragma once

// PaneBackendTargetsPure — the rows of the grid header's "+ ▾" new-pane picker (#2444). Pure: no
// ImGui, no I/O; reads only the in-memory config, so bucket-A covers which rows show and whether the
// caret renders at all.
//
// One row per credentialed tracker kind, except Jira with two or more sites, which gets one row per
// site ("New Jira · <host> pane"). The Jira site is still one global setting (ActiveJiraDomain):
// picking another site's row switches it, the same path as the Views - Jira Domain combo, so every
// Jira pane follows. Side-by-side panes on two Jira sites are #2445.

#include "Config/ConfigManager.h"

#include <string>
#include <vector>

namespace smatchet {
namespace pane_targets {

struct NewPaneTarget {
    std::string BackendKey; ///< Tracker kind for PaneAddRequest::targetBackendKey ("Jira", "Plane", ...).
    /// Jira site to make active before the pane opens; empty when picking this row needs no switch
    /// (a non-Jira kind, a single Jira site, or the site that is already active).
    std::string SwitchToJiraDomain;
    std::string Label; ///< Menu row text: "New Plane pane", "New Jira · acme.atlassian.net pane".
};

/// Picker rows in KnownBackendKeys() order, Jira sites in JiraBackends order. An empty JiraBackends
/// (not yet hydrated) reads as the single site in the live Domain. Runs per frame per pane header, so
/// it never copies `cfg`.
std::vector<NewPaneTarget> BuildNewPaneTargets(const TrackerConfig& cfg);

/// The caret renders only when it offers a choice the bare "+" (duplicate this pane) does not: two or
/// more rows.
inline bool ShowNewPaneCaret(const std::vector<NewPaneTarget>& targets) { return targets.size() >= 2; }

} // namespace pane_targets
} // namespace smatchet
