#ifndef SMATCHET_INTERFACES_IAPP_PENDING_ACTIONS_H
#define SMATCHET_INTERFACES_IAPP_PENDING_ACTIONS_H

// Narrow facet for the pending-action queue (Quality Pillar 6): comments, worklogs and watches
// sent now or saved while the tracker is unreachable, and the queue-panel actions over them.
// AppController implements it; the queue panel TU (Ui/SmatchetOfflineQueueUi_Actions.cpp) depends on
// this instead of the full AppController.h. Rank-0 leaf (Interfaces/): the row and result types come
// from the rank-0 PendingActionTypes.h.
//
// Pillar-1 note: GetPendingActionsSnapshot has per-frame callers (the comments modal, the queue panel,
// the status bar). It is one atomic shared_ptr load behind a vtable call — no SQLite, no copy.

#include "PendingActionTypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class IAppPendingActions {
  public:
    virtual ~IAppPendingActions() = default;

    /// The focused pane's tracker, latched for an action the user is starting. Call it where the user
    /// acted (UI thread), before launching the worker that submits.
    virtual PendingActionTarget LatchPendingActionTarget() const = 0;

    /// Comment on `issueKey` for `target`: sent now, or saved and replayed on reconnect when the tracker is
    /// unreachable. Blocks on the network while online — call it from a worker.
    virtual PendingActionSubmitResult SubmitOrQueueComment(const PendingActionTarget& target,
                                                           const std::string& issueKey, const std::string& body) = 0;

    /// The queued and failed actions as last loaded; never null. No SQLite.
    virtual std::shared_ptr<const PendingActionsSnapshot> GetPendingActionsSnapshot() const = 0;

    /// Queue-panel actions; each finishes on a worker and republishes the snapshot.
    virtual void DiscardPendingActions(const std::vector<std::int64_t>& ids) = 0;
    virtual void RestoreDeadPendingActions(const std::vector<std::int64_t>& originalIds) = 0;
    virtual void DeleteDeadPendingActions(const std::vector<std::int64_t>& deadIds) = 0;
    /// A `needs_review` action the user confirmed did not land: queue it to send again.
    virtual void SendPendingActionAgain(std::int64_t id) = 0;

    /// Restart every offline queue's replay timer and replay now (the queue panel's retry button).
    virtual void RetryOfflineQueuesNow() = 0;

    /// Distinct cache keys (tracker site and account) of the live pane contexts. A queued row whose key is
    /// not among them is held: replay never sends it to another site (#2268). UI thread.
    virtual std::vector<std::string> LiveCacheBackendKeys() const = 0;
};

#endif // SMATCHET_INTERFACES_IAPP_PENDING_ACTIONS_H
