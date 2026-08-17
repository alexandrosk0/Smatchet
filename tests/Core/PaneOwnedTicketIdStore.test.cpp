// PaneOwnedTicketIdStore.test.cpp — the per-pane owned-ticket-id book-keeping behind
// AppController's multi-pane cache scoping (pane-stale-delete-collision F4), pinned at the
// header-only seam because the real AppController cannot be constructed in-process (cpr / SQLite /
// ImGui closure). Three regressions live here:
//
//   * #2049 — a pane that switched tracker kind filed its set under the OLD backend key, and
//             retirement erased the NEW one, leaving an orphan pinning tickets_v2 rows forever.
//   * #2050 — the admit path did a non-atomic read-modify-write, so a PublishOwnedTicketIds
//             landing mid-window was clobbered back to the pre-sync ids.
//   * #2063 — an empty recorded set meant BOTH "never synced" and "forgotten at retirement", and
//             only the first justifies the namespace-wide cold-start fallback; a revived pane
//             therefore re-leaked every sibling pane's rows into its grid.

#include "Sync/PaneOwnedTicketIdStore.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

using smatchet::PaneOwnedTicketIdStore;

namespace {

std::vector<std::string> Sorted(std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

bool Contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

TEST_CASE("MakeKey joins backend key and pane id unambiguously") {
    CHECK(PaneOwnedTicketIdStore::MakeKey("jira", "pane-1") == std::string("jira\npane-1"));
    // Ordered by backend key first — what lets the retention sweep prefix-scan one namespace.
    CHECK(PaneOwnedTicketIdStore::MakeKey("github", "p") < PaneOwnedTicketIdStore::MakeKey("jira", "a"));
}

TEST_CASE("TryGet distinguishes never-recorded from recorded (issue #2063)") {
    PaneOwnedTicketIdStore store;
    std::vector<std::string> out;
    // Absent: the caller falls back to the whole namespace (the bootstrap cold-start seed).
    CHECK_FALSE(store.TryGet("jira", "pane-1", out));
    CHECK(out.empty());

    store.Set("jira", "pane-1", {"T-1", "T-2"});
    CHECK(store.TryGet("jira", "pane-1", out));
    CHECK(Sorted(out) == std::vector<std::string>({"T-1", "T-2"}));

    // An empty pane id is never keyable — Set is a no-op and TryGet reports absent.
    store.Set("jira", "", {"T-9"});
    CHECK_FALSE(store.TryGet("jira", "", out));
}

TEST_CASE("Forget tombstones rather than erases, so a revived pane renders empty (issue #2063)") {
    PaneOwnedTicketIdStore store;
    store.Set("jira", "pane-1", {"T-1", "T-2"});
    store.Forget("pane-1");

    std::vector<std::string> out;
    // Still PRESENT (true) but empty: "this pane was retired", not "this pane never synced". The
    // refresh therefore filters to nothing instead of seeding from every sibling pane's rows.
    CHECK(store.TryGet("jira", "pane-1", out));
    CHECK(out.empty());
    // And it pins nothing against a sibling's stale sweep.
    CHECK(store.CollectRetainedByOtherPanes("jira", "pane-2").empty());
}

TEST_CASE("Forget erases across EVERY backend key the pane ever published under (issue #2049)") {
    PaneOwnedTicketIdStore store;
    // The pane synced under Jira, then SwapBackendIfTrackerChanged re-stamped its key to GitHub
    // and it synced again. Two entries exist; retirement reads only the key live at that moment.
    store.Set("jira", "pane-1", {"JIRA-1"});
    store.Set("github", "pane-1", {"GH-1"});
    store.Set("jira", "pane-2", {"JIRA-SIBLING"});

    store.Forget("pane-1"); // keyed on the pane id alone

    // Pre-fix, the "jira" entry survived and pinned JIRA-1 against pane-2's sweep forever.
    CHECK(store.CollectRetainedByOtherPanes("jira", "pane-2").empty());
    CHECK(store.CollectRetainedByOtherPanes("github", "pane-2").empty());
    // The sibling's own set is untouched.
    CHECK(store.CollectRetainedByOtherPanes("jira", "pane-1") == std::vector<std::string>({"JIRA-SIBLING"}));
}

TEST_CASE("Forget matches the pane id exactly, never as a bare suffix") {
    PaneOwnedTicketIdStore store;
    store.Set("jira", "ab", {"T-AB"});
    store.Set("jira", "b", {"T-B"});

    store.Forget("b");

    std::vector<std::string> out;
    CHECK(store.TryGet("jira", "b", out));
    CHECK(out.empty());
    // "ab" ends in "b" but the '\n' separator does not line up — it must survive intact.
    CHECK(store.TryGet("jira", "ab", out));
    CHECK(out == std::vector<std::string>({"T-AB"}));
}

TEST_CASE("Add appends atomically, so a concurrent publish is extended not clobbered (issue #2050)") {
    PaneOwnedTicketIdStore store;
    store.Set("jira", "pane-1", {"OLD-1"});

    // Reproduce the racing window exactly: the refresh snapshots the set, a sync worker publishes
    // a fresh roster into the window, and only THEN is the admitted row recorded.
    std::vector<std::string> snapshotTakenEarly;
    CHECK(store.TryGet("jira", "pane-1", snapshotTakenEarly));
    store.Set("jira", "pane-1", {"SYNCED-1", "SYNCED-2"}); // the mid-window PublishOwnedTicketIds
    CHECK(store.Add("jira", "pane-1", "ADMITTED"));

    std::vector<std::string> out;
    CHECK(store.TryGet("jira", "pane-1", out));
    // Pre-fix this wrote back snapshotTakenEarly + ADMITTED, reverting the pane to its pre-sync
    // ids — the freshly-synced rows were then filtered out of the grid and unpinned.
    CHECK(Sorted(out) == std::vector<std::string>({"ADMITTED", "SYNCED-1", "SYNCED-2"}));
    CHECK_FALSE(Contains(out, "OLD-1"));
    CHECK(snapshotTakenEarly == std::vector<std::string>({"OLD-1"})); // the stale copy is never written back
}

TEST_CASE("Add is a no-op on an unrecorded pane and on an id already recorded") {
    PaneOwnedTicketIdStore store;
    std::vector<std::string> out;
    // A pane that has never synced has no set to admit into — creating one here would flip it out
    // of the cold-start fallback on the strength of a single edited row.
    CHECK_FALSE(store.Add("jira", "pane-1", "T-1"));
    CHECK_FALSE(store.TryGet("jira", "pane-1", out));

    store.Set("jira", "pane-1", {"T-1"});
    CHECK_FALSE(store.Add("jira", "pane-1", "T-1")); // already there — no duplicate
    CHECK_FALSE(store.Add("jira", "pane-1", ""));    // empty id is not an id
    CHECK_FALSE(store.Add("jira", "", "T-2"));       // unkeyable pane
    CHECK(store.TryGet("jira", "pane-1", out));
    CHECK(out == std::vector<std::string>({"T-1"}));

    // A tombstoned (retired-and-revived) pane DOES have a set, so an admit lands in it.
    store.Forget("pane-1");
    CHECK(store.Add("jira", "pane-1", "T-2"));
    CHECK(store.TryGet("jira", "pane-1", out));
    CHECK(out == std::vector<std::string>({"T-2"}));
}

TEST_CASE("Drop subtracts ids from one pane's set without disturbing the others") {
    PaneOwnedTicketIdStore store;
    store.Set("jira", "pane-1", {"T-1", "T-2", "T-3"});
    store.Set("jira", "pane-2", {"T-2"});

    store.Drop("jira", "pane-1", {"T-2", "MISSING"});

    std::vector<std::string> out;
    CHECK(store.TryGet("jira", "pane-1", out));
    CHECK(out == std::vector<std::string>({"T-1", "T-3"}));
    CHECK(store.TryGet("jira", "pane-2", out));
    CHECK(out == std::vector<std::string>({"T-2"})); // a sibling still retains it
    store.Drop("jira", "pane-1", {});                // empty drop list is a no-op
    CHECK(store.TryGet("jira", "pane-1", out));
    CHECK(out == std::vector<std::string>({"T-1", "T-3"}));
}

TEST_CASE("CollectRetainedByOtherPanes unions siblings only, scoped to one cache namespace") {
    PaneOwnedTicketIdStore store;
    store.Set("jira", "pane-1", {"SELF-1"});
    store.Set("jira", "pane-2", {"SIB-1"});
    store.Set("jira", "pane-3", {"SIB-2"});
    store.Set("github", "pane-9", {"OTHER-NS"}); // a different backend-keyed namespace

    const std::vector<std::string> retained = Sorted(store.CollectRetainedByOtherPanes("jira", "pane-1"));
    CHECK(retained == std::vector<std::string>({"SIB-1", "SIB-2"}));
    CHECK_FALSE(Contains(retained, "SELF-1"));   // never self-retaining
    CHECK_FALSE(Contains(retained, "OTHER-NS")); // stale-deletion is scoped by backend key
}

TEST_CASE("Erase removes the tombstone so a RECYCLED pane id starts cold (issue #2075 follow-up)") {
    // GenerateUniquePaneId hands back the lowest unused id, so closing pane-2 and clicking "+"
    // mints "pane-2" again. Retirement leaves a tombstone by design (#2063); without an erase on
    // creation the NEW pane inherits it, TryGet answers "recorded, empty", and the pane is denied
    // its cold-start seed — permanently, if no non-empty full sync ever completes (offline, or
    // tracker down, or the kick discarded by a backend swap).
    smatchet::PaneOwnedTicketIdStore store;
    store.Set("Jira", "pane-2", {"A", "B"});
    store.Forget("pane-2"); // retirement: tombstone, present-but-empty

    std::vector<std::string> ids;
    REQUIRE(store.TryGet("Jira", "pane-2", ids)); // tombstone is present...
    CHECK(ids.empty());                           // ...and empty

    store.Erase("pane-2"); // pane creation on the recycled id
    ids.assign(1, std::string("stale"));
    CHECK_FALSE(store.TryGet("Jira", "pane-2", ids)); // absent => cold start, seed allowed
}

TEST_CASE("Erase spans every backend key the pane used, and touches no other pane") {
    // Same cross-key reasoning as Forget (#2049): a pane that switched tracker kind filed sets
    // under more than one key, and leaving any of them behind reintroduces the shadowing.
    smatchet::PaneOwnedTicketIdStore store;
    store.Set("Jira", "pane-2", {"A"});
    store.Set("GitHub", "pane-2", {"B"});
    store.Set("Jira", "pane-3", {"C"});

    store.Erase("pane-2");

    std::vector<std::string> ids;
    CHECK_FALSE(store.TryGet("Jira", "pane-2", ids));
    CHECK_FALSE(store.TryGet("GitHub", "pane-2", ids));
    REQUIRE(store.TryGet("Jira", "pane-3", ids)); // sibling untouched
    CHECK(ids == std::vector<std::string>{"C"});
}

TEST_CASE("Erase matches the pane id exactly, never as a bare suffix") {
    smatchet::PaneOwnedTicketIdStore store;
    store.Set("Jira", "ab", {"A"});
    store.Erase("b");

    std::vector<std::string> ids;
    REQUIRE(store.TryGet("Jira", "ab", ids)); // "b" must not match "ab"
    CHECK(ids == std::vector<std::string>{"A"});
}
