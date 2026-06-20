#pragma once

// ToastHistoryPure — the STL-only core of the toast manager's in-session history
// (ticket-change-monitor plan, S2 / § Files-to-modify #13). Carries the types the
// history ring is made of (ToastType, the optional per-entry RowAction, and one
// ToastHistoryEntry) plus the bounded-append helper, all free of ImGui so the ring
// behaviour is unit-testable without a UI session. SmatchetToast.h includes this and
// layers the transient-toast rendering + singleton on top.

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

/// Severity / accent of a toast. Lives here (not SmatchetToast.h) so the pure history
/// unit needs no ImGui; SmatchetToast.h re-exports it transitively for every call site.
enum class ToastType { Info, Success, Warning, Error };

/// What a Notification Center row does when clicked. Empty (no target) for a plain
/// informational toast; ticket toasts bind it to FocusTicketInGrid (wired in S3). The
/// transient toast's own click opens the center generically and does NOT invoke this.
using ToastRowAction = std::function<void()>;

/// One recorded toast, retained in the bounded session history after the transient
/// notification fades. CreatedAt is wall-clock for display ordering/timestamping.
struct ToastHistoryEntry {
    std::chrono::system_clock::time_point CreatedAt;
    std::string Title;
    std::string Message;
    ToastType Type = ToastType::Info;
    ToastRowAction RowAction;
};

/// Append `entry` to `history`, evicting oldest-first so the ring never exceeds `cap`.
/// `cap == 0` means "keep nothing" (history disabled) — the vector is left empty. The
/// common case (one over cap) erases a single front element; a smaller cap than the
/// current size trims the surplus in one range-erase.
inline void PushBoundedHistory(std::vector<ToastHistoryEntry>& history, ToastHistoryEntry entry, std::size_t cap) {
    if (cap == 0) {
        history.clear();
        return;
    }
    history.push_back(std::move(entry));
    if (history.size() > cap) {
        history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - cap));
    }
}
