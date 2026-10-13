#ifndef SMATCHET_SYNC_CHANGE_MONITOR_ROSTER_PROBE_H
#define SMATCHET_SYNC_CHANGE_MONITOR_ROSTER_PROBE_H

// Worker half of the change monitor's ROSTER path (docs/plans/active/delta-sync.md § Phase 6). Header-only and
// free of AppController state so it is unit-testable against a fake reader.

#include "ITrackerIssueReader.h"
#include "Logger.h"
#include "Sync/DeltaSyncPure.h"
#include "Sync/MembershipDiffPure.h"
#include "TrackerRoster.h"

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace smatchet {
namespace sync {

/// Existence probes spent per monitor cycle (the remainder is retried next cycle).
constexpr std::size_t kMaxVanishedProbesPerCycle = 25;
/// Bodies downloaded per monitor cycle; a larger backlog is picked up next cycle (the diff is idempotent).
constexpr std::size_t kMaxMonitorBodiesPerCycle = 200;
/// Extra requests the body download may spend isolating a refused key.
constexpr int kMonitorRefusalSplitBudget = 8;

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

/// Downloads `keys`. A refused multi-key request is split (bounded by `budget`) so one unreadable ticket cannot
/// starve every other row of the pane on every cycle; a key refused on its own is skipped this cycle.
/// Any other failure is returned.
inline Result<std::vector<CachedTicket>, TrackerError>
FetchMonitorBodies(ITrackerIssueReader& reader, const TrackerConfig& cfg, const ViewsStore& views,
                   const std::vector<std::string>& keys, int& budget) {
    using R = Result<std::vector<CachedTicket>, TrackerError>;
    const std::function<bool()> neverCancel = []() { return false; };
    R fetched = reader.FetchIssuesForKeysCancellable(cfg, keys, views, neverCancel);
    if (fetched.has_value() || fetched.error().Kind != TrackerErrorKind::InvalidRequest) {
        return fetched;
    }
    if (keys.size() == 1) {
        return R::Ok(std::vector<CachedTicket>());
    }
    if (budget < 2) {
        return fetched;
    }
    budget -= 2;
    const auto mid = keys.begin() + static_cast<std::ptrdiff_t>(keys.size() / 2);
    R first = FetchMonitorBodies(reader, cfg, views, std::vector<std::string>(keys.begin(), mid), budget);
    if (!first.has_value()) {
        return first;
    }
    R second = FetchMonitorBodies(reader, cfg, views, std::vector<std::string>(mid, keys.end()), budget);
    if (!second.has_value()) {
        return second;
    }
    std::vector<CachedTicket> merged = std::move(first.value());
    for (CachedTicket& t : second.value()) {
        merged.push_back(std::move(t));
    }
    return R::Ok(std::move(merged));
}

/// One FetchViewRoster call replaces FetchIssuesChangedSince + FetchIssueKeysForView: diffs the roster against
/// `residentUpdated` (id -> held stamp), downloads only the changed / new bodies and probes the vanished keys.
/// False = soft failure (logged); the caller skips the cycle and leaves its anchor alone.
inline bool RunRosterProbe(ITrackerIssueReader& reader, const TrackerConfig& cfg, const ViewsStore& views,
                           const std::unordered_map<std::string, std::string>& residentUpdated,
                           const std::string& paneId, std::vector<CachedTicket>& outFetched,
                           std::vector<MembershipRemovalVerdict>& outVerdicts) {
    const std::function<bool()> neverCancel = []() { return false; };
    Result<TrackerRoster, TrackerError> roster = reader.FetchViewRoster(cfg, views, neverCancel);
    if (!roster.has_value()) {
        LOG_INFO("ChangeMonitor roster probe skipped pane='%s': %s", paneId.c_str(), roster.error().Detail.c_str());
        return false;
    }
    const RosterDiff diff = DiffRosterAgainstResident(roster.value(), residentUpdated);
    if (!diff.ToFetch.empty()) {
        std::vector<std::string> keys = diff.ToFetch;
        if (keys.size() > kMaxMonitorBodiesPerCycle) {
            keys.resize(kMaxMonitorBodiesPerCycle);
        }
        int budget = kMonitorRefusalSplitBudget;
        Result<std::vector<CachedTicket>, TrackerError> bodies = FetchMonitorBodies(reader, cfg, views, keys, budget);
        if (!bodies.has_value()) {
            LOG_INFO("ChangeMonitor body fetch skipped pane='%s': %s", paneId.c_str(), bodies.error().Detail.c_str());
            return false;
        }
        outFetched = std::move(bodies.value());
    }
    outVerdicts = ProbeVanishedKeys(reader, cfg, diff.Vanished, paneId);
    return true;
}

} // namespace sync
} // namespace smatchet

#endif // SMATCHET_SYNC_CHANGE_MONITOR_ROSTER_PROBE_H
