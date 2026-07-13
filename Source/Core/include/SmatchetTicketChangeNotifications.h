#pragma once

// SmatchetTicketChangeNotifications (ticket-change-monitor plan, S1c-2, item 12) — the
// "plain Push" notifier sink for the per-pane change monitor. Given the salient changes a
// poll detected (DiffChangedTickets / the membership reconcile), surface ONE summarised toast
// rather than one per change. Sibling of SmatchetGridNotifications.cpp; the richer toast-history
// ring + Notification Center (plan items 13/13a) land in S2/S3 and will route through here.
// UI-thread only — SmatchetToastManager is not thread-safe; the monitor calls this from its
// main-thread apply hop, never from the off-thread fetch.

#include <functional>
#include <string>
#include <vector>

#include "Sync/TicketChangeDiffPure.h"

/// Handler the UI layer registers so a Notification Center row for a ticket change can focus that
/// ticket in the grid (FocusTicketInGrid, ticket-change-monitor plan item 11). Args: the pane the
/// change was detected on + the changed issue id. Invoked on the UI thread from the center row
/// click. Registered once at UI init; a null handler (never registered) makes the row inert.
using TicketChangeFocusHandler = std::function<void(const std::string& paneId, const std::string& issueId)>;
void SetTicketChangeFocusHandler(TicketChangeFocusHandler handler);

/// Surface `changes` as a single summarised "Tickets" toast (FormatTicketChangeToast, cap 1 +
/// "+N more"). Empty `changes` is a no-op. Consecutive calls carrying the identical change set
/// are de-duplicated by a function-static last-signature guard, so an overlapping/retried poll
/// that re-reports the same deltas does not double-toast. `paneId` is the pane the changes were
/// detected on; it is captured into the toast's row action so a Notification Center click can
/// focus the primary changed ticket in that pane. UI thread only.
void NotifyTicketChanges(const std::vector<smatchet::TicketChangeSummary>& changes,
                         const std::string& paneId = std::string());
