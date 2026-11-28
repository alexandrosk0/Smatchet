#pragma once

// LaunchClaim — single-owner release of a "launch in progress" latch (Quality Pillar 3 / 6).
//
// A caller that sets a latch (a queue's `draining_`, a store-load claim, an in-flight key) and then hands
// a task to LaunchBackgroundTask has three ways to end up with no task running: the launch throws, the
// launcher drops the task silently (shutdown, empty task), or the task starts and finds shutdown set and
// skips its body. Releasing the latch in each place separately either misses a path (the latch sticks) or
// hits two (the latch is released twice). A LaunchClaim makes the release a single decision:
//
//   * The launched closure holds the claim through a std::shared_ptr and calls `TakeForRun()` first. True
//     means the body now owns the latch and releases it itself; false means the claim was already
//     released, and the body must do nothing.
//   * The closure's destruction (the last shared_ptr copy going away) or the launch's own catch block
//     calls `ReleaseIfUntaken()`, which runs `releaseUnrun` exactly once if no body took the claim.
//
// Exactly one party wins the atomic exchange, so `releaseUnrun` runs at most once and never after a body
// has started. Thread-safe. Lifetime: whatever `releaseUnrun` captures must outlive every closure that
// holds the claim (the same rule as the queues' own `this`).
//
// `releaseUnrun` may run on any thread, including inside a launcher that holds its own lock (the
// destroyed-unrun path), so it must take leaf locks only, never launch anything and never throw. A throw is
// contained and logged anyway.

#include "Logger.h"

#include <atomic>
#include <exception>
#include <functional>
#include <utility>

namespace smatchet {

class LaunchClaim {
  public:
    explicit LaunchClaim(std::function<void()> releaseUnrun) : releaseUnrun_(std::move(releaseUnrun)) {}
    ~LaunchClaim() { ReleaseIfUntaken(); }
    LaunchClaim(const LaunchClaim&) = delete;
    LaunchClaim& operator=(const LaunchClaim&) = delete;

    /// The launched body calls this first. True: the body owns the latch from here on. False: the claim
    /// was already released (launch failed or was dropped), so the body must not run.
    bool TakeForRun() noexcept { return !taken_.exchange(true, std::memory_order_acq_rel); }

    /// Runs `releaseUnrun` once when no body has taken the claim; a no-op after `TakeForRun()` succeeded
    /// or after an earlier release. Also called by the destructor.
    void ReleaseIfUntaken() noexcept {
        if (taken_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (!releaseUnrun_) {
            return;
        }
        try {
            releaseUnrun_();
        } catch (const std::exception& ex) {
            LOG_ERROR("LaunchClaim: release callable threw: %s", ex.what());
        } catch (...) {
            LOG_ERROR("LaunchClaim: release callable threw a non-std exception");
        }
    }

  private:
    std::atomic<bool> taken_{false};
    const std::function<void()> releaseUnrun_;
};

} // namespace smatchet
