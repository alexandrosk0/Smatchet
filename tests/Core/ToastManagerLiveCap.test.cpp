// ToastManagerLiveCap — the live (on-screen) toast stack is bounded even though error
// toasts are sticky (never auto-expire, UX critique M2). A burst of failures — e.g. one
// error toast per failed bulk edit — must not stack past the viewport top and demand one
// dismissal click each: Push evicts oldest-first past MaxLiveToasts() while the bounded
// history ring and the unread-error counter keep the full trail. Push/LiveCount/History
// never touch ImGui, so this links in the pure bucket-A block (Render stays bucket-E).

#include <doctest/doctest.h>

#include "SmatchetToast.h"
#include "../support/ScopedLimits.h"

#include <cstddef>
#include <cstdint>
#include <string>

TEST_CASE("SmatchetToastManager: live stack bounded at MaxLiveToasts(); history + unread errors keep the trail") {
    // Pin the two limits this test's arithmetic depends on (live cap 6, history 200); restored on exit.
    const smatchet_tests::ScopedLimits pinnedLimits([](smatchet::limits::LimitsConfig& l) {
        l.ToastMaxLive = 6;
        l.ToastHistoryMax = 200;
    });
    SmatchetToastManager& mgr = SmatchetToastManager::Instance();
    // Shared singleton — start from a clean slate and restore one at the end.
    mgr.DismissAllLive();
    mgr.ClearHistory();
    mgr.MarkHistorySeen();

    const std::size_t cap = SmatchetToastManager::MaxLiveToasts();
    const int pushed = static_cast<int>(cap) + 4;
    for (int i = 0; i < pushed; ++i) {
        mgr.Push("Save failed", "SMAT-" + std::to_string(i), ToastType::Error, 4000);
    }

    // Live stack clamped; the full trail survives in history + the unread-error badge.
    CHECK(mgr.LiveCount() == cap);
    CHECK(mgr.History().size() == static_cast<std::size_t>(pushed));
    CHECK(mgr.UnreadErrorCount() == pushed);

    // History eviction is oldest-first, so the newest entry is the last pushed.
    REQUIRE_FALSE(mgr.History().empty());
    CHECK(mgr.History().back().Message == "SMAT-" + std::to_string(pushed - 1));

    // Viewing the Notification Center clears the unread-error badge.
    mgr.MarkHistorySeen();
    CHECK(mgr.UnreadErrorCount() == 0);

    // Non-error toasts share the same live bound.
    mgr.DismissAllLive();
    for (int i = 0; i < pushed; ++i) {
        mgr.Push("Copied", "", ToastType::Info, 1500);
    }
    CHECK(mgr.LiveCount() == cap);
    CHECK(mgr.UnreadErrorCount() == 0); // info toasts never count as unread errors

    mgr.DismissAllLive();
    mgr.ClearHistory();
}

TEST_CASE("SmatchetToastManager: HasLive follows the toast on screen, not the history") {
    const smatchet_tests::ScopedLimits pinnedLimits([](smatchet::limits::LimitsConfig& l) {
        l.ToastMaxLive = 2;
        l.ToastHistoryMax = 200;
    });
    SmatchetToastManager& mgr = SmatchetToastManager::Instance();
    mgr.DismissAllLive();
    mgr.ClearHistory();

    CHECK_FALSE(mgr.HasLive("Shortcut not run", "bad args"));
    mgr.Push("Shortcut not run", "bad args", ToastType::Warning, 6000);
    CHECK(mgr.HasLive("Shortcut not run", "bad args"));
    // Title and message both have to match.
    CHECK_FALSE(mgr.HasLive("Shortcut not run", "other args"));
    CHECK_FALSE(mgr.HasLive("Toolbar button not run", "bad args"));

    // Dismissed: no longer on screen, though the history keeps it.
    mgr.DismissAllLive();
    CHECK_FALSE(mgr.HasLive("Shortcut not run", "bad args"));
    CHECK(mgr.History().size() == 1);

    // Pushed out at the live cap by newer toasts.
    mgr.Push("Shortcut not run", "bad args", ToastType::Warning, 6000);
    mgr.Push("Copied", "one", ToastType::Info, 1500);
    CHECK(mgr.HasLive("Shortcut not run", "bad args"));
    mgr.Push("Copied", "two", ToastType::Info, 1500);
    CHECK_FALSE(mgr.HasLive("Shortcut not run", "bad args"));

    // Past its time but not yet removed (removal waits for the next Render): not on screen.
    mgr.DismissAllLive();
    mgr.Push("Shortcut not run", "bad args", ToastType::Warning, 0);
    CHECK(mgr.LiveCount() == 1);
    CHECK_FALSE(mgr.HasLive("Shortcut not run", "bad args"));
    // An error stays until it is dismissed, whatever its time.
    mgr.Push("Save failed", "disk full", ToastType::Error, 0);
    CHECK(mgr.HasLive("Save failed", "disk full"));

    mgr.DismissAllLive();
    mgr.MarkHistorySeen(); // the error above counts as unread: not left for the next case
    mgr.ClearHistory();
}

// Undoable toasts (docs/plans/input-ux-w2-drafts-undo.md): every way an undoable toast leaves the
// screen settles its action exactly once — Undo runs only the undo; expiry, dismissal, eviction at the
// live cap, DismissAllLive and CommitPendingUndos run only the commit. Push / UndoNewest /
// DismissAllLive never touch ImGui, so these run without a UI session.
namespace {

struct ToastUndoCounts {
    int Undone = 0;
    int Committed = 0;
};

void PushCountedUndoable(SmatchetToastManager& mgr, const std::string& title, ToastUndoCounts& counts) {
    mgr.PushUndoable(title, "", 8000, [&counts]() { ++counts.Undone; }, [&counts]() { ++counts.Committed; });
}

} // namespace

TEST_CASE("SmatchetToastManager: UndoNewest undoes the newest undoable toast and removes it") {
    SmatchetToastManager& mgr = SmatchetToastManager::Instance();
    mgr.DismissAllLive();
    ToastUndoCounts older;
    ToastUndoCounts newer;
    PushCountedUndoable(mgr, "View deleted", older);
    mgr.Push("Copied", "", ToastType::Info, 4000); // a plain toast on top is skipped
    PushCountedUndoable(mgr, "Conversation cleared", newer);
    CHECK(mgr.LiveUndoableCount() == 2);

    CHECK(mgr.UndoNewest());
    CHECK(newer.Undone == 1);
    CHECK(newer.Committed == 0);
    CHECK(older.Undone == 0);
    CHECK(mgr.LiveUndoableCount() == 1);
    CHECK_FALSE(mgr.HasLive("Conversation cleared", ""));

    CHECK(mgr.UndoNewest());
    CHECK(older.Undone == 1);
    CHECK_FALSE(mgr.UndoNewest());
    CHECK(mgr.LiveCount() == 1); // the plain toast is untouched

    mgr.DismissAllLive();
    CHECK(older.Committed == 0);
    CHECK(newer.Committed == 0);
    mgr.ClearHistory();
}

TEST_CASE("SmatchetToastManager: dismissing undoable toasts commits each action once") {
    SmatchetToastManager& mgr = SmatchetToastManager::Instance();
    mgr.DismissAllLive();
    ToastUndoCounts a;
    ToastUndoCounts b;
    PushCountedUndoable(mgr, "A", a);
    PushCountedUndoable(mgr, "B", b);
    mgr.DismissAllLive();
    CHECK(a.Committed == 1);
    CHECK(b.Committed == 1);
    CHECK(a.Undone + b.Undone == 0);
    CHECK_FALSE(mgr.UndoNewest());
    mgr.DismissAllLive();
    CHECK(a.Committed == 1);
    mgr.ClearHistory();
}

TEST_CASE("SmatchetToastManager: an undoable toast pushed out at the live cap commits") {
    const smatchet_tests::ScopedLimits pinnedLimits([](smatchet::limits::LimitsConfig& l) {
        l.ToastMaxLive = 3;
        l.ToastHistoryMax = 200;
    });
    SmatchetToastManager& mgr = SmatchetToastManager::Instance();
    mgr.DismissAllLive();
    ToastUndoCounts evicted;
    PushCountedUndoable(mgr, "Oldest", evicted);
    mgr.Push("one", "", ToastType::Info, 4000);
    mgr.Push("two", "", ToastType::Info, 4000);
    CHECK(evicted.Committed == 0);
    mgr.Push("three", "", ToastType::Info, 4000);
    CHECK(mgr.LiveCount() == 3);
    CHECK(evicted.Committed == 1);
    CHECK(evicted.Undone == 0);
    CHECK(mgr.LiveUndoableCount() == 0);
    mgr.DismissAllLive();
    CHECK(evicted.Committed == 1);
    mgr.ClearHistory();
}

TEST_CASE("SmatchetToastManager: CommitPendingUndos commits now and leaves nothing to undo") {
    SmatchetToastManager& mgr = SmatchetToastManager::Instance();
    mgr.DismissAllLive();
    ToastUndoCounts counts;
    PushCountedUndoable(mgr, "Conversation cleared", counts);
    mgr.CommitPendingUndos();
    CHECK(counts.Committed == 1);
    CHECK(mgr.LiveCount() == 1);
    CHECK(mgr.LiveUndoableCount() == 0);
    CHECK_FALSE(mgr.UndoNewest());
    mgr.DismissAllLive();
    CHECK(counts.Committed == 1);
    CHECK(counts.Undone == 0);
    mgr.ClearHistory();
}

TEST_CASE("SmatchetToastManager: CommitUndo settles one pending undo and takes its button off the toast") {
    SmatchetToastManager& mgr = SmatchetToastManager::Instance();
    mgr.DismissAllLive();
    ToastUndoCounts first;
    ToastUndoCounts second;
    const std::uint64_t firstId = mgr.PushUndoable(
        "Conversation cleared", "", 8000, [&first]() { ++first.Undone; }, [&first]() { ++first.Committed; });
    PushCountedUndoable(mgr, "View deleted", second);
    CHECK(firstId != 0);

    CHECK(mgr.CommitUndo(firstId));
    CHECK(first.Committed == 1);
    CHECK(first.Undone == 0);
    CHECK(mgr.LiveCount() == 2);          // the toast itself stays up
    CHECK(mgr.LiveUndoableCount() == 1);  // without its Undo button
    CHECK_FALSE(mgr.CommitUndo(firstId)); // settled once
    CHECK_FALSE(mgr.CommitUndo(0));

    // Ctrl+Z reaches only the toast that still has an Undo.
    CHECK(mgr.UndoNewest());
    CHECK(second.Undone == 1);
    CHECK_FALSE(mgr.UndoNewest());
    mgr.DismissAllLive();
    CHECK(first.Committed == 1);
    CHECK(second.Committed == 0);
    mgr.ClearHistory();
}
