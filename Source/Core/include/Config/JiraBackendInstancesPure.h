#pragma once

#include "ConfigManager.h"

#include <cstddef>
#include <string>

namespace smatchet {
namespace jira_backends {

/// Host used for equality / cache keys: scheme, path, port, and case stripped.
std::string NormalizeJiraHost(const std::string& domain);

bool HostsMatch(const std::string& a, const std::string& b);

/// Ensure JiraBackends[0] exists, seeded from Domain/Email/ApiToken when empty.
void EnsureHydrated(TrackerConfig& cfg);

/// After Load: `JiraBackends` currently holds JSON extras only. Prepend the first
/// instance from live Domain/Email/ApiToken (env overrides already applied), then
/// overlay the active instance onto those live fields.
void AdoptLoadedExtras(TrackerConfig& cfg);

/// Copy instance 0 onto Domain/Email/ApiToken so Save writes the first site at the
/// legacy top-level keys.
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

/// `"Jira"` for the first instance; `"Jira:<host>"` for an extra. Other tracker
/// kinds use NormalizeViewsBackendKey.
std::string TrackerCacheBackendKey(const TrackerConfig& cfg);

const JiraBackendInstance* FindByHost(const TrackerConfig& cfg, const std::string& domain);

} // namespace jira_backends
} // namespace smatchet
