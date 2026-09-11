#pragma once

#include "ConfigManager.h"

#include <cstddef>
#include <string>
#include <vector>

namespace smatchet {
namespace jira_backends {

/// Host used for equality / cache keys: scheme, path, port, and case stripped.
std::string NormalizeJiraHost(const std::string& domain);

bool HostsMatch(const std::string& a, const std::string& b);

/// Ensure JiraBackends[0] exists, seeded from Domain/Email/ApiToken when empty.
void EnsureHydrated(TrackerConfig& cfg);

/// After Load: `JiraBackends` currently holds JSON extras only. Prepend the first
/// instance from live Domain/Email/ApiToken. Does not overlay the active instance
/// onto live fields — call `ApplyActiveLiveFields` after env/CLI overrides.
void AdoptLoadedExtras(TrackerConfig& cfg);

/// When the first instance is active, copy live Domain/Email/ApiToken onto
/// JiraBackends[0] (Load-mutate-Save of those fields is the first site). Then
/// copy instance 0 onto the live fields so Save writes the first site at the
/// legacy top-level keys. Extra-active configs leave [0] unchanged so the extra
/// overlay does not leak into the first-instance keys.
void PrepareForPersist(TrackerConfig& cfg);

/// Overlay Domain/Email/ApiToken with the active instance; empty extra email/token
/// inherit from JiraBackends[0].
void ApplyActiveLiveFields(TrackerConfig& cfg);

/// Select an existing instance by host. Returns false when the host is unknown.
bool SelectActive(TrackerConfig& cfg, const std::string& domain);

/// Append an extra site. Empty/duplicate hosts return false.
bool AddExtra(TrackerConfig& cfg, const JiraBackendInstance& inst);

/// Remove JiraBackends[1 + extraIndex]. Falls back to instance 0 when the removed
/// row was active. extraIndex is 0-based among extras only.
bool RemoveExtraAt(TrackerConfig& cfg, std::size_t extraIndex);

/// Replace extras ([1+]) from `extras`. Remaps `ActiveJiraDomain` when the active
/// extra is renamed; falls back to instance 0 if that row was removed. Returns
/// false if any extra was rejected (empty or duplicate host). Does not overlay
/// live Domain (Preferences Test connection probes instance 0).
bool ReplaceExtras(TrackerConfig& cfg, const std::vector<JiraBackendInstance>& extras);

/// `"Jira"` for the first instance; `"Jira:<host>"` for an extra. Other tracker
/// kinds use NormalizeViewsBackendKey.
std::string TrackerCacheBackendKey(const TrackerConfig& cfg);

const JiraBackendInstance* FindByHost(const TrackerConfig& cfg, const std::string& domain);

} // namespace jira_backends
} // namespace smatchet
