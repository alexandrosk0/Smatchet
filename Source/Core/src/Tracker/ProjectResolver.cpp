#include "ProjectResolver.h"

#include "ITrackerConnectivity.h"

#include <cctype>

namespace smatchet {

std::string ExtractIssueKeyPrefix(const std::string& id) {
    const auto dash = id.find('-');
    if (dash == std::string::npos || dash == 0) {
        return std::string();
    }
    // Jira project keys match ^[A-Za-z][A-Za-z0-9_]* — they MUST start with a letter. The
    // leading-letter requirement is what keeps UUIDs ("550e8400-...") and other digit-leading
    // strings from being misclassified as keys (they return "" instead of a hex fragment).
    const unsigned char first = static_cast<unsigned char>(id[0]);
    if (std::isalpha(first) == 0) {
        return std::string();
    }
    for (std::size_t i = 1; i < dash; ++i) {
        const unsigned char c = static_cast<unsigned char>(id[i]);
        if (std::isalnum(c) == 0 && c != '_') {
            return std::string();
        }
    }
    // The part after the dash must be a Jira issue number (digits only) for the whole string to
    // be a real "KEY-123" issue key — guards against "PROJ-foo" leaking through as a key.
    if (dash + 1 >= id.size()) {
        return std::string();
    }
    for (std::size_t i = dash + 1; i < id.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(id[i])) == 0) {
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
