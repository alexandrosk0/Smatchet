// Bucket-A coverage for ToastHistoryPure (ticket-change-monitor plan, S2 / § Files-to-modify
// #13) — the bounded-ring append the toast manager records every Push into. Pure STL (no
// ImGui), so the eviction + order + row-action retention are testable without a UI session.

#include "Ui/ToastHistoryPure.h"
#include "Ui/UndoLedgerPure.h"

#include "doctest/doctest.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

ToastHistoryEntry Entry(const std::string& title) {
    ToastHistoryEntry e;
    e.Title = title;
    e.Type = ToastType::Info;
    return e;
}

} // namespace

TEST_CASE("PushBoundedHistory: appends in order while under cap") {
    std::vector<ToastHistoryEntry> history;
    PushBoundedHistory(history, Entry("A"), 200);
    PushBoundedHistory(history, Entry("B"), 200);
    PushBoundedHistory(history, Entry("C"), 200);
    REQUIRE(history.size() == 3);
    CHECK(history[0].Title == "A");
    CHECK(history[1].Title == "B");
    CHECK(history[2].Title == "C");
}

TEST_CASE("PushBoundedHistory: keeps exactly cap entries at the boundary") {
    std::vector<ToastHistoryEntry> history;
    for (int i = 0; i < 3; ++i) {
        PushBoundedHistory(history, Entry(std::to_string(i)), 3);
    }
    REQUIRE(history.size() == 3);
    CHECK(history.front().Title == "0");
    CHECK(history.back().Title == "2");
}

TEST_CASE("PushBoundedHistory: over cap evicts oldest-first, newest retained") {
    std::vector<ToastHistoryEntry> history;
    for (int i = 0; i < 5; ++i) {
        PushBoundedHistory(history, Entry(std::to_string(i)), 3);
    }
    REQUIRE(history.size() == 3);
    CHECK(history[0].Title == "2"); // 0 and 1 evicted
    CHECK(history[1].Title == "3");
    CHECK(history[2].Title == "4");
}

TEST_CASE("PushBoundedHistory: cap 0 disables history (keeps nothing)") {
    std::vector<ToastHistoryEntry> history;
    PushBoundedHistory(history, Entry("A"), 0);
    CHECK(history.empty());
}

TEST_CASE("PushBoundedHistory: shrinking cap below current size trims the surplus") {
    std::vector<ToastHistoryEntry> history;
    PushBoundedHistory(history, Entry("A"), 10);
    PushBoundedHistory(history, Entry("B"), 10);
    PushBoundedHistory(history, Entry("C"), 10);
    PushBoundedHistory(history, Entry("D"), 2); // cap drops to 2 with 4 in flight
    REQUIRE(history.size() == 2);
    CHECK(history[0].Title == "C");
    CHECK(history[1].Title == "D");
}

TEST_CASE("PushBoundedHistory: retains a callable RowAction on the surviving entry") {
    std::vector<ToastHistoryEntry> history;
    int fired = 0;
    ToastHistoryEntry e = Entry("clickable");
    e.RowAction = [&fired]() { ++fired; };
    PushBoundedHistory(history, std::move(e), 200);
    REQUIRE(history.size() == 1);
    REQUIRE(static_cast<bool>(history[0].RowAction));
    history[0].RowAction();
    CHECK(fired == 1);
}

// UndoLedger (Ui/UndoLedgerPure.h) — the pending actions behind a toast's Undo button
// (docs/plans/input-ux-w2-drafts-undo.md). Each entry settles exactly once: Undo runs only its undo,
// Commit only its commit.
namespace {

struct UndoCounts {
    int Undone = 0;
    int Committed = 0;
};

std::uint64_t AddCounted(smatchet::undo::UndoLedger& ledger, UndoCounts& counts) {
    return ledger.Add([&counts]() { ++counts.Undone; }, [&counts]() { ++counts.Committed; });
}

} // namespace

TEST_CASE("UndoLedger: Undo runs the undo once and never the commit") {
    smatchet::undo::UndoLedger ledger;
    UndoCounts counts;
    const std::uint64_t id = AddCounted(ledger, counts);
    CHECK(id != 0);
    CHECK(ledger.Contains(id));
    CHECK(ledger.Undo(id));
    CHECK(counts.Undone == 1);
    CHECK(counts.Committed == 0);
    CHECK_FALSE(ledger.Contains(id));
    // Settled: neither a second Undo nor a later Commit runs anything.
    CHECK_FALSE(ledger.Undo(id));
    CHECK_FALSE(ledger.Commit(id));
    ledger.CommitAll();
    CHECK(counts.Undone == 1);
    CHECK(counts.Committed == 0);
}

TEST_CASE("UndoLedger: Commit runs the commit once and never the undo") {
    smatchet::undo::UndoLedger ledger;
    UndoCounts counts;
    const std::uint64_t id = AddCounted(ledger, counts);
    CHECK(ledger.Commit(id));
    CHECK_FALSE(ledger.Commit(id));
    CHECK_FALSE(ledger.Undo(id));
    CHECK(counts.Committed == 1);
    CHECK(counts.Undone == 0);
    CHECK(ledger.Size() == 0);
}

TEST_CASE("UndoLedger: CommitAll settles every pending entry, oldest first, and empty callbacks are fine") {
    smatchet::undo::UndoLedger ledger;
    std::vector<int> order;
    ledger.Add(nullptr, [&order]() { order.push_back(1); });
    ledger.Add(nullptr, nullptr);
    ledger.Add([]() {}, [&order]() { order.push_back(3); });
    ledger.CommitAll();
    CHECK(ledger.Size() == 0);
    REQUIRE(order.size() == 2);
    CHECK(order[0] == 1);
    CHECK(order[1] == 3);
}

TEST_CASE("UndoLedger: past the bound the oldest entry commits") {
    smatchet::undo::UndoLedger ledger;
    UndoCounts first;
    const std::uint64_t firstId = AddCounted(ledger, first);
    UndoCounts rest;
    for (std::size_t i = 1; i < smatchet::undo::UndoLedger::MaxEntries(); ++i) {
        AddCounted(ledger, rest);
    }
    CHECK(ledger.Size() == smatchet::undo::UndoLedger::MaxEntries());
    CHECK(first.Committed == 0);
    AddCounted(ledger, rest);
    CHECK(ledger.Size() == smatchet::undo::UndoLedger::MaxEntries());
    CHECK(first.Committed == 1);
    CHECK_FALSE(ledger.Contains(firstId));
    CHECK(rest.Committed == 0);
}

TEST_CASE("UndoLedger: a callback may settle or add other entries") {
    smatchet::undo::UndoLedger ledger;
    UndoCounts other;
    const std::uint64_t otherId = AddCounted(ledger, other);
    std::uint64_t addedId = 0;
    const std::uint64_t id =
        ledger.Add([&]() { ledger.Commit(otherId); }, [&]() { addedId = AddCounted(ledger, other); });
    CHECK(ledger.Undo(id));
    CHECK(other.Committed == 1);
    const std::uint64_t id2 = ledger.Add(nullptr, [&]() { addedId = AddCounted(ledger, other); });
    CHECK(ledger.Commit(id2));
    CHECK(addedId != 0);
    CHECK(ledger.Contains(addedId));
}
