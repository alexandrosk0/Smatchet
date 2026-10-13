#pragma once

// DeltaSyncPure - the decisions of the delta reload, free of I/O so every branch is unit-testable
// (docs/plans/active/delta-sync.md § Phase 3). Header-only, C++14, no platform headers; compiles in
// the standalone and DX12 targets and in the doctest rig. The version/field rules shared with the
// cache layer live in TicketSyncMeta.h (re-exported here).

#include "Sync/SyncMode.h"
#include "TicketSyncMeta.h"
#include "TrackerRoster.h"

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
