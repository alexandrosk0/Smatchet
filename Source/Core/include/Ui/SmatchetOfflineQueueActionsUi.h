#pragma once

// The Offline Queue panel's "Other queued changes" table (Quality Pillar 6): queued and failed comments,
// worklogs and watches, read from the IAppPendingActions snapshot, never SQLite. UI thread only.

#include <string>

class IAppPendingActions;

namespace SmatchetOfflineQueueActionsUi {

/// True when there is a queued or failed action to list.
bool HasRows(const IAppPendingActions& app);

/// Draws the table (nothing when empty) with discard (confirmed), send-again, retry and delete actions.
void Draw(IAppPendingActions& app);

/// A queued row is held when it was written for a tracker site no live pane uses (#2268): replay keeps it
/// until that site is active again. Both queue tables show it with this state label, and this hover tooltip
/// naming the site (called while the state item is hovered). Each drawn held row is counted for the UI tests.
const char* HeldStateLabel();
void ShowHeldTooltip(const std::string& backendKey);
void NoteHeldRowDrawn();
/// Held rows drawn since the last reset (UI thread; bucket-E).
int HeldRowsDrawnForTests();
void ResetHeldRowsDrawnForTests();

} // namespace SmatchetOfflineQueueActionsUi

/// Conflict resolver, for the UI tests (UI thread; defined with the resolver in SmatchetOfflineQueueUi.cpp).
/// True while the resolver shows a resolution that was typed before and kept when it was cancelled.
bool SmatchetOfflineConflictDraftRestoredForTest();
/// Forgets every kept resolution, so one test case does not open on what another one typed.
void SmatchetOfflineConflictDraftsClearForTest();
