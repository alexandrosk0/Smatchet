#pragma once

// Worker half of the change monitor's ROSTER path (docs/plans/active/delta-sync.md § Phase 6). Header-only and
// free of AppController state so it is unit-testable against a fake reader.

#include "ITrackerIssueReader.h"
#include "Logger.h"
#include "Sync/DeltaSyncPure.h"
#include "Sync/MembershipDiffPure.h"
#include "Sync/RefusalIsolatingFetchPure.h"
#include "TrackerRoster.h"

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

/// One FetchViewRoster call replaces FetchIssuesChangedSince + FetchIssueKeysForView: diffs the roster against
/// `residentUpdated` (id -> held stamp), downloads only the changed / new bodies and probes the vanished keys.
/// The roster walk shares the RELOAD's caps (a view the reload would truncate must not grow past that here), so
/// a larger view always yields an incomplete roster: that is UseLegacy, and nothing was fetched or probed.
inline RosterProbeOutcome RunRosterProbe(ITrackerIssueReader& reader, const TrackerConfig& cfg, const ViewsStore& views,
                                         const std::unordered_map<std::string, std::string>& residentUpdated,
                                         const std::string& paneId, std::vector<CachedTicket>& outFetched,
                                         std::vector<MembershipRemovalVerdict>& outVerdicts) {
    const std::function<bool()> neverCancel = []() { return false; };
    Result<TrackerRoster, TrackerError> roster = reader.FetchViewRoster(cfg, views, neverCancel);
    if (!roster.has_value()) {
        LOG_INFO("ChangeMonitor roster probe skipped pane='%s': %s", paneId.c_str(), roster.error().Detail.c_str());
        return RosterProbeOutcome::Skipped;
    }
    if (!roster.value().Complete) {
        LOG_INFO("ChangeMonitor roster incomplete pane='%s' (%s): using the changed-since path this cycle",
                 paneId.c_str(), roster.value().Warning.c_str());
        return RosterProbeOutcome::UseLegacy;
    }
    const RosterDiff diff = DiffRosterAgainstResident(roster.value(), residentUpdated);
    if (!diff.ToFetch.empty()) {
        std::vector<std::string> keys = diff.ToFetch;
        if (keys.size() > kMaxMonitorBodiesPerCycle) {
            keys.resize(kMaxMonitorBodiesPerCycle);
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
        if (!bodies.has_value()) {
            LOG_INFO("ChangeMonitor body fetch skipped pane='%s': %s", paneId.c_str(), bodies.error().Detail.c_str());
            return RosterProbeOutcome::Skipped;
        }
        if (!refused.empty()) {
            LOG_INFO("ChangeMonitor pane='%s': %zu key(s) refused by the tracker, skipped this cycle", paneId.c_str(),
                     refused.size());
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
/// anchor, which every applied cycle (either path) advances, so a fallback cycle never misses a change.
inline bool RunChangeProbeCycle(ITrackerIssueReader& reader, const TrackerConfig& cfg, const ViewsStore& views,
                                bool useRoster, const std::unordered_map<std::string, std::string>& residentUpdated,
                                const std::vector<std::string>& cachedKeys, std::chrono::seconds window,
                                const std::vector<std::string>& salientFieldIds, const std::string& paneId,
                                std::vector<CachedTicket>& outFetched,
                                std::vector<MembershipRemovalVerdict>& outVerdicts, bool& outAddNewRows) {
    outAddNewRows = false;
    if (useRoster) {
        const RosterProbeOutcome outcome =
            RunRosterProbe(reader, cfg, views, residentUpdated, paneId, outFetched, outVerdicts);
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
