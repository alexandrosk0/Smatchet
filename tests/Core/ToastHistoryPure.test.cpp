// Bucket-A coverage for ToastHistoryPure (ticket-change-monitor plan, S2 / § Files-to-modify
// #13) — the bounded-ring append the toast manager records every Push into. Pure STL (no
// ImGui), so the eviction + order + row-action retention are testable without a UI session.

#include "Ui/ToastHistoryPure.h"

#include "doctest/doctest.h"

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
