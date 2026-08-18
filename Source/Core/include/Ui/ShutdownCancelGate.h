#ifndef SMATCHET_UI_SHUTDOWN_CANCEL_GATE_H
#define SMATCHET_UI_SHUTDOWN_CANCEL_GATE_H

// Cancel-before-join handshake for UI windows that own std::async workers
// holding a raw AppController* (docs/plans/shipped/ui-freeze-pillar2-blocking.md,
// WS-A / #1150).
//
// Hoisted out of SmatchetUserInfoUi so the ASan UAF guard
// (tests/Core/UserInfoActivityCancelUaf.test.cpp) drives THIS destructor
// ordering rather than a hand-copied re-statement of it (DR29).
//
// The shape it encodes: a worker parked inside a multi-second backend scan is
// only released by TWO signals — the cooperative CancelToken (which it polls at
// its IO boundaries) AND the backend's in-scan IO cancel (which unblocks it from
// the inside). Both must fire BEFORE the owner's std::future members destruct,
// because ~future joins: signal first and the join is near-instant with the
// captured controller still alive (Pillar-3, no UAF; Pillar-2, no stall); join
// first and the UI thread blocks for the whole scan.
//
// Declare a gate as the LAST member of the owning class. Members destruct in
// reverse declaration order, so the gate's destructor runs before the future
// members it protects — the ordering is structural, not a comment.

#include "CancelToken.h"

#include <functional>
#include <utility>

namespace smatchet {
namespace ui {

class ShutdownCancelGate {
  public:
    ShutdownCancelGate() = default;
    ShutdownCancelGate(const ShutdownCancelGate&) = delete;
    ShutdownCancelGate& operator=(const ShutdownCancelGate&) = delete;

    /// Fires the handshake for an owner that never got an explicit teardown call.
    /// Idempotent with `Signal()`: cancelling twice is a no-op and the IO cancel
    /// hook is required to tolerate a repeat (production's ClearPaneUserActivity
    /// does).
    ~ShutdownCancelGate() { Signal(); }

    /// The token every worker launched by this owner copies and polls.
    const CancelToken& Token() const { return token_; }

    /// Latch the backend's in-scan IO cancel (production: AppController::
    /// ClearPaneUserActivity for the owning pane). Called once the owner knows
    /// which controller/pane its workers are bound to; may be re-latched on
    /// retarget. A gate with no hook still performs the cooperative half.
    void SetIoCancel(std::function<void()> ioCancel) { ioCancel_ = std::move(ioCancel); }

    /// Cooperative cancel first, then release the in-scan IO — in that order, so
    /// a worker woken by the IO cancel already observes `IsCancelled()` and
    /// returns without touching its captured raw pointer again.
    void Signal() {
        token_.Cancel();
        if (ioCancel_) {
            ioCancel_();
        }
    }

    /// Teardown-then-reuse form (window close / retarget): the same handshake,
    /// then a fresh flag for the next run. Workers still holding the OLD token
    /// copy keep observing the cancelled flag and stop.
    void SignalAndReset() {
        Signal();
        token_.Reset();
    }

  private:
    std::function<void()> ioCancel_;
    CancelToken token_;
};

} // namespace ui
} // namespace smatchet

#endif // SMATCHET_UI_SHUTDOWN_CANCEL_GATE_H
