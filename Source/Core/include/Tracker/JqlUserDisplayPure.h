#pragma once

#include <string>
#include <vector>

struct TrackerField; // Tracker/TrackerFieldSchema.h
struct TrackerUser;  // Tracker/TrackerFieldSchema.h

/// Two-way translation between the opaque account identifiers a tracker query carries and
/// the display names a human recognises. The JQL editor buffer holds the readable name form
/// (`assignee = "Jane Doe"`); the query of record on the wire and on disk stays id-canonical
/// (`assignee = "712020:00aa4ab4-…"`) — RenderQueryWithUserNames maps id→name for display
/// and RenderQueryWithAccountIds maps name→id at the apply boundaries. Pure: plain
/// string / vector transforms, no ImGui, no backend, no I/O.
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

/// Rewrite `query` for DISPLAY: inside the value position of a user-type field (resolved
/// against `fields`, the same clause walk RenderQueryWithAccountIds uses), a value token
/// matching a known `TrackerUser::AccountId` becomes that user's display name, re-quoted
/// when the name needs it. Ids anywhere else — non-user fields, cf[…] clauses, function
/// arguments, the ORDER BY tail — stay byte-for-byte, so the editor only ever shows a
/// rewrite the reverse mapping can undo. Empty `fields` rewrites nothing (fail-safe).
/// `outReplaced`, when non-null, receives the substitution count.
std::string RenderQueryWithUserNames(const std::string& query, const std::vector<TrackerField>& fields,
                                     const std::vector<TrackerUser>& users, int* outReplaced);

/// Rewrite `query` for the WIRE: inside the value position of a user-type field (resolved
/// against `fields` via FindTrackerField + IsQueryUserField), a value token that uniquely
/// case-insensitively matches a `TrackerUser::DisplayName` becomes that user's AccountId,
/// re-quoted as the id needs. Everything else — other fields' values, keywords, tokens that
/// already look like account ids, unknown or ambiguous names — is left byte-for-byte, so an
/// unresolvable name reaches the backend as typed and fails loudly rather than silently.
/// `users` must be ONE merged vector (catalog + search-resolved) — uniqueness is judged
/// across all known users at once. `outReplaced`, when non-null, receives the substitution
/// count.
std::string RenderQueryWithAccountIds(const std::string& query, const std::vector<TrackerField>& fields,
                                      const std::vector<TrackerUser>& users, int* outReplaced);

} // namespace jql_user_display
