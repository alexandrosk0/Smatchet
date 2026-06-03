#include "SmatchetJqlProjectPill_detail.h"

#include <string>

namespace SmatchetJqlProjectPill {
namespace detail {

bool EntryPassesPillFilter(const FieldCatalogCache::CachedProjectEntry& e, const std::string& domain) {
    if (e.projectKey.empty()) {
        return false;
    }
    if (!e.backend.empty() && e.backend != "Jira") {
        return false;
    }
    if (!e.endpoint.empty() && !domain.empty() && e.endpoint != domain) {
        return false;
    }
    return true;
}

} // namespace detail
} // namespace SmatchetJqlProjectPill
