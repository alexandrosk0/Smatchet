#pragma once

#include <cstddef>
#include <string>

class ITrackerConnectivity;

namespace smatchet {

/** "TICKET-123" -> "TICKET". The input must match a real Jira issue key shape:
 *  `^[A-Za-z][A-Za-z0-9_]*-[0-9]+$`. Returns "" otherwise — including when no '-'
 *  is present, '-' is the first char (e.g. "-123"), the prefix does not start with
 *  an ASCII letter, the prefix contains a non-[A-Za-z0-9_] char, or the suffix is
 *  not digits-only. The leading-letter requirement is what keeps UUIDs
 *  ("550e8400-e29b-...") and other digit-leading strings from being misclassified
 *  as keys. Pure; no I/O. */
std::string ExtractIssueKeyPrefix(const std::string& id);

/** First Jira-shaped issue key found in free text ("see PROJ-123 for details" -> "PROJ-123"
 *  at [4, 12)). Tokenizes on word boundaries (a token is a maximal run of [A-Za-z0-9_-]) and
 *  validates each token via ExtractIssueKeyPrefix — key validation stays single-source. */
struct IssueKeyMatch {
    std::string Key;        ///< "" when no key was found.
    std::size_t Start = 0;  ///< byte offset of the key's first char in `text`.
    std::size_t End = 0;    ///< one past the key's last char (Start == End when no match).
};
IssueKeyMatch FindFirstIssueKeyInText(const std::string& text);

/** Resolve the project key for a new draft / bulk-import row.
 *
 *  Resolution order (Jira):
 *    1. client->ExtractProjectFromQuery(activeViewQuery) — JQL `project = …` scope
 *    2. lastVisibleTicketId key prefix — "TICKET-123" → "TICKET"
 *    3. legacyFallback — `cfg.ProjectKey` today; "" after PR 6.
 *
 *  For Plane the key-prefix step is skipped (Plane IDs are UUIDs with no project prefix);
 *  resolution is (1) → (3).
 *
 *  Pure C++14; no I/O. `client` may be null — then steps 1 (and the Plane-detect for step 2)
 *  degrade to "skip" and we fall straight to the prefix / legacy fallback.
 *
 *  Added in PR 2 of docs/plans/shipped/remove-global-project-key.md.
 */
std::string ResolveProjectForDraft(const ITrackerConnectivity* client, const std::string& activeViewQuery,
                                   const std::string& lastVisibleTicketId, const std::string& legacyFallback);

/** PR 4b / OQ-6: when a draft is built from a known parent ticket (clone / child-of), the
 *  parent's own project — not the active view — should seed the picker. For Jira this is the
 *  parent's key prefix ("PARENT-12" -> "PARENT"). For Plane the parent has no key prefix, so
 *  this helper falls straight through to ResolveProjectForDraft.
 *
 *  Step order (Jira):
 *    1. parentTicketId key prefix
 *    2. ResolveProjectForDraft(client, activeViewQuery, "", legacyFallback)
 *  (Plane skips step 1.) */
std::string ResolveProjectForDraftFromParent(const std::string& parentTicketId, const ITrackerConnectivity* client,
                                             const std::string& activeViewQuery, const std::string& legacyFallback);

} // namespace smatchet
