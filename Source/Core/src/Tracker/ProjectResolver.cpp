#include "ProjectResolver.h"

#include "ITrackerConnectivity.h"
#include "TrackerFieldSchema.h"

namespace smatchet {

namespace {
// ASCII-only classification — std::isalpha/isalnum/isdigit from <cctype> are locale-dependent and
// can accept non-ASCII bytes, contradicting the ASCII-only Jira-key grammar documented in the header.
bool IsAsciiAlpha(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
bool IsAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}
bool IsAsciiAlphaNum(char c) {
    return IsAsciiAlpha(c) || IsAsciiDigit(c);
}
// Token alphabet for FindFirstIssueKeyInText — the chars that can appear inside a
// candidate "KEY-123" token. Everything else is a word boundary.
bool IsIssueKeyTokenChar(char c) {
    return IsAsciiAlphaNum(c) || c == '_' || c == '-';
}
} // namespace

std::string ExtractIssueKeyPrefix(const std::string& id) {
    const auto dash = id.find('-');
    if (dash == std::string::npos || dash == 0) {
        return std::string();
    }
    // Jira project keys match ^[A-Za-z][A-Za-z0-9_]* — they MUST start with a letter. The
    // leading-letter requirement is what keeps UUIDs ("550e8400-...") and other digit-leading
    // strings from being misclassified as keys (they return "" instead of a hex fragment).
    if (!IsAsciiAlpha(id[0])) {
        return std::string();
    }
    for (std::size_t i = 1; i < dash; ++i) {
        const char c = id[i];
        if (!IsAsciiAlphaNum(c) && c != '_') {
            return std::string();
        }
    }
    // The part after the dash must be a Jira issue number (digits only) for the whole string to
    // be a real "KEY-123" issue key — guards against "PROJ-foo" leaking through as a key.
    if (dash + 1 >= id.size()) {
        return std::string();
    }
    for (std::size_t i = dash + 1; i < id.size(); ++i) {
        if (!IsAsciiDigit(id[i])) {
            return std::string();
        }
    }
    return id.substr(0, dash);
}

IssueKeyMatch FindFirstIssueKeyInText(const std::string& text) {
    IssueKeyMatch match;
    std::size_t i = 0;
    while (i < text.size()) {
        if (!IsIssueKeyTokenChar(text[i])) {
            ++i;
            continue;
        }
        std::size_t end = i + 1;
        while (end < text.size() && IsIssueKeyTokenChar(text[end])) {
            ++end;
        }
        const std::string token = text.substr(i, end - i);
        if (!ExtractIssueKeyPrefix(token).empty()) {
            match.Key = token;
            match.Start = i;
            match.End = end;
            return match;
        }
        i = end;
    }
    return match;
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

    // (3) Legacy fallback — "" now that the global project key is removed.
    return legacyFallback;
}

std::string ResolvePlaneOperationProject(ITrackerConnectivity* client, const std::string& viewQuery,
                                         const std::string& cfgQuery) {
    if (client == nullptr || client->GetTrackerType() != "Plane") {
        return std::string();
    }
    std::string projectKey = client->ExtractProjectFromQuery(viewQuery);
    if (projectKey.empty()) {
        projectKey = client->ExtractProjectFromQuery(cfgQuery);
    }
    if (projectKey.empty()) {
        const std::vector<RemoteProject> projects = client->ListProjects();
        if (projects.size() == 1 && !projects.front().id.empty()) {
            projectKey = projects.front().id;
        }
    }
    return projectKey;
}

} // namespace smatchet
