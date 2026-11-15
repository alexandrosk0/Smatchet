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
