#ifndef SMATCHET_UI_CANCEL_TOKEN_H
#define SMATCHET_UI_CANCEL_TOKEN_H

// Cooperative cancellation primitive for UI-owned background workers
// (docs/plans/ui-freeze-pillar2-blocking.md, WS-A).
// Pillar-2 (UI never freezes): destroying a `std::async` future whose worker is
// still running blocks the UI thread until the worker drains — several seconds
// for a p4-annotate activity scan (#1150) or a bulk-import run (#734). A worker
// that polls a `CancelToken` at its IO/loop boundaries observes a signal and
// returns promptly, so the eventual destructor-join is near-instant.
// Pillar-3 (never crash): cancellation is *cooperative*, not a forced abort —
// the join is KEPT (signal then join). A worker that captured a raw `appPtr` /
// `this` must check the token and return BEFORE dereferencing the captured
// pointer again; the owner signals cancel before the futures destruct, the
// destructor still joins, and the captured controller stays alive across that
// near-instant join. The atomic flag lives in a `shared_ptr`, so it outlives
// both the owner and the worker regardless of which drops its copy first.

#include <atomic>
#include <future>
#include <memory>
#include <utility>
#include <vector>

namespace smatchet {
namespace ui {

/**
 * A copyable handle onto a shared `std::atomic<bool>` cancel flag. The owner
 * keeps one copy and calls `Cancel()`; each worker captures its own copy (by
 * value) and polls `IsCancelled()` at its await/loop/IO-chunk boundaries.
 *
 * Default-constructed tokens are *valid and not cancelled* — copying shares the
 * underlying flag, so a worker observes the owner's signal. The flag is held in
 * a `shared_ptr`, so it outlives whichever of owner/worker drops its copy last;
 * `IsCancelled()` on a (never-expected) null handle reports cancelled, so a
 * moved-from token fails safe toward "stop".
 */
class CancelToken {
  public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    /// Request cancellation. Idempotent; safe from any thread.
    void Cancel() const {
        if (flag_) {
            flag_->store(true, std::memory_order_release);
        }
    }

    /// True once `Cancel()` has been called (or the handle is null — fail-safe).
    bool IsCancelled() const { return !flag_ || flag_->load(std::memory_order_acquire); }

    /// Swap in a fresh (un-cancelled) flag for a relaunch on the same owner.
    /// Workers still holding the OLD handle keep observing the old (possibly
    /// cancelled) flag — exactly what a relaunch wants, so the abandoned worker
    /// still stops while the new worker runs against the new flag.
    void Reset() { flag_ = std::make_shared<std::atomic<bool>>(false); }

  private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

/**
 * Move every still-valid future out of `futures` into `graveyard` WITHOUT
 * waiting on any of them, leaving `futures` empty. This is the non-blocking
 * half of every abandon path in the UI: the caller has already signalled the
 * shared `CancelToken`, so the abandoned workers are winding down, and the
 * graveyard is drained off the UI hot path (at teardown) instead of inline.
 * Shared leaf so owners that keep their own indexed `std::vector<std::future>`
 * (bulk import) and `CancellableFutureSet` run the identical code.
 */
template <typename T>
void AbandonFuturesInto(std::vector<std::future<T>>& futures, std::vector<std::future<T>>& graveyard) {
    for (auto& f : futures) {
        if (f.valid()) {
            graveyard.push_back(std::move(f));
        }
    }
    futures.clear();
}

/**
 * Owner-side helper that bundles one shared `CancelToken` with the futures of
 * the workers that observe it. The owner hands `Token()` to each worker body,
 * `Add()`s the future it returns, and on teardown / window-close / `.clear()`:
 *
 *   - `SignalCancelAll()` flips the flag so every live worker stops at its next
 *     cancel-check (non-blocking),
 *   - `AbandonInto(graveyard)` moves the futures out without an inline wait, so
 *     the UI frame never blocks (the graveyard is drained off the hot path),
 *   - `JoinAll()` waits each future at app teardown (signal-then-join, fast)
 *     while the captured controller is still alive — the Pillar-3 guarantee.
 *
 * `T` is the future payload type (e.g. `IssueCreateResult`).
 */
template <typename T> class CancellableFutureSet {
  public:
    /// The shared token every worker added here should capture (by value) and poll.
    const CancelToken& Token() const { return token_; }

    /// Adopt a future whose worker observes `Token()`. Skips invalid futures.
    void Add(std::future<T>&& fut) {
        if (fut.valid()) {
            futures_.push_back(std::move(fut));
        }
    }

    bool Empty() const { return futures_.empty(); }
    std::size_t Size() const { return futures_.size(); }

    /// Flip the cancel flag for every worker added here. Non-blocking.
    void SignalCancelAll() const { token_.Cancel(); }

    /// Move all futures into `graveyard` WITHOUT waiting — the caller drains the
    /// graveyard off the UI hot path. Callers normally `SignalCancelAll()` first
    /// so the abandoned workers are already winding down. The set is left empty
    /// with a fresh token so it can be reused for a new run.
    void AbandonInto(std::vector<std::future<T>>& graveyard) {
        AbandonFuturesInto(futures_, graveyard);
        token_.Reset();
    }

    /// Teardown drain: signal cancel, then wait every future (fast, because the
    /// workers are already stopping). Swallows worker exceptions — a shutdown
    /// drain must never throw. Call while the captured controller is still alive.
    void JoinAll() {
        token_.Cancel();
        for (auto& f : futures_) {
            if (!f.valid()) {
                continue;
            }
            try {
                f.wait();
                (void)f.get();
            } catch (...) { // catch-all-ok: swallow worker exception on shutdown drain
            }
        }
        futures_.clear();
    }

  private:
    CancelToken token_;
    std::vector<std::future<T>> futures_;
};

} // namespace ui
} // namespace smatchet

#endif // SMATCHET_UI_CANCEL_TOKEN_H
