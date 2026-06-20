#pragma once

// TicketChangeDiffPure (ticket-change-monitor plan, S1a) — backend-agnostic snapshot diff:
// given two CachedTicket snapshots of one view (prev poll vs this poll), report which
// salient fields changed on tickets present in BOTH. Membership deltas (a key appeared /
// vanished) come from MembershipDiffPure + an existence probe in the monitor, not here.
// The Jira changelog path (JiraChangelogDeltaPure, S1b) yields the same TicketChangeSummary
// shape with a real author; this snapshot differ leaves author empty.
// "Salient" means a fixed roster of canonical roles — Status, Assignee, Priority, Summary,
// DueDate, Sprint, Labels, Components — each resolved per-backend to its concrete field id.
// Non-salient changes such as description, custom fields, or comment churn are ignored.
// There is no "canonicalId" namespace: salience IS this roster. Diff and formatting are
// free functions (PaneSyncKickPolicy.h precedent), unit-testable without a backend or pane.

#include <string>
#include <vector>

#include "CachedTicketTypes.h"

namespace smatchet {

/// One salient role: its per-backend concrete field id and the human label shown in a
/// toast. Multiple roster entries may share a label when a role resolves to more than
/// one backend field id (e.g. two Sprint custom fields).
struct SalientFieldRole {
    std::string fieldId; ///< Backend-concrete field id, e.g. "status", "customfield_10020".
    std::string label;   ///< Canonical role label shown in the toast, e.g. "Status", "Sprint".
};

/// What kind of change a TicketChangeSummary describes.
enum class TicketChangeKind {
    Modified, ///< A salient field changed on a ticket still in view.
    Added,    ///< A key newly entered the view (from MembershipDiffPure::AddedKeys).
    LeftView, ///< A key vanished from view but still exists server-side (probe 200).
    Deleted,  ///< A key vanished and is gone server-side (probe 404).
};

/// One notifiable change. For Modified, `changedFieldLabel` / `fromValue` / `toValue`
/// describe the field delta and `author` names who made it when known (empty from the
/// snapshot differ; filled by the changelog path). For Added/LeftView/Deleted only
/// `kind` and `issueId` are meaningful.
struct TicketChangeSummary {
    TicketChangeKind kind = TicketChangeKind::Modified;
    std::string issueId;           ///< Issue key, e.g. "PROJ-123".
    std::string changedFieldLabel; ///< Salient role label (Modified only).
    std::string fromValue;         ///< Prior field value (Modified only).
    std::string toValue;           ///< New field value (Modified only).
    std::string author;            ///< Who made the change, if known (Modified only).
};

/// True when `fieldId` is one of the salient roles in `roster`.
bool IsSalientChangeField(const std::string& fieldId, const std::vector<SalientFieldRole>& roster);

/// Salient-field deltas for tickets present in BOTH snapshots, matched by `id`. For each
/// such ticket and each roster entry, the field values are compared (CachedTicket field
/// map); any difference emits a Modified TicketChangeSummary with the role label and the
/// from/to values. Keys present in only one snapshot are ignored (membership is handled
/// by the caller). Output order follows `next`, then roster order within a ticket.
std::vector<TicketChangeSummary> DiffChangedTickets(const std::vector<CachedTicket>& prev,
                                                    const std::vector<CachedTicket>& next,
                                                    const std::vector<SalientFieldRole>& roster);

/// Compose a toast body from `changes`. Up to `cap` change lines are shown (clamped to
/// >= 1); any remainder is summarised as a trailing "+N more" line. Each line reads:
///   Modified: "PROJ-123 Status: To Do -> In Progress" (+ " (by Alice)" when author set;
///             "PROJ-123 Status: set to In Progress" when prior value was empty;
///             "PROJ-123 Status: cleared" when the new value is empty)
///   Added:    "PROJ-123 added to view"
///   LeftView: "PROJ-123 left view"
///   Deleted:  "PROJ-123 deleted"
/// Lines are joined with '\n'. Empty `changes` yields an empty string.
std::string FormatTicketChangeToast(const std::vector<TicketChangeSummary>& changes, int cap = 1);

} // namespace smatchet
