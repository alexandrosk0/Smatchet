#ifndef SMATCHET_COMMANDS_MAIN_THREAD_DISPATCH_H
#define SMATCHET_COMMANDS_MAIN_THREAD_DISPATCH_H

// Helper for command handlers that mutate or read UI-thread-owned state.
// Background:
//   Command handlers run on whichever thread called `CommandRegistry::Dispatch`:
//     - MCP REST/JSON-RPC handlers → httplib worker thread
//     - Lua `commands.invoke` from automation worker → Lua bg thread
//     - In-app Command Palette → UI thread
//     - Lua hooks invoked during draw → UI thread
//   Several handlers touch state that is *not* internally synchronized (the most notable
//   is `SmatchetUI::ViewState`, modified from the UI thread by the Views Dashboard window
//   and the grid sort/column code). Mutating those structures from a worker is UB.
// Solution: this helper runs `fn` on the UI thread.
//   - If the caller is already on the UI thread (palette dispatch, Lua hook): `fn` runs
//     inline — no deadlock, no extra latency.
//   - Otherwise: post to `app.mainThreadDispatcher` (drained at the top of every
//     SmatchetUI::Draw frame) and block on a one-shot `std::promise`. Round-trip is one
//     frame (~16 ms at 60 Hz, ~7 ms at 144 Hz) — well under any agent expectation.
// Exception safety: if `fn` throws, the exception is captured in the future and rethrown
// on the worker side. Callers can wrap the call site in try/catch and produce a HandlerError
// envelope; the UI thread never sees the unhandled exception.
// Timeout: the helper does NOT impose a timeout. Posting blocks indefinitely; callers that
// need a cap should wrap with `wait_for` on their own future chain. In practice the UI
// thread drains the queue every frame, so the only ways this can hang are
//   (a) the UI thread is itself blocked (e.g. inside a long Lua handler), or
//   (b) AppController::mainThreadDispatcher.BeginShutdown() was called.
// Case (b) returns immediately with a shutdown error (see DR15 below).
// DR15 shutdown-hang fix: the dispatcher can DISCARD a posted task without ever running it
// -- silently once BeginShutdown() has flipped its no-op flag (called first in ~AppController,
// before the worker join), and via the drop-oldest eviction when the bounded queue saturates.
// The posted lambda owns a std::promise whose future this frame blocks on. As long as this
// frame ALSO held a reference to that promise, a discarded task's destruction left the promise
// alive-but-unfulfilled, so no broken_promise fired and future.get() blocked the shutdown join
// forever. RunOnUiThread now hands sole promise ownership to the posted task, so a discarded
// task breaks the future and future.get() unblocks with a shutdown error instead of hanging.

#include "Commands/IMainThreadPoster.h"
#include "Commands/Command.h"

#include <exception>
#include <future>
#include <stdexcept>
#include <utility>

namespace smatchet {
namespace cmd {

/// Block on the future produced by RunOnUiThread's posted task. Extracted so RunOnUiThread stays
/// small and the DR15 broken_promise -> shutdown-error translation lives in one place: a task the
/// dispatcher discarded (shutdown / eviction) leaves the future with a broken_promise, which we
/// surface as a clear shutdown error rather than the opaque std::future_error. A genuine handler
/// exception (delivered via set_exception) is NOT a std::future_error, so it propagates unchanged.
/// Declared before RunOnUiThread so its unqualified call resolves at template-definition point
/// (std::future's namespace is std, so ADL would never reach this helper).
template <typename Result> Result WaitForUiThreadResult(std::future<Result> future) {
    try {
        return future.get();
    } catch (const std::future_error&) {
        throw std::runtime_error(
            "UI-thread dispatch cancelled: main-thread task was dropped before running "
            "(dispatcher shutting down or queue saturated).");
    }
}

/// Run `fn` on the UI thread and return its result. Type-erased over the result type so
/// it works for `CommandResult`, `bool`, plain JSON, etc. — but the most common usage is
/// `RunOnUiThread<CommandResult>(app, []() -> CommandResult { ... });`.
template <typename Result, typename Fn> Result RunOnUiThread(IMainThreadPoster& app, Fn fn) {
    // Fast path: already on UI thread.
    if (app.IsOnUiThread()) {
        return fn();
    }

    // Slow path: cross-thread hop via the bounded main-thread dispatcher.
    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    app.PostToMainThread([promise, fn = std::move(fn)]() mutable {
        try {
            promise->set_value(fn());
        } catch (...) {
            try {
                promise->set_exception(std::current_exception());
            } catch (...) {
                // promise already satisfied somehow; ignore.
            }
        }
    });
    // DR15: release this frame's promise reference so the posted task is its sole owner. If the
    // dispatcher discards the task (post-BeginShutdown no-op, or drop-oldest eviction) the task's
    // destruction now breaks the shared state, unblocking future.get() below with a broken_promise
    // instead of hanging the shutdown join. On the normal path the task still owns its copy and
    // fulfils the promise before it is destroyed, so this reset is a no-op for the result.
    promise.reset();
    return WaitForUiThreadResult(std::move(future));
}

/// Specialisation-free helper for handlers that already return `CommandResult`. Catches
/// any exception escaped from `fn` and converts it into a `HandlerError` envelope so the
/// CLI gets a well-formed response instead of an unhandled exception from `future::get()`.
template <typename Fn> CommandResult RunOnUiThreadAsCommandResult(IMainThreadPoster& app, Fn fn) {
    try {
        return RunOnUiThread<CommandResult>(app, std::move(fn));
    } catch (const std::exception& e) {
        return CommandResult::Failure(ErrorCode::HandlerError, std::string("UI-thread handler threw: ") + e.what());
    } catch (...) {
        return CommandResult::Failure(ErrorCode::HandlerError, "UI-thread handler threw (unknown exception).");
    }
}

} // namespace cmd
} // namespace smatchet

#endif // SMATCHET_COMMANDS_MAIN_THREAD_DISPATCH_H
