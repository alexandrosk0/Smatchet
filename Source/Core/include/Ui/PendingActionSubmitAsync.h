#pragma once

// SubmitPendingActionAsync — the worker/post-back idiom for a tracker write that goes through the
// pending-action queue (comments, worklogs, watches; Quality Pillar 6). The submit blocks on the
// network while online (Pillar 2: never on the UI thread), and its outcome always reaches the UI
// thread — a submit that throws posts a Failed result — so a caller's in-flight latch always clears.

#include "Commands/IAppThreading.h"
#include "Logger.h"
#include "PendingActionTypes.h"
#include "ScopeExit.h"

#include <exception>

namespace smatchet {
namespace ui {

/// Runs `submit()` (returns PendingActionSubmitResult) on a background task, then `apply(result)` on
/// the UI thread. Throws only when the launch itself fails; the caller then releases its latch.
/// `submit` and `apply` are copied into the task, so capture by value.
template <typename SubmitFn, typename ApplyFn>
void SubmitPendingActionAsync(IAppThreading& threading, SubmitFn submit, ApplyFn apply) {
    IAppThreading* threadingPtr = &threading;
    threading.LaunchBackgroundTask([threadingPtr, submit, apply]() {
        bool posted = false;
        ScopeExit reportThrow([threadingPtr, &apply, &posted]() {
            if (posted) {
                return;
            }
            try {
                threadingPtr->PostToMainThread([apply]() { apply(PendingActionSubmitResult()); });
            } catch (const std::exception& ex) {
                LOG_ERROR("SubmitPendingActionAsync: could not report a failed submit: %s", ex.what());
            } catch (...) {
                // Boundary tier: this runs in ScopeExit's destructor, where a throw would terminate the app.
                LOG_ERROR("SubmitPendingActionAsync: could not report a failed submit: unknown exception");
            }
        });
        const PendingActionSubmitResult result = submit();
        threadingPtr->PostToMainThread([apply, result]() { apply(result); });
        posted = true;
    });
}

} // namespace ui
} // namespace smatchet
