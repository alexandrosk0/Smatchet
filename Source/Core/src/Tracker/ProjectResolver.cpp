#include "ProjectResolver.h"

#include "ITrackerConnectivity.h"

#include <cctype>

namespace smatchet {

std::string ExtractIssueKeyPrefix(const std::string& id) {
    const auto dash = id.find('-');
    if (dash == std::string::npos || dash == 0) {
        return std::string();
    }
    for (std::size_t i = 0; i < dash; ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (!std::isalnum(c) && c != '_') {
            return std::string();
        }
    }
    return id.substr(0, dash);
}

std::string ResolveProjectForDraft(const ITrackerConnectivity* client, const std::string& activeViewQuery,
                                   const std::string& lastVisibleTicketId, const std::string& legacyFallback) {
    // (1) JQL / structured-query scope (backend-specific parse lives inside the concrete client).
    if (client != nullptr && !activeViewQuery.empty()) {
        const std::string fromQuery = client->ExtractProjectFromQuery(activeViewQuery);
        if (!fromQuery.empty()) {
            return fromQuery;
        }
    }

    // (2) Last-visible-ticket key prefix. Jira-only — Plane IDs are UUIDs without a project prefix,
    //     so we skip this step when the backend reports itself as Plane.
    const bool isPlane = (client != nullptr && client->GetTrackerType() == "Plane");
    if (!isPlane && !lastVisibleTicketId.empty()) {
        const std::string prefix = ExtractIssueKeyPrefix(lastVisibleTicketId);
        if (!prefix.empty()) {
            return prefix;
        }
    }

    // (3) Legacy fallback — `cfg.ProjectKey` today; "" after PR 6 (final removal).
    return legacyFallback;
}

std::string ResolveProjectForDraftFromParent(const std::string& parentTicketId, const ITrackerConnectivity* client,
                                             const std::string& activeViewQuery, const std::string& legacyFallback) {
    const bool isPlane = (client != nullptr && client->GetTrackerType() == "Plane");
    if (!isPlane && !parentTicketId.empty()) {
        const std::string prefix = ExtractIssueKeyPrefix(parentTicketId);
        if (!prefix.empty()) {
            return prefix;
        }
    }
    return ResolveProjectForDraft(client, activeViewQuery, std::string(), legacyFallback);
}

} // namespace smatchet
