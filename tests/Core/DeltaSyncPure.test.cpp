// DeltaSyncPure - bucket-A coverage for the delta reload's decisions (Source/Core/include/Sync/
// DeltaSyncPure.h, docs/plans/active/delta-sync.md § Phase 3): which strategy runs, which rows are
// downloaded, when a mostly-changed view falls back to a full fetch, and how queued requests combine.

#include <doctest/doctest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "Sync/DeltaSyncPure.h"

using namespace smatchet::sync;

namespace {

TicketSyncMeta Meta(const std::string& updated, std::vector<std::string> fields, std::int64_t at = 1000) {
    TicketSyncMeta m;
    m.RemoteUpdated = updated;
    m.Fields = NormalizeFieldList(std::move(fields));
    m.SyncedAtEpochSec = at;
    return m;
}

TrackerRosterEntry R(const std::string& key, const std::string& updated) {
    TrackerRosterEntry e;
    e.Key = key;
    e.Updated = updated;
    return e;
}

SyncStrategyInput Ready() {
    SyncStrategyInput in;
    in.Requested = SyncMode::Delta;
    in.DeltaEnabled = true;
    in.BackendSupportsRoster = true;
    in.HaveAnyCachedMeta = true;
    return in;
}

} // namespace

TEST_CASE("ChooseSyncStrategy runs Delta only when asked, enabled, supported and the cache knows versions") {
    SyncStrategyChoice c = ChooseSyncStrategy(Ready());
    CHECK(c.Mode == SyncMode::Delta);
    CHECK(c.FallbackReason.empty());
}

TEST_CASE("ChooseSyncStrategy: a forced Full is Full with no fallback reason") {
    SyncStrategyInput in = Ready();
    in.Requested = SyncMode::Full;
    const SyncStrategyChoice c = ChooseSyncStrategy(in);
    CHECK(c.Mode == SyncMode::Full);
    CHECK(c.FallbackReason.empty()); // not a fallback: the caller asked for it
}

TEST_CASE("ChooseSyncStrategy: each missing precondition falls back to Full with a reason") {
    {
        SyncStrategyInput in = Ready();
        in.DeltaEnabled = false;
        const SyncStrategyChoice c = ChooseSyncStrategy(in);
        CHECK(c.Mode == SyncMode::Full);
        CHECK_FALSE(c.FallbackReason.empty());
    }
    {
        SyncStrategyInput in = Ready();
        in.BackendSupportsRoster = false;
        const SyncStrategyChoice c = ChooseSyncStrategy(in);
        CHECK(c.Mode == SyncMode::Full);
        CHECK_FALSE(c.FallbackReason.empty());
    }
    {
        SyncStrategyInput in = Ready();
        in.HaveAnyCachedMeta = false;
        const SyncStrategyChoice c = ChooseSyncStrategy(in);
        CHECK(c.Mode == SyncMode::Full);
        CHECK_FALSE(c.FallbackReason.empty());
    }
}

TEST_CASE("CombineSyncModes: a queued Full is never downgraded") {
    CHECK(CombineSyncModes(SyncMode::Delta, SyncMode::Delta) == SyncMode::Delta);
    CHECK(CombineSyncModes(SyncMode::Delta, SyncMode::Full) == SyncMode::Full);
    CHECK(CombineSyncModes(SyncMode::Full, SyncMode::Delta) == SyncMode::Full);
    CHECK(CombineSyncModes(SyncMode::Full, SyncMode::Full) == SyncMode::Full);
}

TEST_CASE("ComputeDeltaFetchSet: an unchanged, covered, fresh row is not fetched") {
    std::unordered_map<std::string, TicketSyncMeta> meta;
    meta["A-1"] = Meta("T1", {"summary", "status", "updated"});
    const DeltaFetchSet set =
        ComputeDeltaFetchSet({R("A-1", "T1")}, meta, RequiredFieldsOfView({"summary", "status"}), 2000, 21600);
    CHECK(set.ToFetch.empty());
    CHECK(set.Unchanged == std::vector<std::string>{"A-1"});
}

TEST_CASE("ComputeDeltaFetchSet: new, moved, unknown and unstated versions are fetched") {
    std::unordered_map<std::string, TicketSyncMeta> meta;
    meta["MOVED"] = Meta("T1", {"summary"});
    meta["NOSTAMP"] = Meta("", {"summary"});
    meta["ROSTERBLANK"] = Meta("T1", {"summary"});
    meta["SAME"] = Meta("T1", {"summary"});
    const DeltaFetchSet set = ComputeDeltaFetchSet(
        {R("NEW", "T1"), R("MOVED", "T2"), R("NOSTAMP", "T1"), R("ROSTERBLANK", ""), R("SAME", "T1")}, meta, {}, 2000,
        0);
    CHECK(set.ToFetch == std::vector<std::string>{"NEW", "MOVED", "NOSTAMP", "ROSTERBLANK"});
    CHECK(set.Unchanged == std::vector<std::string>{"SAME"});
}

TEST_CASE("ComputeDeltaFetchSet: a row fetched without a column the view needs is fetched again") {
    std::unordered_map<std::string, TicketSyncMeta> meta;
    meta["A-1"] = Meta("T1", {"summary", "updated"});
    const DeltaFetchSet needsLabels =
        ComputeDeltaFetchSet({R("A-1", "T1")}, meta, RequiredFieldsOfView({"summary", "labels"}), 2000, 0);
    CHECK(needsLabels.ToFetch == std::vector<std::string>{"A-1"});
    const DeltaFetchSet fine = ComputeDeltaFetchSet({R("A-1", "T1")}, meta, RequiredFieldsOfView({"summary"}), 2000, 0);
    CHECK(fine.ToFetch.empty());
}

TEST_CASE("ComputeDeltaFetchSet: the max-row-age bound refetches old rows; 0 disables it") {
    std::unordered_map<std::string, TicketSyncMeta> meta;
    meta["OLD"] = Meta("T1", {"summary"}, 1000);
    meta["FRESH"] = Meta("T1", {"summary"}, 100000);
    meta["NEVER"] = Meta("T1", {"summary"}, 0);
    const std::vector<TrackerRosterEntry> roster = {R("OLD", "T1"), R("FRESH", "T1"), R("NEVER", "T1")};
    const DeltaFetchSet bounded = ComputeDeltaFetchSet(roster, meta, {}, 100500, 21600);
    CHECK(bounded.ToFetch == std::vector<std::string>{"OLD", "NEVER"});
    CHECK(bounded.Unchanged == std::vector<std::string>{"FRESH"});
    const DeltaFetchSet unbounded = ComputeDeltaFetchSet(roster, meta, {}, 100500, 0);
    CHECK(unbounded.ToFetch.empty());
}

TEST_CASE("ComputeDeltaFetchSet: duplicate and blank roster keys are listed once / skipped") {
    std::unordered_map<std::string, TicketSyncMeta> meta;
    const DeltaFetchSet set = ComputeDeltaFetchSet({R("A-1", "T1"), R("A-1", "T1"), R("", "T1")}, meta, {}, 1, 0);
    CHECK(set.ToFetch == std::vector<std::string>{"A-1"});
}

TEST_CASE("ShouldPreferFullFetch needs both more than half AND more than 100 rows changed") {
    CHECK_FALSE(ShouldPreferFullFetch(1000, 100)); // not above the absolute floor
    CHECK_FALSE(ShouldPreferFullFetch(1000, 500)); // exactly half
    CHECK(ShouldPreferFullFetch(1000, 501));
    CHECK_FALSE(ShouldPreferFullFetch(150, 101 - 1));
    CHECK(ShouldPreferFullFetch(150, 101)); // 101 of 150 is over half and over the floor
    CHECK_FALSE(ShouldPreferFullFetch(10000, 101));
    CHECK_FALSE(ShouldPreferFullFetch(0, 0));
}

TEST_CASE("RequiredFieldsOfView drops blanks and the synthetic id column and sorts") {
    CHECK(RequiredFieldsOfView({"summary", "", "id", "assignee", "summary"}) ==
          std::vector<std::string>{"assignee", "summary"});
}
