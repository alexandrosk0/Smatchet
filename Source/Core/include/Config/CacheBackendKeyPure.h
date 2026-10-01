#pragma once

// CacheBackendKeyPure — the key that namespaces the local cache (tickets, saved lookups and the offline
// write queues) by tracker site and account (Quality Pillar 6, #2268). Pure: no I/O, no Logger.h.
//
// Key shape: `<Kind>@<site>`, readable in logs and never carrying a secret or an email address:
//   Jira    Jira@<host>#<acct>              acct = first 12 hex of SHA-256 of the lower-cased account email
//   Plane   Plane@<host>/<workspace>
//   GitHub  GitHub@<host>/<owner>/<repo>
//   Linear  Linear@<host>/<team>
// A tracker whose identity is not configured keeps the bare kind ("Jira", "Plane", ...), which is also
// the legacy key shape, so an unconfigured tracker and the fixture backends are unaffected.

#include "ConfigManager.h"

#include <string>
#include <utility>
#include <vector>

namespace smatchet {
namespace cache_keys {

/// The cache key for the tracker `cfg` currently points at (its TrackerType and live site fields).
std::string TrackerCacheBackendKey(const TrackerConfig& cfg);

/// The one-time move from the legacy kind keys ("Jira", "Jira:<host>", "Plane", "GitHub", "Linear") to the
/// site keys `cfg` gives them, as (legacy key, site key) pairs. Covers every configured tracker, not only the
/// active one. A legacy key whose tracker has no configured identity maps to itself and is left out.
std::vector<std::pair<std::string, std::string>> LegacyCacheKeyRekeys(const TrackerConfig& cfg);

/// The tracker kind a cache key belongs to ("Jira" for "Jira@acme.atlassian.net#...", "Jira:host" or "Jira").
std::string CacheBackendKeyKind(const std::string& key);

/// A label for showing a cache key to the user: "Jira · acme.atlassian.net", "Plane · host/workspace". The
/// account hash is never shown. A bare kind returns the kind.
std::string DescribeCacheBackendKey(const std::string& key);

/// True when a queued row keyed `rowKey` matches none of the live contexts' keys: replay holds it, so it is
/// never sent to another site. An empty key matches nothing (replay never sends such a row either).
bool IsHeldCacheKey(const std::string& rowKey, const std::vector<std::string>& liveKeys);

} // namespace cache_keys
} // namespace smatchet
