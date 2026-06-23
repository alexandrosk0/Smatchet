#pragma once

struct UiDrawSession;

// Notification Center (ticket-change-monitor plan, S3) — a session-long, newest-first log of every
// toast, read from the toast manager's bounded history ring (S2); each row runs its optional row
// action on click. Opened from the View menu, the notifications command, or a toast click. See cpp.
void SmatchetDrawNotificationCenterWindow(UiDrawSession& d);
