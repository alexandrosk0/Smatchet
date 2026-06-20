#pragma once

// MembershipDiffPure — pure set-difference helper for the ticket-change monitor's
// membership-reconcile step (ticket-change-monitor plan, S1a). A pane's "tracked set"
// is its in-memory ActiveTickets keys at poll time; each cycle the monitor re-fetches
// the view's current key list (keys-only) and asks: which keys vanished? Those removed
// keys are the candidates the monitor then disambiguates with a per-key existence probe
// (404 -> Deleted, 200 -> LeftView). Extracted as a header-pure free function
// (PaneSyncKickPolicy.h / BackgroundWorkerReap.h precedent) so the reconcile is
// unit-testable without a backend or a live pane.

#include <string>
#include <unordered_set>
#include <vector>

namespace smatchet {

/// Keys present in `cachedKeys` (the pane's prior tracked set) but absent from
/// `fetchedKeys` (the view's current membership) — i.e. issues that left the tracked
/// set since the last poll. Output preserves `cachedKeys` order and is de-duplicated
/// (a key repeated in `cachedKeys` appears at most once in the result).
inline std::vector<std::string> RemovedKeys(const std::vector<std::string>& cachedKeys,
                                            const std::vector<std::string>& fetchedKeys) {
    const std::unordered_set<std::string> present(fetchedKeys.begin(), fetchedKeys.end());
    std::unordered_set<std::string> emitted;
    std::vector<std::string> removed;
    for (const std::string& key : cachedKeys) {
        if (present.find(key) != present.end()) {
            continue;
        }
        if (!emitted.insert(key).second) {
            continue;
        }
        removed.push_back(key);
    }
    return removed;
}

/// Keys present in `fetchedKeys` (current membership) but absent from `cachedKeys`
/// (the prior tracked set) — i.e. issues that newly entered the view since the last
/// poll. Output preserves `fetchedKeys` order and is de-duplicated. The first poll
/// establishes the baseline silently; callers suppress Added toasts on that cycle.
inline std::vector<std::string> AddedKeys(const std::vector<std::string>& cachedKeys,
                                          const std::vector<std::string>& fetchedKeys) {
    const std::unordered_set<std::string> prior(cachedKeys.begin(), cachedKeys.end());
    std::unordered_set<std::string> emitted;
    std::vector<std::string> added;
    for (const std::string& key : fetchedKeys) {
        if (prior.find(key) != prior.end()) {
            continue;
        }
        if (!emitted.insert(key).second) {
            continue;
        }
        added.push_back(key);
    }
    return added;
}

/// One vanished key after the existence probe: `stillExists` is the ProbeIssueExists
/// verdict — true (200, the conservative default) means "left the view", false (404)
/// means "deleted server-side".
struct MembershipRemovalVerdict {
    std::string issueKey;
    bool stillExists = true;
};

/// The apply-side decision for a batch of probe verdicts, separated from the side
/// effects so it can be unit-tested without a pane or cache:
///   - `residentRemovals` — verdicts whose key is currently in the pane's cache, in
///     input order. These drive the in-memory erase and one LeftView/Deleted toast each
///     (kind from `stillExists`). A verdict for a key the pane no longer holds (already
///     evicted, never resident) is dropped here — no erase, no toast.
///   - `cacheDeleteKeys` — every 404 verdict's key, in input order, REGARDLESS of pane
///     residency: the shared per-backend cache spans panes, so a deleted issue is purged
///     from it even if this pane never tracked it. A 200 verdict never deletes.
struct MembershipReconcilePlan {
    std::vector<MembershipRemovalVerdict> residentRemovals;
    std::vector<std::string> cacheDeleteKeys;
};

/// Classify `verdicts` against `residentIds` (the pane's current cached ticket ids).
/// Pure: see MembershipReconcilePlan for the residency vs cache-delete split.
inline MembershipReconcilePlan ClassifyMembershipRemovals(const std::vector<std::string>& residentIds,
                                                          const std::vector<MembershipRemovalVerdict>& verdicts) {
    const std::unordered_set<std::string> resident(residentIds.begin(), residentIds.end());
    MembershipReconcilePlan plan;
    for (const MembershipRemovalVerdict& v : verdicts) {
        if (!v.stillExists) {
            plan.cacheDeleteKeys.push_back(v.issueKey);
        }
        if (resident.find(v.issueKey) != resident.end()) {
            plan.residentRemovals.push_back(v);
        }
    }
    return plan;
}

} // namespace smatchet
