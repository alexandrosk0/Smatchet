#pragma once

// MainThreadDispatcherDrain — the pure requeue step for the dispatcher's drain-time budget.
// Plan: docs/plans/shipped/memory-budget-and-lifetime-hardening.md § Phase 4.
// When a frame's Drain() runs out of its time budget before the queue empties, the untouched
// tail is deferred to the next frame. It must go AHEAD of any task posted by a worker during
// the unlocked drain run (those were posted later), and the merged queue must still honour the
// count cap by dropping the oldest. That ordering+trim is pure; it is templated on the task
// type so it can be unit-tested with a stand-in (e.g. int) instead of a move-only std::function.

#include <cstddef>
#include <utility>
#include <vector>

namespace smatchet {

/// Build the next-frame queue from the `deferred` tail (drained-out-of-time, FIFO order) and
/// the tasks that `arrived` while the drain ran: deferred first (posted earlier), then arrived,
/// trimmed to `maxSize` by dropping the oldest (front). Both inputs are moved into `out`.
template <typename T>
void RequeueDeferredFront(std::vector<T>& deferred, std::vector<T>& arrived, std::size_t maxSize,
                          std::vector<T>& out) {
    out.clear();
    out.reserve(deferred.size() + arrived.size());
    for (auto& t : deferred) {
        out.push_back(std::move(t));
    }
    for (auto& t : arrived) {
        out.push_back(std::move(t));
    }
    if (out.size() > maxSize) {
        const std::size_t drop = out.size() - maxSize;
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

} // namespace smatchet
