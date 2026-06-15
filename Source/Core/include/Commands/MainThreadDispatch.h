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
// Case (b) returns immediately with a SetException error.

#include "Commands/IMainThreadPoster.h"
#include "Commands/Command.h"

#include <exception>
#include <future>
#include <utility>

namespace smatchet {
namespace cmd {

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
    return future.get();
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
