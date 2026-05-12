#include "ProjectResolver.h"

#include "ITrackerClient.h"

#include <cctype>

namespace smatchet {

namespace {

// "TICKET-123" -> "TICKET". Returns "" when:
//   * no '-' present
//   * '-' is the first char (e.g. "-123")
//   * the part before '-' contains any non-[A-Za-z0-9_] character (Jira keys are uppercase letters
//     and digits; we accept the conservative identifier set so the helper is liberal in what it
//     accepts without misclassifying obviously non-key strings).
std::string ExtractKeyPrefix(const std::string& id) {
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

} // namespace

std::string ResolveProjectForDraft(const ITrackerClient* client,
                                   const std::string& activeViewQuery,
                                   const std::string& lastVisibleTicketId,
                                   const std::string& legacyFallback) {
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
        const std::string prefix = ExtractKeyPrefix(lastVisibleTicketId);
        if (!prefix.empty()) {
            return prefix;
        }
    }

    // (3) Legacy fallback — `cfg.ProjectKey` today; "" after PR 6 (final removal).
    return legacyFallback;
}

} // namespace smatchet
