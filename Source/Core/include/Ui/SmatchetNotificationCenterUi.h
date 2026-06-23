#pragma once

struct UiDrawSession;

// Notification Center (ticket-change-monitor plan, S3 / § Files-to-modify #13a) — a session-long,
// newest-first log of every toast the app has raised. Reads the bounded history ring exposed by
// SmatchetToastManager::History() (shipped in S2) and lists timestamp · type · title · message
// per row; a row click invokes that entry's RowAction (ticket entries focus the ticket once
// FocusTicketInGrid lands — a follow-up; informational entries are inert). A "Clear all" button
// empties the ring. Opened by the View menu, the `notifications` command, or a transient toast's
// own click (which raises SmatchetToastManager::RequestOpenCenter, consumed in SmatchetUI::Draw).
// UI-thread only — drawn from SmatchetUI::Draw; the manager singleton is not thread-safe.
void SmatchetDrawNotificationCenterWindow(UiDrawSession& d);
