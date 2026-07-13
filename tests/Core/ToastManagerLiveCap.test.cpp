// ToastManagerLiveCap — the live (on-screen) toast stack is bounded even though error
// toasts are sticky (never auto-expire, UX critique M2). A burst of failures — e.g. one
// error toast per failed bulk edit — must not stack past the viewport top and demand one
// dismissal click each: Push evicts oldest-first past kMaxLiveToasts while the bounded
// history ring and the unread-error counter keep the full trail. Push/LiveCount/History
// never touch ImGui, so this links in the pure bucket-A block (Render stays bucket-E).

#include <doctest/doctest.h>

#include "SmatchetToast.h"

#include <cstddef>
#include <string>

TEST_CASE("SmatchetToastManager: live stack bounded at kMaxLiveToasts; history + unread errors keep the trail") {
    SmatchetToastManager& mgr = SmatchetToastManager::Instance();
    // Shared singleton — start from a clean slate and restore one at the end.
    mgr.DismissAllLive();
    mgr.ClearHistory();
    mgr.MarkHistorySeen();

    const std::size_t cap = SmatchetToastManager::kMaxLiveToasts;
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
