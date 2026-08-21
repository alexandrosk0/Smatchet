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
/// Jira Cloud accountId (`557058:<uuid>` or `qm:<uuid>:<uuid>`), a bare UUID, or the
/// 24-char legacy object id (alphanumeric with at least one digit — Atlassian's own
/// examples carry non-hex letters, e.g. `…ede21g`). Shape-based, so an account with no
/// catalog entry is still recognisable as a hash.
bool LooksLikeAccountId(const std::string& token);

/// Collect every account-id-shaped value token (quoted or bare) in `query` that does NOT
/// resolve against `knownUsers`, deduped, in first-appearance order. This is the work list
/// for a by-accountId backend lookup: a saved view restored at startup carries ids no
/// session search has named yet, and Jira's user/search endpoint cannot match an id.
std::vector<std::string> CollectUnresolvedAccountIds(const std::string& query,
                                                     const std::vector<TrackerUser>& knownUsers);

/// Strip one level of double-quoting from a value token (`"a \"b\""` -> `a "b"`), or return
/// the text unchanged when it is not quoted. The inverse of the quoting an autocomplete
/// insert applies, so a caller holding an insert string can recover the raw value.
std::string UnquoteValueToken(const std::string& text);

/// Rewrite `query` for DISPLAY: every value token (quoted or bare) matching a known
/// `TrackerUser::AccountId` becomes that user's display name, re-quoted when the name needs
/// it; unmatched tokens are left byte-for-byte. `outReplaced`, when non-null, receives the
/// substitution count — 0 means the caller should show the raw query and skip the echo.
std::string RenderQueryWithUserNames(const std::string& query, const std::vector<TrackerUser>& users, int* outReplaced);

} // namespace jql_user_display
