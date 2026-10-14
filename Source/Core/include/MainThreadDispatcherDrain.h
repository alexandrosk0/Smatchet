#pragma once

// MainThreadDispatcherDrain — the pure requeue step for the dispatcher's drain-time budget.
// Plan: docs/plans/shipped/memory-budget-and-lifetime-hardening.md § Phase 4.
// When a frame's Drain() runs out of its time budget before the queue empties, the untouched
// tail is deferred to the next frame. It must go AHEAD of any task posted by a worker during
// the unlocked drain run (those were posted later), and the merged queue must still honour the
// count caps by dropping the oldest. That ordering+trim is pure; it is templated on the task
// type so it can be unit-tested with a stand-in (e.g. int) instead of a move-only std::function.

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace smatchet {

/// Build the next-frame queue from the `deferred` tail (drained-out-of-time, FIFO order) and
/// the tasks that `arrived` while the drain ran: deferred first (posted earlier), then arrived,
/// trimmed to `maxSize` by dropping the oldest (front). Both inputs are moved into `out`.
template <typename T>
void RequeueDeferredFront(std::vector<T>& deferred, std::vector<T>& arrived, std::size_t maxSize, std::vector<T>& out) {
    out.clear();
    out.reserve(deferred.size() + arrived.size());
    // Move-append both ranges (deferred first, then arrived) via the std::move
    // range algorithm rather than raw push_back loops (cppcheck useStlAlgorithm).
    std::move(deferred.begin(), deferred.end(), std::back_inserter(out));
    std::move(arrived.begin(), arrived.end(), std::back_inserter(out));
    if (out.size() > maxSize) {
        const std::size_t drop = out.size() - maxSize;
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

/// What TrimDispatcherQueue removed, and how many completions remain queued.
struct DispatcherQueueTrim {
    std::size_t DroppedDroppable = 0;
    std::size_t DroppedCompletions = 0;
    std::size_t CompletionsLeft = 0;
};

/// Enforce the dispatcher's two count caps on a FIFO queue (oldest first), keeping the survivors in
/// order: the oldest droppable entries go until at most `maxDroppable` remain, and the oldest
/// completions go only when more than `maxCompletions` of them are queued. A droppable overflow
/// therefore never costs a completion. `isCompletion(entry)` classifies an entry. O(n).
template <typename T, typename IsCompletion>
DispatcherQueueTrim TrimDispatcherQueue(std::vector<T>& queue, std::size_t maxDroppable, std::size_t maxCompletions,
                                        IsCompletion isCompletion) {
    DispatcherQueueTrim trim;
    std::size_t completions = 0;
    for (const T& entry : queue) {
        if (isCompletion(entry)) {
            ++completions;
        }
    }
    const std::size_t droppable = queue.size() - completions;
    std::size_t dropDroppable = droppable > maxDroppable ? droppable - maxDroppable : 0;
    std::size_t dropCompletions = completions > maxCompletions ? completions - maxCompletions : 0;
    trim.DroppedDroppable = dropDroppable;
    trim.DroppedCompletions = dropCompletions;
    trim.CompletionsLeft = completions - dropCompletions;
    if (dropDroppable == 0 && dropCompletions == 0) {
        return trim;
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        std::size_t& toDrop = isCompletion(queue[i]) ? dropCompletions : dropDroppable;
        if (toDrop > 0) {
            --toDrop;
            continue;
        }
        if (kept != i) {
            queue[kept] = std::move(queue[i]);
        }
        ++kept;
    }
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(kept), queue.end());
    return trim;
}

} // namespace smatchet
