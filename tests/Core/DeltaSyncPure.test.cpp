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

// --- change monitor: roster vs what the pane holds -------------------------------------------------------

TEST_CASE("DiffRosterAgainstResident: changed and new keys are fetched, new ones are Added") {
    TrackerRoster roster;
    roster.Complete = true;
    roster.Entries = {R("A-1", "T1"), R("A-2", "T2"), R("A-3", "T1")};
    const std::unordered_map<std::string, std::string> resident = {{"A-1", "T1"}, {"A-2", "T1"}};
    const RosterDiff diff = DiffRosterAgainstResident(roster, resident);
    CHECK(diff.ToFetch == std::vector<std::string>{"A-2", "A-3"});
    CHECK(diff.Added == std::vector<std::string>{"A-3"});
    CHECK(diff.Vanished.empty());
}

TEST_CASE("DiffRosterAgainstResident: resident keys missing from a COMPLETE roster vanished; an incomplete one reports "
          "none") {
    TrackerRoster roster;
    roster.Entries = {R("A-1", "T1")};
    const std::unordered_map<std::string, std::string> resident = {{"A-1", "T1"}, {"B-2", "T1"}, {"A-9", "T1"}};
    roster.Complete = true;
    CHECK(DiffRosterAgainstResident(roster, resident).Vanished == std::vector<std::string>{"A-9", "B-2"});
    roster.Complete = false;
    CHECK(DiffRosterAgainstResident(roster, resident).Vanished.empty());
}

TEST_CASE("DiffRosterAgainstResident: an unstamped roster entry never triggers a fetch for a held row") {
    TrackerRoster roster;
    roster.Complete = true;
    roster.Entries = {R("A-1", "")};
    const RosterDiff held = DiffRosterAgainstResident(roster, {{"A-1", "T1"}});
    CHECK(held.ToFetch.empty());
    // A held row with no stamp yet is refetched once the tracker states one (the refetched row carries it).
    roster.Entries = {R("A-1", "T1")};
    CHECK(DiffRosterAgainstResident(roster, {{"A-1", ""}}).ToFetch == std::vector<std::string>{"A-1"});
}

TEST_CASE("DiffRosterAgainstResident: duplicate roster keys are handled once") {
    TrackerRoster roster;
    roster.Complete = true;
    roster.Entries = {R("A-1", "T1"), R("A-1", "T1")};
    CHECK(DiffRosterAgainstResident(roster, {}).ToFetch == std::vector<std::string>{"A-1"});
}

TEST_CASE("ComputeDeltaFetchSet: two renderings of one instant are the same version, a later instant is not") {
    std::unordered_map<std::string, TicketSyncMeta> meta;
    meta["A-1"] = Meta("2024-05-01T10:00:00.000+0000", {"summary"});
    meta["A-2"] = Meta("2024-05-01T10:00:00.000+0000", {"summary"});
    meta["A-3"] = Meta("2024-05-01T10:00:00.000+0000", {"summary"});
    const std::vector<TrackerRosterEntry> roster = {
        R("A-1", "2024-05-01T12:00:00.000+0200"), // the same instant in another zone: unchanged
        R("A-2", "2024-05-01T10:00:00Z"),         // the same instant, UTC spelling: unchanged
        R("A-3", "2024-05-01T10:00:01.000+0000")  // one second later: moved
    };

    const DeltaFetchSet set = ComputeDeltaFetchSet(roster, meta, {"summary"}, 2000, 0);

    CHECK(set.Unchanged == std::vector<std::string>{"A-1", "A-2"});
    CHECK(set.ToFetch == std::vector<std::string>{"A-3"});
}

TEST_CASE("DiffRosterAgainstResident: a re-rendered stamp is not a change, a different instant is") {
    TrackerRoster roster;
    roster.Complete = true;
    roster.Entries = {R("A-1", "2024-05-01T12:00:00.000+0200"), R("A-2", "2024-05-01T12:00:05.000+0200")};
    const std::unordered_map<std::string, std::string> resident = {{"A-1", "2024-05-01T10:00:00.000+0000"},
                                                                   {"A-2", "2024-05-01T10:00:00.000+0000"}};

    const RosterDiff diff = DiffRosterAgainstResident(roster, resident);

    CHECK(diff.ToFetch == std::vector<std::string>{"A-2"});
    CHECK(diff.Added.empty());
}

TEST_CASE("RequiredFieldsOfView trims padded ids so a hand-edited view is still covered by a clean row") {
    const std::vector<std::string> required = RequiredFieldsOfView({" summary", "status\t", "  ", " id ", "labels"});
    CHECK(required == std::vector<std::string>{"labels", "status", "summary"});
    CHECK(FieldsSatisfy(NormalizeFieldList({"summary", "status", "labels"}), required));
}

TEST_CASE("AnyRosterKeyHasCachedMeta: a sibling pane's cached versions do not count for this view") {
    std::unordered_map<std::string, TicketSyncMeta> meta;
    meta["OTHER-1"] = Meta("T1", {"summary"});
    meta["BLANK-1"] = Meta("", {"summary"}); // a row cached with no known version

    CHECK_FALSE(AnyRosterKeyHasCachedMeta({R("A-1", "T1"), R("BLANK-1", "T1")}, meta));
    CHECK(AnyRosterKeyHasCachedMeta({R("A-1", "T1"), R("OTHER-1", "T1")}, meta));
    CHECK_FALSE(AnyRosterKeyHasCachedMeta({}, meta));
    CHECK_FALSE(AnyRosterKeyHasCachedMeta({R("", "T1")}, meta));
}
