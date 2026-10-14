#pragma once

// SubmitPendingActionAsync — the worker/post-back idiom for a tracker write that goes through the
// pending-action queue (comments, worklogs, watches; Quality Pillar 6). The submit blocks on the
// network while online (Pillar 2: never on the UI thread), and its outcome is posted to the UI thread
// even when the submit throws (as a Failed result), so a caller's in-flight latch clears.
// Both posts go through PostCompletionToMainThread, so a poster that routes completions to
// MainThreadDispatcher::PostCompletionToMainThread keeps them when its queue overflows. A post the
// dispatcher cannot queue at all (no memory) is counted and logged there; the exit guard below also
// retries with a Failed result if a post throws.

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
        // A pointer, not a nested reference capture of `apply`: MSVC mistypes a reference capture of a
        // by-copy capture inside a const call operator (C2440) where Clang and GCC accept it.
        const ApplyFn* const applyPtr = &apply;
        bool posted = false;
        ScopeExit reportThrow([threadingPtr, applyPtr, &posted]() {
            if (posted) {
                return;
            }
            try {
                threadingPtr->PostCompletionToMainThread(
                    [applyFailed = *applyPtr]() { applyFailed(PendingActionSubmitResult()); });
            } catch (const std::exception& ex) {
                LOG_ERROR("SubmitPendingActionAsync: could not report a failed submit: %s", ex.what());
            } catch (...) {
                // Boundary tier: this runs in ScopeExit's destructor, where a throw would terminate the app.
                LOG_ERROR("SubmitPendingActionAsync: could not report a failed submit: unknown exception");
            }
        });
        const PendingActionSubmitResult result = submit();
        threadingPtr->PostCompletionToMainThread([apply, result]() { apply(result); });
        posted = true;
    });
}

} // namespace ui
} // namespace smatchet
