#pragma once

// DeltaSyncPure - the decisions of the delta reload, free of I/O so every branch is unit-testable
// (docs/plans/active/delta-sync.md § Phase 3). Header-only, C++14, no platform headers; compiles in
// the standalone and DX12 targets and in the doctest rig. The version/field rules shared with the
// cache layer live in TicketSyncMeta.h (re-exported here).

#include "Sync/SyncMode.h"
#include "TicketSyncMeta.h"
#include "TrackerRoster.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace smatchet {
namespace sync {

// ---- Strategy -------------------------------------------------------------------------------

struct SyncStrategyInput {
    SyncMode Requested = SyncMode::Delta;
    bool DeltaEnabled = true;           ///< TrackerConfig::DeltaSyncEnabled (the kill switch)
    bool BackendSupportsRoster = false; ///< ITrackerIssueReader::SupportsVersionedRoster()
    bool HaveAnyCachedMeta = false;     ///< the cache holds at least one row with a known version
};

struct SyncStrategyChoice {
    SyncMode Mode = SyncMode::Full;
    /// Why Delta was not chosen; empty when it was, or when Full was simply requested.
    std::string FallbackReason;
};

/// Delta only when it was asked for, is enabled, the backend can list versions and the cache knows
/// at least one. Everything else is today's full fetch.
inline SyncStrategyChoice ChooseSyncStrategy(const SyncStrategyInput& in) {
    SyncStrategyChoice choice;
    if (in.Requested == SyncMode::Full) {
        return choice; // forced: not a fallback, nothing to report
    }
    if (!in.DeltaEnabled) {
        choice.FallbackReason = "delta sync is disabled";
    } else if (!in.BackendSupportsRoster) {
        choice.FallbackReason = "the tracker cannot list ticket versions";
    } else if (!in.HaveAnyCachedMeta) {
        choice.FallbackReason = "no cached ticket versions yet";
    } else {
        choice.Mode = SyncMode::Delta;
    }
    return choice;
}

// ---- Which rows to download -------------------------------------------------------------------

struct DeltaFetchSet {
    /// Keys whose body must be downloaded, in roster order.
    std::vector<std::string> ToFetch;
    /// Keys whose cached row is current, in roster order.
    std::vector<std::string> Unchanged;
};

/// Splits the roster. A key is fetched when the cache has no version for it, the tracker did not state one,
/// the version moved, the cached row was fetched without a field `requiredFields` needs, or it is older than
/// `maxRowAgeSec` (0 disables the age bound) - the age bound exists because data copied from OTHER issues
/// (parent summary, link status, sprint, display names) changes without moving this ticket's own stamp.
/// Duplicate roster keys are listed once.
inline DeltaFetchSet ComputeDeltaFetchSet(const std::vector<TrackerRosterEntry>& roster,
                                          const std::unordered_map<std::string, TicketSyncMeta>& meta,
                                          const std::vector<std::string>& requiredFields, std::int64_t nowEpochSec,
                                          std::int64_t maxRowAgeSec) {
    DeltaFetchSet set;
    std::unordered_set<std::string> seen;
    for (const TrackerRosterEntry& entry : roster) {
        if (entry.Key.empty() || !seen.insert(entry.Key).second) {
            continue;
        }
        const auto it = meta.find(entry.Key);
        bool fetch = it == meta.end();
        if (!fetch) {
            const TicketSyncMeta& cached = it->second;
            fetch = cached.RemoteUpdated.empty() || entry.Updated.empty() || cached.RemoteUpdated != entry.Updated ||
                    !FieldsSatisfy(cached.Fields, requiredFields) ||
                    (maxRowAgeSec > 0 &&
                     (cached.SyncedAtEpochSec <= 0 || nowEpochSec - cached.SyncedAtEpochSec > maxRowAgeSec));
        }
        (fetch ? set.ToFetch : set.Unchanged).push_back(entry.Key);
    }
    return set;
}

/// When most of the view changed, a roster round trip plus keyed downloads costs more than one streamed fetch:
/// switch to Full once more than half the rows AND more than `kDeltaFullFetchMinChanged` rows are stale.
constexpr std::size_t kDeltaFullFetchMinChanged = 100;

inline bool ShouldPreferFullFetch(std::size_t rosterSize, std::size_t changedCount) {
    return changedCount > kDeltaFullFetchMinChanged && changedCount * 2 > rosterSize;
}

// ---- Change monitor -----------------------------------------------------------------------------

struct RosterDiff {
    /// Keys whose body the monitor must download: new to the pane, or whose stamp moved. Roster order.
    std::vector<std::string> ToFetch;
    /// The subset of ToFetch the pane does not hold yet (they join the grid).
    std::vector<std::string> Added;
    /// Resident keys the roster no longer lists - only reported for a COMPLETE roster (a capped walk cannot
    /// prove a row left the view). Sorted, for a deterministic existence-probe order.
    std::vector<std::string> Vanished;
};

/// Compares a roster with what a pane holds (`resident`: id -> the stamp of the held row). A held row is
/// refetched only when both stamps are known and differ, or when the held row has no stamp yet (once; the
/// refetched row carries one). A roster entry without a stamp never triggers a fetch on its own.
inline RosterDiff DiffRosterAgainstResident(const TrackerRoster& roster,
                                            const std::unordered_map<std::string, std::string>& resident) {
    RosterDiff diff;
    std::unordered_set<std::string> listed;
    for (const TrackerRosterEntry& entry : roster.Entries) {
        if (entry.Key.empty() || !listed.insert(entry.Key).second) {
            continue;
        }
        const auto it = resident.find(entry.Key);
        if (it == resident.end()) {
            diff.ToFetch.push_back(entry.Key);
            diff.Added.push_back(entry.Key);
        } else if (!entry.Updated.empty() && it->second != entry.Updated) {
            diff.ToFetch.push_back(entry.Key);
        }
    }
    if (roster.Complete) {
        for (const auto& kv : resident) {
            if (listed.find(kv.first) == listed.end()) {
                diff.Vanished.push_back(kv.first);
            }
        }
        std::sort(diff.Vanished.begin(), diff.Vanished.end());
    }
    return diff;
}

/// The fields a view needs a cached row to carry: its column ids, minus blanks and the synthetic "id".
inline std::vector<std::string> RequiredFieldsOfView(const std::vector<std::string>& viewFields) {
    std::vector<std::string> required;
    for (const std::string& field : viewFields) {
        if (!field.empty() && field != "id") {
            required.push_back(field);
        }
    }
    return NormalizeFieldList(std::move(required));
}

} // namespace sync
} // namespace smatchet
