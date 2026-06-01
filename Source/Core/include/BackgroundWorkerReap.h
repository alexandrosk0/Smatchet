#pragma once

// BackgroundWorkerReap — the pure container half of AppController's background-worker reaping
// (docs/plans/shipped/memory-budget-and-lifetime-hardening.md § Phase 4). Extracted as a free
// template so the reap algorithm is unit-testable with fakes, without instantiating the heavy
// AppController or spawning real std::threads. AppController supplies the real predicates
// (done-flag check + std::thread::join).

#include <algorithm>
#include <cstddef>
#include <vector>

namespace smatchet {

/// Erase every worker that `reapable(w)` accepts, invoking `onReap(w)` on each (e.g. join its
/// thread) just before removal. Workers `reapable` rejects are kept, order preserved.
/// @returns the number of workers reaped.
template <typename Worker, typename Reapable, typename OnReap>
std::size_t ReapFinishedWorkers(std::vector<Worker>& workers, Reapable reapable, OnReap onReap) {
    std::size_t reaped = 0;
    workers.erase(std::remove_if(workers.begin(), workers.end(),
                                 [&](Worker& w) {
                                     if (!reapable(w)) {
                                         return false;
                                     }
                                     onReap(w);
                                     ++reaped;
                                     return true;
                                 }),
                  workers.end());
    return reaped;
}

} // namespace smatchet
