#pragma once

// Worker half of the change monitor's ROSTER path (docs/plans/active/delta-sync.md § Phase 6). Header-only and
// free of AppController state so it is unit-testable against a fake reader.

#include "ITrackerIssueReader.h"
#include "Logger.h"
#include "Sync/ChangeMonitorPaneState.h"
#include "Sync/DeltaSyncPure.h"
#include "Sync/MembershipDiffPure.h"
#include "Sync/RefusalIsolatingFetchPure.h"
#include "TrackerRoster.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace smatchet {
namespace sync {

/// Existence probes spent per monitor cycle (the remainder is retried next cycle).
constexpr std::size_t kMaxVanishedProbesPerCycle = 25;
/// Bodies downloaded per monitor cycle; a larger backlog is picked up next cycle (the diff is idempotent).
constexpr std::size_t kMaxMonitorBodiesPerCycle = 200;

/// ProbeIssueExists for each of `removed` (capped) -> one verdict per probed key. A probe error is
/// non-destructive (the row is reported as still existing).
inline std::vector<MembershipRemovalVerdict> ProbeVanishedKeys(ITrackerIssueReader& reader, const TrackerConfig& cfg,
                                                               const std::vector<std::string>& removed,
                                                               const std::string& paneId) {
    std::vector<MembershipRemovalVerdict> verdicts;
    for (std::size_t i = 0; i < removed.size() && i < kMaxVanishedProbesPerCycle; ++i) {
        auto probe = reader.ProbeIssueExists(cfg, removed[i]);
        MembershipRemovalVerdict v;
        v.issueKey = removed[i];
        v.stillExists = static_cast<bool>(probe) ? probe.value() : true;
        verdicts.push_back(v);
    }
    if (removed.size() > kMaxVanishedProbesPerCycle) {
        LOG_INFO("ChangeMonitor reconcile capped pane='%s': %zu vanished, probing %zu this cycle", paneId.c_str(),
                 removed.size(), kMaxVanishedProbesPerCycle);
    }
    return verdicts;
}

/// How a roster probe ended.
enum class RosterProbeOutcome {
    Applied,   ///< outFetched / outVerdicts are the cycle's result.
    Skipped,   ///< Soft failure (logged); the caller skips the cycle and leaves its anchor alone.
    UseLegacy, ///< The roster was cut short by a cap/deadline, so it cannot prove membership; run the legacy path.
};

/// What a cycle needs to know about the pane beyond its resident stamps, and what it reports back.
struct RosterProbeContext {
    /// Keys an earlier cycle found the tracker refusing: not asked for again until the pane's next reload.
    const std::unordered_set<std::string>* RefusedKeys = nullptr;
    /// Resident rows the hierarchy walk owns (WalkOwnedKeys): never reported as "left the view".
    const std::unordered_set<std::string>* WalkOwned = nullptr;
    /// Filled by the cycle; the apply hop folds it into the pane's ChangeMonitorPaneState.
    ChangeProbeCycleNotes* Notes = nullptr;
};

/// Removes from `diff` the keys the cycle must not act on: remembered refusals leave ToFetch and Added, and
/// rows the hierarchy walk owns leave Vanished.
inline void FilterRosterDiff(RosterDiff& diff, const RosterProbeContext& context) {
    const auto drop = [](std::vector<std::string>& keys, const std::unordered_set<std::string>* skip) {
        if (skip == nullptr || skip->empty()) {
            return;
        }
        keys.erase(std::remove_if(keys.begin(), keys.end(),
                                  [skip](const std::string& key) { return skip->find(key) != skip->end(); }),
                   keys.end());
    };
    drop(diff.ToFetch, context.RefusedKeys);
    drop(diff.Added, context.RefusedKeys);
    drop(diff.Vanished, context.WalkOwned);
}

/// One FetchViewRoster call replaces FetchIssuesChangedSince + FetchIssueKeysForView: diffs the roster against
/// `residentUpdated` (id -> held stamp), downloads only the changed / new bodies and probes the vanished keys.
/// The roster walk shares the RELOAD's caps (a view the reload would truncate must not grow past that here), so
/// a larger view always yields an incomplete roster: that is UseLegacy, and nothing was fetched or probed.
inline RosterProbeOutcome RunRosterProbe(ITrackerIssueReader& reader, const TrackerConfig& cfg, const ViewsStore& views,
                                         const std::unordered_map<std::string, std::string>& residentUpdated,
                                         const std::string& paneId, std::vector<CachedTicket>& outFetched,
                                         std::vector<MembershipRemovalVerdict>& outVerdicts,
                                         const RosterProbeContext& context = RosterProbeContext()) {
    const std::function<bool()> neverCancel = []() { return false; };
    Result<TrackerRoster, TrackerError> roster = reader.FetchViewRoster(cfg, views, neverCancel);
    if (!roster.has_value()) {
        LOG_INFO("ChangeMonitor roster probe skipped pane='%s': %s", paneId.c_str(), roster.error().Detail.c_str());
        return RosterProbeOutcome::Skipped;
    }
    if (!roster.value().Complete) {
        LOG_INFO("ChangeMonitor roster incomplete pane='%s' (%s): using the changed-since path this cycle",
                 paneId.c_str(), roster.value().Warning.c_str());
        if (context.Notes != nullptr) {
            context.Notes->RosterIncomplete = true; // later cycles skip the capped walk until the pane reloads
        }
        return RosterProbeOutcome::UseLegacy;
    }
    RosterDiff diff = DiffRosterAgainstResident(roster.value(), residentUpdated);
    FilterRosterDiff(diff, context);
    if (!diff.ToFetch.empty()) {
        std::vector<std::string> keys = diff.ToFetch;
        if (keys.size() > kMaxMonitorBodiesPerCycle) {
            keys.resize(kMaxMonitorBodiesPerCycle);
            if (context.Notes != nullptr) {
                context.Notes->BodiesCapped = true;
            }
        }
        // A refused key (Jira rejects a whole `key in (...)` batch for ONE unreadable ticket) is isolated and
        // skipped; the rows of every other half are kept, so one refused key cannot fail the cycle forever.
        int budget = RefusalSplitBudget(keys.size());
        std::vector<std::string> refused;
        const std::function<bool()> cancelled = []() { return false; };
        Result<std::vector<CachedTicket>, TrackerError> bodies = FetchKeysIsolatingRefusals(
            [&](const std::vector<std::string>& batch) {
                return reader.FetchIssuesForKeysCancellable(cfg, batch, views, cancelled);
            },
            cancelled, keys, budget, refused);
        if (!bodies.has_value() && IsTrackerRefusal(bodies.error())) {
            // More refused keys than one cycle can isolate: the keyed downloads cannot succeed, and the refused keys
            // are not learned, so every cycle would fail the same way. The changed-since path needs no key list.
            LOG_INFO("ChangeMonitor pane='%s': too many refused keys to isolate (%s): using the changed-since path",
                     paneId.c_str(), bodies.error().Detail.c_str());
            if (context.Notes != nullptr) {
                context.Notes->RosterIncomplete = true;
            }
            return RosterProbeOutcome::UseLegacy;
        }
        if (!bodies.has_value()) {
            LOG_INFO("ChangeMonitor body fetch skipped pane='%s': %s", paneId.c_str(), bodies.error().Detail.c_str());
            return RosterProbeOutcome::Skipped;
        }
        if (!refused.empty()) {
            LOG_INFO("ChangeMonitor pane='%s': %zu key(s) refused by the tracker, skipped until the pane reloads",
                     paneId.c_str(), refused.size());
            if (context.Notes != nullptr) {
                context.Notes->RefusedKeys.insert(context.Notes->RefusedKeys.end(), refused.begin(), refused.end());
            }
        }
        // The tracker answers a `key in (...)` request, but a hostile or buggy server can return more rows than
        // were asked for. Only the requested ids may reach the grid, the cache and the pane's owned-id set.
        const std::unordered_set<std::string> wanted(keys.begin(), keys.end());
        std::unordered_set<std::string> taken;
        outFetched.clear();
        for (CachedTicket& ticket : bodies.value()) {
            if (wanted.count(ticket.id) != 0 && taken.insert(ticket.id).second) {
                outFetched.push_back(std::move(ticket));
            }
        }
    }
    outVerdicts = ProbeVanishedKeys(reader, cfg, diff.Vanished, paneId);
    return RosterProbeOutcome::Applied;
}

/// Legacy membership reconcile: FetchIssueKeysForView (probe caps) diffed against `cachedKeys`, then
/// ProbeIssueExists on each vanished key (capped). A keys-fetch soft failure yields no verdicts this cycle; a
/// probe error is non-destructive. Touches no shared state, so it is safe on the worker thread.
inline std::vector<MembershipRemovalVerdict>
ComputeMembershipRemovals(ITrackerIssueReader& reader, const TrackerConfig& cfg, const ViewsStore& views,
                          const std::vector<std::string>& cachedKeys, const std::string& paneId) {
    auto keysRes = reader.FetchIssueKeysForView(cfg, views);
    if (!static_cast<bool>(keysRes)) {
        LOG_INFO("ChangeMonitor keys-fetch skipped pane='%s': %s", paneId.c_str(), keysRes.error().Detail.c_str());
        return std::vector<MembershipRemovalVerdict>();
    }
    return ProbeVanishedKeys(reader, cfg, RemovedKeys(cachedKeys, keysRes.value()), paneId);
}

/// The worker half of one monitor cycle. `useRoster` tries the roster path first and falls back to the legacy
/// changed-since + keys-only reconcile when the roster is incomplete; `outAddNewRows` says whether rows that
/// entered the view may be added (roster path only: the legacy path never grew the grid). False = soft failure,
/// logged; the caller skips the cycle and leaves its anchor alone. The legacy window is measured from the
/// anchor, which an applied cycle advances unless it reports `BodiesCapped` (a roster cycle that downloaded only
/// part of a larger backlog keeps the anchor, so a later fall-back to the legacy window still covers the rest).
/// `context` carries the pane's remembered refusals and the rows the hierarchy walk owns in, and the cycle's notes out.
inline bool RunChangeProbeCycle(ITrackerIssueReader& reader, const TrackerConfig& cfg, const ViewsStore& views,
                                bool useRoster, const std::unordered_map<std::string, std::string>& residentUpdated,
                                const std::vector<std::string>& cachedKeys, std::chrono::seconds window,
                                const std::vector<std::string>& salientFieldIds, const std::string& paneId,
                                std::vector<CachedTicket>& outFetched,
                                std::vector<MembershipRemovalVerdict>& outVerdicts, bool& outAddNewRows,
                                const RosterProbeContext& context = RosterProbeContext()) {
    outAddNewRows = false;
    if (useRoster) {
        const RosterProbeOutcome outcome =
            RunRosterProbe(reader, cfg, views, residentUpdated, paneId, outFetched, outVerdicts, context);
        if (outcome == RosterProbeOutcome::Skipped) {
            return false;
        }
        if (outcome == RosterProbeOutcome::Applied) {
            outAddNewRows = true;
            return true;
        }
        outFetched.clear();
        outVerdicts.clear();
    }
    auto res = reader.FetchIssuesChangedSince(cfg, views, window, salientFieldIds);
    if (!static_cast<bool>(res)) {
        // Soft failure (partial/warned fetch, transport): the anchor stays so the next interval retries the window.
        LOG_INFO("ChangeMonitor probe skipped pane='%s': %s", paneId.c_str(), res.error().Detail.c_str());
        return false;
    }
    outFetched = std::move(res.value());
    outVerdicts = ComputeMembershipRemovals(reader, cfg, views, cachedKeys, paneId);
    return true;
}

} // namespace sync
} // namespace smatchet
