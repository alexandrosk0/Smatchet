#include "SmatchetProjectPicker_detail.h"

#include "StringUtil.h"

#include <string>

namespace SmatchetProjectPicker {
namespace detail {

bool ContainsCi(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return ToLowerAsciiCopy(haystack).find(ToLowerAsciiCopy(needle)) != std::string::npos;
}

std::string MakeRowLabel(const RemoteProject& p, const std::string& backendKind) {
    // Jira: "KEY — DisplayName". Plane: key is empty, display name only.
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
