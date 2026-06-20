#pragma once

// SalientRosterResolve (ticket-change-monitor plan, S1c-2) — the catalog->roster bridge for the
// per-pane change monitor. It maps the fixed canonical salient roster onto one pane's field
// catalog, so it necessarily consumes the layer-3 Tracker schema (TrackerField). It lives at the
// root layer (rank 0) rather than in Sync/ (rank 2) precisely so this forward dependency on
// Tracker/ is a legitimate un-categorized->subsystem edge, not a low->high layer back-edge (same
// pattern the include-graph gate blesses for GridLiveContext.h -> Tracker/TrackerFieldSchema.h).
// The pure differ that consumes the resolved roster stays in Sync/TicketChangeDiffPure.h.

#include <vector>

#include "Sync/TicketChangeDiffPure.h"  // SalientFieldRole (output)
#include "Tracker/TrackerFieldSchema.h" // TrackerField (input)

namespace smatchet {

/// Resolve the fixed canonical salient roster — Status, Assignee, Priority, Summary, DueDate,
/// Sprint, Labels, Components — against one pane's field catalog, mapping each role to the
/// concrete backend field id(s) that carry it. Resolution prefers the structural
/// `TrackerFieldFamily` (Status/Sprint/Labels), then the Jira `SchemaSystem` id, then a
/// case-insensitive field-name match so non-Jira backends still resolve. A field matches at
/// most one role; a role absent from the catalog simply yields no entry (the diff skips it).
/// Output follows canonical role order, then catalog order within a role. Pure — no backend,
/// no network; this is the per-call input to DiffChangedTickets.
std::vector<SalientFieldRole> ResolveSalientRoster(const std::vector<TrackerField>& fields);

} // namespace smatchet
