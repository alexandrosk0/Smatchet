// ActiveRowMergePure / LocalRefreshGatePure - bucket-A coverage for the apply drain's roster merge (id -> index map
// kept for a whole sync session, version-move reports for cache-served rows) and the ordering gate that stops an
// older local-cache read from replacing a newer one (docs/plans/active/delta-sync.md).

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "Sync/ActiveRowMergePure.h"
#include "Sync/LocalRefreshGatePure.h"

using namespace smatchet::sync;

namespace {

CachedTicket Row(const std::string& id, const std::string& updated, const std::string& summary = "s") {
    CachedTicket t;
    t.id = id;
    t.fieldValues["summary"] = summary;
    t.fieldValues["updated"] = updated;
    return t;
}

} // namespace

TEST_CASE("MergeRowsIntoActive: fresh rows replace or append, cache-served rows only when missing or different") {
    std::vector<CachedTicket> active = {Row("A-1", "2024-05-01T10:00:00.000+0000", "old")};
    ActiveRowIndex index;
    std::vector<std::string> moved;

    const bool changed = MergeRowsIntoActive(active, 0, index, {Row("A-1", "2024-05-01T10:00:00.000+0000", "new")},
                                             {Row("A-2", "T", "added"), Row("A-2", "T", "added")}, moved);

    CHECK(changed);
    REQUIRE(active.size() == 2); // the duplicate cache-served row is merged into the first, not appended twice
    CHECK(active[0].fieldValues.at("summary") == "new");
    CHECK(active[1].id == "A-2");

    std::vector<std::string> movedAgain;
    const bool again = MergeRowsIntoActive(active, 0, index, {}, {Row("A-2", "T", "added")}, movedAgain);
    CHECK_FALSE(again); // an identical cache-served row republishes nothing
}

TEST_CASE("MergeRowsIntoActive: a version move is reported for fresh rows AND for cache-served rows") {
    std::vector<CachedTicket> active = {Row("A-1", "2024-05-01T10:00:00.000+0000"),
                                        Row("A-2", "2024-05-01T10:00:00.000+0000"),
                                        Row("A-3", "2024-05-01T10:00:00.000+0000")};
    ActiveRowIndex index;
    std::vector<std::string> moved;

    MergeRowsIntoActive(active, 0, index, {Row("A-1", "2024-05-01T11:00:00.000+0000")},
                        {Row("A-2", "2024-05-01T11:00:00.000+0000"),  // another process already cached the newer copy
                         Row("A-3", "2024-05-01T12:00:00.000+0200")}, // the same instant as held: not a move (and
                        moved);                                       // the text differs, so the row is replaced)

    CHECK(moved == std::vector<std::string>{"A-1", "A-2"});
}

TEST_CASE("MergeRowsIntoActive: a cache-served row provably older than the resident copy never replaces it") {
    std::vector<CachedTicket> active = {Row("A-1", "2024-05-01T12:00:00.000+0000", "resident, newer")};
    ActiveRowIndex index;
    std::vector<std::string> moved;

    const bool changed =
        MergeRowsIntoActive(active, 0, index, {}, {Row("A-1", "2024-05-01T10:00:00.000+0000", "cached, older")}, moved);

    CHECK_FALSE(changed);
    CHECK(active[0].fieldValues.at("summary") == "resident, newer");
    CHECK(moved.empty());
}

TEST_CASE("RowVersionMoved needs two known stamps naming different instants") {
    CHECK_FALSE(RowVersionMoved(Row("A-1", ""), Row("A-1", "T2")));
    CHECK_FALSE(RowVersionMoved(Row("A-1", "T1"), Row("A-1", "")));
    CHECK_FALSE(RowVersionMoved(Row("A-1", "2024-05-01T10:00:00Z"), Row("A-1", "2024-05-01T10:00:00.000+0000")));
    CHECK(RowVersionMoved(Row("A-1", "2024-05-01T10:00:00Z"), Row("A-1", "2024-05-01T10:00:01Z")));
}

TEST_CASE("ActiveRowIndex is built once while the roster stays as the merge left it") {
    std::vector<CachedTicket> active;
    ActiveRowIndex index;
    std::vector<std::string> moved;
    std::uint64_t revision = 0;

    for (int frame = 0; frame < 5; ++frame) {
        std::vector<CachedTicket> rows;
        for (int i = 0; i < 20; ++i) {
            rows.push_back(Row("K-" + std::to_string(frame * 20 + i), "T"));
        }
        MergeRowsIntoActive(active, revision, index, rows, {}, moved);
        ++revision;                // the caller bumps the revision for a change ...
        index.Revision = revision; // ... and records the value its own bump produced
    }

    CHECK(active.size() == 100);
    CHECK(index.Builds == 1);
}

TEST_CASE("ActiveRowIndex rebuilds when the roster changed behind it, and a stale hit never misplaces a row") {
    std::vector<CachedTicket> active = {Row("A-1", "T"), Row("A-2", "T"), Row("A-3", "T")};
    ActiveRowIndex index;
    std::vector<std::string> moved;
    MergeRowsIntoActive(active, 5, index, {}, {Row("A-1", "T")}, moved);
    REQUIRE(index.Builds == 1);

    // Another path replaced the roster wholesale (same size, other order) and bumped the revision.
    active = {Row("A-3", "T"), Row("A-1", "T"), Row("A-9", "T")};
    MergeRowsIntoActive(active, 6, index, {Row("A-9", "T", "fresh")}, {}, moved);
    CHECK(index.Builds == 2);
    CHECK(active.size() == 3); // A-9 replaced in place, not appended
    CHECK(active[2].fieldValues.at("summary") == "fresh");

    // A change the revision did not announce (same revision, same size, other ids): the hit is verified.
    active = {Row("B-1", "T"), Row("B-2", "T"), Row("B-3", "T")};
    index.Revision = 6;
    index.Rows = 3;
    MergeRowsIntoActive(active, 6, index, {Row("A-3", "T", "x")}, {}, moved);
    CHECK(index.Builds == 3);  // A-3 pointed at a row with another id: rebuilt
    CHECK(active.size() == 4); // and A-3 is genuinely new
}

TEST_CASE("ActiveRowIndex::Release drops the map and forces a rebuild") {
    std::vector<CachedTicket> active = {Row("A-1", "T")};
    ActiveRowIndex index;
    std::vector<std::string> moved;
    MergeRowsIntoActive(active, 0, index, {}, {}, moved);
    REQUIRE(index.Valid);
    index.Release();
    CHECK_FALSE(index.Valid);
    CHECK(index.Positions.empty());
    MergeRowsIntoActive(active, 0, index, {}, {}, moved);
    CHECK(index.Builds == 2);
}

TEST_CASE("LocalRefreshGate: a refresh that started earlier never replaces one that started later and landed first") {
    LocalRefreshGate gate;
    const std::uint64_t older = gate.Begin();
    const std::uint64_t newer = gate.Begin();

    CHECK(gate.ClaimWholesale(newer));
    CHECK_FALSE(gate.ClaimWholesale(older));
    const std::uint64_t later = gate.Begin();
    CHECK(gate.ClaimWholesale(later));
}

TEST_CASE("LocalRefreshGate: refreshes that land in start order all apply") {
    LocalRefreshGate gate;
    const std::uint64_t first = gate.Begin();
    const std::uint64_t second = gate.Begin();
    CHECK(gate.ClaimWholesale(first));
    CHECK(gate.ClaimWholesale(second));
}

TEST_CASE("AdmitRowIfMissing: a refresh that lost the race still owes the row its caller just saved") {
    std::vector<CachedTicket> active = {Row("A-1", "T")};
    const std::vector<CachedTicket> read = {Row("A-1", "T0", "older"), Row("A-2", "T", "saved")};

    CHECK(AdmitRowIfMissing(active, read, "A-2"));
    REQUIRE(active.size() == 2);
    CHECK(active[1].id == "A-2");

    CHECK_FALSE(AdmitRowIfMissing(active, read, "A-2")); // already there now
    CHECK_FALSE(AdmitRowIfMissing(active, read, "A-1")); // the newer read's copy stays
    CHECK(active[0].fieldValues.at("summary") == "s");
    CHECK_FALSE(AdmitRowIfMissing(active, read, ""));
    CHECK_FALSE(AdmitRowIfMissing(active, read, "Z-9")); // not in the read either
}
