#pragma once

// MonitorProbeConfigPure - which tracker config one change-monitor cycle probes a pane with (docs/plans/active/
// delta-sync.md § Phases (phase 6)). Header-only, C++14, no I/O, so the choice is unit-testable without an
// AppController.

#include "Config/CacheBackendKeyPure.h" // TrackerCacheBackendKey: same site and account
#include "Config/ConfigManager.h"       // TrackerConfig

#include <string>
#include <utility>

namespace smatchet {
namespace sync {

/// Copies the tracker secrets of `globalCfg` over `sessionCfg` when both name the same site and account (the same
/// cache key, #2268). The Jira and Plane readers build their auth header from the config of each request, and a
/// session config keeps the secrets it was latched with: without this, a token saved after a pane's last sync left
/// that pane's probes failing with 401 until it synced again. A pane on another site keeps its own secrets - the
/// global ones belong to the focused site and would pair that site's token with this one's host.
inline void RefreshSessionSecretsFromGlobal(const TrackerConfig& globalCfg, TrackerConfig& sessionCfg) {
    if (smatchet::cache_keys::TrackerCacheBackendKey(globalCfg) !=
        smatchet::cache_keys::TrackerCacheBackendKey(sessionCfg)) {
        return;
    }
    sessionCfg.Email = globalCfg.Email;
    sessionCfg.ApiToken = globalCfg.ApiToken;
    sessionCfg.PlaneApiKey = globalCfg.PlaneApiKey;
    sessionCfg.GitHubPat = globalCfg.GitHubPat;
    sessionCfg.LinearApiKey = globalCfg.LinearApiKey;
}

/// Picks the config a monitor cycle probes a pane with.
///
/// The pane's rows answer its latest sync session, so once one has started (`sessionStarted`) its config
/// (`sessionCfg`) wins whole: its query (an empty one is the backend's default query, not "unknown"), its repository
/// or project (GitHubOwner / GitHubRepo, ...) and its SelectedFields. Probing a pane with the focused pane's config
/// instead ran its query against the focused pane's repository, and the membership reconcile then dropped the
/// pane's rows as "left the view". Only the monitor's own settings - whether it runs, its interval and whether
/// it may take the delta (roster) path - stay global: they are read from `globalCfg`, and so are the secrets when
/// `globalCfg` names the same site and account (RefreshSessionSecretsFromGlobal).
///
/// Before a session has started the focused config (`globalCfg`) is all that is known; `kickedJql`
/// (GridLiveContext::lastSyncedJql, written by a pane kick or a focus switch) replaces its query when set.
inline TrackerConfig ChooseMonitorProbeConfig(const TrackerConfig& globalCfg, bool sessionStarted,
                                              TrackerConfig sessionCfg, const std::string& kickedJql) {
    if (!sessionStarted) {
        TrackerConfig cfg = globalCfg;
        if (!kickedJql.empty()) {
            cfg.JqlQuery = kickedJql;
        }
        return cfg;
    }
    sessionCfg.TicketChangeMonitorEnabled = globalCfg.TicketChangeMonitorEnabled;
    sessionCfg.TicketChangeMonitorIntervalSec = globalCfg.TicketChangeMonitorIntervalSec;
    sessionCfg.DeltaSyncEnabled = globalCfg.DeltaSyncEnabled;
    RefreshSessionSecretsFromGlobal(globalCfg, sessionCfg);
    return sessionCfg; // a by-value parameter is moved out implicitly
}

} // namespace sync
} // namespace smatchet
