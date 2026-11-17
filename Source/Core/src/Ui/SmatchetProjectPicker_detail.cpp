#include "SmatchetProjectPicker_detail.h"

#include "StringUtil.h"

#include <algorithm>
#include <string>
#include <utility>

namespace SmatchetProjectPicker {
namespace detail {

bool ContainsCi(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return ToLowerAsciiCopy(haystack).find(ToLowerAsciiCopy(needle)) != std::string::npos;
}

std::string MakeRowLabel(const RemoteProject& p, const std::string& backendKind) {
    // Jira: "KEY — DisplayName". Plane: key is empty, display name only. Linear's draft scope is the
    // Team, which carries a real key (the TEAM-123 prefix) + a name, so it reads like a Jira project:
    // "KEY — Team name".
    if (backendKind == "Plane" || p.key.empty()) {
        return p.displayName.empty() ? p.id : p.displayName;
    }
    if (p.displayName.empty()) {
        return p.key;
    }
    return p.key + " \xE2\x80\x94 " + p.displayName;
}

bool RecentEntryPasses(const FieldCatalogCache::CachedProjectEntry& e, const std::string& backendKind,
                       const std::string& endpoint, const std::string& filter) {
    if (e.projectKey.empty()) {
        return false;
    }
    if (!backendKind.empty() && !e.backend.empty() && e.backend != backendKind) {
        return false;
    }
    if (!endpoint.empty() && !e.endpoint.empty() && e.endpoint != endpoint) {
        return false;
    }
    if (!filter.empty() && !ContainsCi(e.projectKey, filter)) {
        return false;
    }
    return true;
}

std::vector<std::string> RecentSeedKeys(std::vector<FieldCatalogCache::CachedProjectEntry> entries,
                                        const std::string& backendKind, const std::string& endpoint,
                                        const std::vector<std::string>& listedKeys, std::size_t maxKeys) {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const FieldCatalogCache::CachedProjectEntry& a,
                        const FieldCatalogCache::CachedProjectEntry& b) { return a.lastUsedUnix > b.lastUsedUnix; });
    std::vector<std::string> keys;
    for (auto& e : entries) {
        if (keys.size() >= maxKeys) {
            break;
        }
        if (!RecentEntryPasses(e, backendKind, endpoint, std::string())) {
            continue;
        }
        if (std::find(listedKeys.begin(), listedKeys.end(), e.projectKey) == listedKeys.end()) {
            continue;
        }
        if (std::find(keys.begin(), keys.end(), e.projectKey) == keys.end()) {
            keys.push_back(std::move(e.projectKey));
        }
    }
    return keys;
}

std::string AllProjectKey(const RemoteProject& p, const std::string& backendKind) {
    return (backendKind == "Plane") ? p.id : p.key;
}

bool AllProjectPasses(const std::string& key, const RemoteProject& p, const std::string& filter) {
    if (key.empty()) {
        return false;
    }
    if (!filter.empty() && !ContainsCi(key, filter) && !ContainsCi(p.displayName, filter)) {
        return false;
    }
    return true;
}

} // namespace detail
} // namespace SmatchetProjectPicker
