// Bucket-A doctest for the grid's undo history (Ui/GridUndoPure.h): push / take / take-newest per pane, the bound,
// inverse order, a bulk entry, a cleared field, a failed change and an entry that is still saving. Header-only.

#include "Ui/GridUndoPure.h"

#include <doctest/doctest.h>

using smatchet::grid::undo::CellChange;
using smatchet::grid::undo::ChangeState;
using smatchet::grid::undo::CheckTarget;
using smatchet::grid::undo::Entry;
using smatchet::grid::undo::History;
using smatchet::grid::undo::Inverse;
using smatchet::grid::undo::kMaxEntries;
using smatchet::grid::undo::TargetState;

namespace {
CellChange Change(const char* issue, const char* field, const char* before, const char* after) {
    CellChange c;
    c.IssueId = issue;
    c.FieldId = field;
    c.Before = {before};
    c.After = {after};
    c.BeforeDisplay = before;
    return c;
}

Entry One(const char* pane, const char* issue, const char* before, const char* after) {
    Entry e;
    e.PaneId = pane;
    e.Changes.push_back(Change(issue, "summary", before, after));
    return e;
}
} // namespace

TEST_CASE("History: Push stamps increasing ids and Take removes exactly that entry") {
    History h;
    const std::uint64_t a = h.Push(One("p", "A-1", "a", "b"));
    const std::uint64_t b = h.Push(One("p", "A-2", "c", "d"));
    CHECK(a != 0);
    CHECK(b > a);
    CHECK(h.Size() == 2);
    Entry out;
    CHECK(h.Take(a, out));
    CHECK(out.Id == a);
    CHECK(out.Changes.size() == 1);
    CHECK(h.Size() == 1);
    CHECK_FALSE(h.Take(a, out)); // gone
    CHECK(h.Contains(b));
    CHECK_FALSE(h.Contains(a));
}

TEST_CASE("History: past kMaxEntries the oldest entry is dropped") {
    History h;
    std::uint64_t first = 0;
    for (std::size_t i = 0; i < kMaxEntries + 3; ++i) {
        const std::uint64_t id = h.Push(One("p", "A-1", "x", "y"));
        if (i == 0) {
            first = id;
        }
    }
    CHECK(h.Size() == kMaxEntries);
    CHECK_FALSE(h.Contains(first));
}

TEST_CASE("History: TakeNewest is per pane and takes the newest landed entry") {
    History h;
    const std::uint64_t a = h.Push(One("left", "A-1", "a", "b"));
    const std::uint64_t b = h.Push(One("right", "A-2", "c", "d"));
    const std::uint64_t c = h.Push(One("left", "A-3", "e", "f"));
    h.MarkLanded(a, "A-1", "summary", "b");
    h.MarkLanded(b, "A-2", "summary", "d");
    h.MarkLanded(c, "A-3", "summary", "f");
    Entry out;
    CHECK(h.TakeNewest("left", out) == History::TakeResult::Taken);
    CHECK(out.Id == c);
    CHECK(h.TakeNewest("left", out) == History::TakeResult::Taken);
    CHECK(out.Id == a);
    CHECK(h.TakeNewest("left", out) == History::TakeResult::Empty); // the right pane's entry is not ours
    CHECK(h.Size() == 1);
}

TEST_CASE("History: an entry with a Pending change is not taken and stays") {
    History h;
    const std::uint64_t id = h.Push(One("p", "A-1", "a", "b"));
    Entry out;
    CHECK(h.TakeNewest("p", out) == History::TakeResult::StillSaving);
    CHECK(h.Size() == 1);
    CHECK(h.MarkLanded(id, "A-1", "summary", "b"));
    CHECK(h.TakeNewest("p", out) == History::TakeResult::Taken);
    CHECK(h.Size() == 0);
}

TEST_CASE("History: MarkFailed drops the change and removes an emptied entry") {
    History h;
    Entry e;
    e.PaneId = "p";
    e.BulkJobId = 7;
    e.Changes.push_back(Change("A-1", "status", "To Do", "Done"));
    e.Changes.push_back(Change("A-2", "status", "Open", "Done"));
    const std::uint64_t id = h.Push(std::move(e));
    CHECK(h.MarkFailed(id, "A-1", "status"));
    CHECK(h.Size() == 1);
    CHECK_FALSE(h.MarkFailed(id, "A-1", "status")); // already dropped
    CHECK(h.MarkFailed(id, "A-2", "status"));
    CHECK(h.Size() == 0);                                   // emptied entry is gone
    CHECK_FALSE(h.MarkLanded(id, "A-2", "status", "Done")); // unknown id is a no-op
}

TEST_CASE("Inverse: newest change first, Before restored, AfterDisplay becomes the conflict base") {
    History h;
    Entry e;
    e.PaneId = "p";
    e.Changes.push_back(Change("A-1", "status", "To Do", "Done"));
    e.Changes.push_back(Change("A-2", "status", "Open", "Done"));
    const std::uint64_t id = h.Push(std::move(e));
    h.MarkLanded(id, "A-1", "status", "Done");
    h.MarkLanded(id, "A-2", "status", "Done (Resolved)");
    Entry out;
    REQUIRE(h.Take(id, out));
    const std::vector<CellChange> inv = Inverse(out);
    REQUIRE(inv.size() == 2);
    CHECK(inv[0].IssueId == "A-2"); // newest first
    CHECK(inv[0].After == std::vector<std::string>{"Open"});
    CHECK(inv[0].BeforeDisplay == "Done (Resolved)"); // the value we wrote is the base
    CHECK(inv[1].IssueId == "A-1");
    CHECK(inv[1].After == std::vector<std::string>{"To Do"});
}

TEST_CASE("Inverse: a bulk entry gives each row its own Before back") {
    Entry e;
    e.PaneId = "p";
    e.BulkJobId = 3;
    e.Changes.push_back(Change("A-1", "priority", "High", "Low"));
    e.Changes.push_back(Change("A-2", "priority", "Medium", "Low"));
    for (std::size_t i = 0; i < e.Changes.size(); ++i) {
        e.Changes[i].State = ChangeState::Landed;
        e.Changes[i].AfterDisplay = "Low";
    }
    const std::vector<CellChange> inv = Inverse(e);
    REQUIRE(inv.size() == 2);
    CHECK(inv[0].After == std::vector<std::string>{"Medium"});
    CHECK(inv[1].After == std::vector<std::string>{"High"});
}

TEST_CASE("Inverse: a cleared field (empty AfterDisplay) is still inverted; pending and failed are not") {
    Entry e;
    e.PaneId = "p";
    e.Changes.push_back(Change("A-1", "labels", "x", ""));
    e.Changes.push_back(Change("A-2", "labels", "y", "z"));
    e.Changes.push_back(Change("A-3", "labels", "w", "v"));
    e.Changes[0].State = ChangeState::Landed; // AfterDisplay stays empty: the field was cleared
    e.Changes[1].State = ChangeState::Pending;
    e.Changes[2].State = ChangeState::Failed;
    const std::vector<CellChange> inv = Inverse(e);
    REQUIRE(inv.size() == 1);
    CHECK(inv[0].IssueId == "A-1");
    CHECK(inv[0].BeforeDisplay.empty());
    CHECK(inv[0].After == std::vector<std::string>{"x"});
}

TEST_CASE("CheckTarget: another site or account is Switched whatever the generation says") {
    CHECK(CheckTarget("Jira@a.example#111", 3, "Jira@b.example#111", 4) == TargetState::Switched);
    CHECK(CheckTarget("Jira@a.example#111", 3, "Jira@a.example#222", 3) == TargetState::Switched);
    CHECK(CheckTarget("GitHub@api.github.com/o/one", 1, "Linear@api.linear.app/t", 1) == TargetState::Switched);
    CHECK(CheckTarget("Jira@a.example#111", 3, "", 0) == TargetState::Switched); // the pane is gone
}

TEST_CASE("CheckTarget: the same tracker is Current, or Rebound once the pane has another client") {
    CHECK(CheckTarget("Jira@a.example#111", 3, "Jira@a.example#111", 3) == TargetState::Current);
    CHECK(CheckTarget("Jira@a.example#111", 3, "Jira@a.example#111", 5) == TargetState::Rebound);
}

TEST_CASE("History: DropIf removes only the stale entries of the named pane") {
    History h;
    const std::uint64_t a = h.Push(One("left", "A-1", "a", "b"));
    const std::uint64_t b = h.Push(One("right", "A-2", "c", "d"));
    const std::uint64_t c = h.Push(One("left", "A-3", "e", "f"));
    std::size_t offered = 0;
    h.DropIf("left", [&](const Entry& e) {
        ++offered;
        return e.Id == c;
    });
    CHECK(offered == 2); // the right pane's entry is never offered
    CHECK(h.Contains(a));
    CHECK(h.Contains(b));
    CHECK_FALSE(h.Contains(c));
    h.DropIf("left", [](const Entry&) { return true; });
    CHECK_FALSE(h.Contains(a));
    CHECK(h.Contains(b));
    CHECK(h.Size() == 1);
}
