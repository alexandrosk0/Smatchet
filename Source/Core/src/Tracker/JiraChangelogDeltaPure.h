#pragma once
// Jira-private changelog-delta parser (ticket-change-monitor plan, S1b). Sibling of
// JiraActivityFeed.h: Jira-shaped changelog JSON stays behind JiraClient and only the
// backend-agnostic TicketChangeSummary crosses out. Where JiraActivityFeed maps a user's
// whole activity window to TrackerActivityEntry, this maps ONE issue's `expand=changelog`
// histories to the salient from->to deltas the per-pane change monitor notifies on — the
// attributed (author + changelog-entry id) source that the cache-snapshot differ
// (TicketChangeDiffPure) cannot supply. Pure function, bucket-A tested
// (tests/Core/JiraChangelogDeltaPure.test.cpp) — no network, no JiraClient.

#include "Sync/TicketChangeDiffPure.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace JiraChangelogDeltaPure {

/// One attributed salient change parsed from a Jira changelog history item. `summary` is the
/// backend-agnostic notifiable change (always TicketChangeKind::Modified here — membership
/// kinds come from the reconcile path, not the changelog); `changelogEntryId` + `createdAt`
/// feed the monitor's per-pane seen-set dedup keyed on (issueKey, changelogEntryId).
struct JiraChangelogDelta {
    smatchet::TicketChangeSummary summary;
    std::string changelogEntryId; ///< Jira history `id` — stable per changelog entry.
    std::string createdAt;        ///< History `created` ISO timestamp.
};

/// Salient from->to deltas for one issue's `expand=changelog` JSON. For each history newer
/// than `sinceIsoMinute` (leading-16-char "YYYY-MM-DDTHH:MM" lexicographic compare; an empty
/// bound or a too-short timestamp keeps the entry), authored by anyone OTHER than
/// `selfAccountId` (self-authored changes are suppressed when `selfAccountId` is non-empty),
/// each item whose `fieldId` (fallback `field`) is in `roster` yields one JiraChangelogDelta:
/// kind=Modified, issueId=issue `key`, changedFieldLabel=the matched roster label,
/// from/to=display-preferred (`fromString`/`toString` else `from`/`to`), author=`displayName`.
/// Non-salient items, non-object nodes, and missing changelog are skipped. Output order
/// follows the changelog (histories, then items within a history).
std::vector<JiraChangelogDelta>
DeltasFromIssueJson(const nlohmann::json& issue, const std::string& sinceIsoMinute,
                    const std::vector<smatchet::SalientFieldRole>& roster,
                    const std::string& selfAccountId);

} // namespace JiraChangelogDeltaPure
