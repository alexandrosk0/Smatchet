#pragma once

#include <string>
#include <vector>

struct TrackerUser; // Tracker/TrackerFieldSchema.h

/// Presentation helpers that turn the opaque account identifiers a tracker query carries
/// into the display names a human recognises. JQL on Jira Cloud can only match a user by
/// accountId (`assignee = "712020:00aa4ab4-…"`), so the query text of record must keep the
/// id — these render a readable ECHO of it, never the query sent to the backend. Pure:
/// plain string / vector transforms, no ImGui, no backend, no I/O.
namespace jql_user_display {

/// True when `token` is an opaque account identifier rather than human-readable text: a
/// Jira Cloud accountId (`557058:<uuid>` or `qm:<uuid>:<uuid>`), a bare UUID, or the 24-hex
/// object id older deployments emit. Shape-based, so an account with no catalog entry is
/// still recognisable as a hash.
bool LooksLikeAccountId(const std::string& token);

/// Rewrite `query` for DISPLAY: every value token (quoted or bare) matching a known
/// `TrackerUser::AccountId` becomes that user's display name, re-quoted when the name needs
/// it; unmatched tokens are left byte-for-byte. `outReplaced`, when non-null, receives the
/// substitution count — 0 means the caller should show the raw query and skip the echo.
std::string RenderQueryWithUserNames(const std::string& query, const std::vector<TrackerUser>& users, int* outReplaced);

} // namespace jql_user_display
