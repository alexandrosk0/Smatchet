#pragma once

// The Offline Queue panel's "Other queued changes" table (Quality Pillar 6): queued and failed comments
// (later worklogs and watch), read from the IAppPendingActions snapshot, never SQLite. UI thread only.

class IAppPendingActions;

namespace SmatchetOfflineQueueActionsUi {

/// True when there is a queued or failed action to list.
bool HasRows(const IAppPendingActions& app);

/// Draws the table (nothing when empty) with discard (confirmed), send-again, retry and delete actions.
void Draw(IAppPendingActions& app);

} // namespace SmatchetOfflineQueueActionsUi
