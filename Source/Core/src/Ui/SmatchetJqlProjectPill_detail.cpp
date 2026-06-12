#include "SmatchetJqlProjectPill_detail.h"

#include <string>

namespace SmatchetJqlProjectPill {
namespace detail {

bool EntryPassesPillFilter(const FieldCatalogCache::CachedProjectEntry& e, const std::string& backendKind,
                           const std::string& endpoint) {
    if (e.projectKey.empty()) {
        return false;
    }
    if (!e.backend.empty() && !backendKind.empty() && e.backend != backendKind) {
        return false;
    }
    if (!e.endpoint.empty() && !endpoint.empty() && e.endpoint != endpoint) {
        return false;
    }
    return true;
}

} // namespace detail
} // namespace SmatchetJqlProjectPill
