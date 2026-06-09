#pragma once

// BackgroundTaskFirewall — the pure exception-containment half of AppController's
// background-worker launch (issue #1081 / PR #1080). An exception escaping a worker-thread
// function calls std::terminate — the UI-thread SEH guard does NOT cover worker threads —
// so a single throw in any background task takes the whole app down. This firewall contains
// it: the task is abandoned, the outcome is reported to the caller (which logs), and the app
// stays up (Pillar-3 never-crash / graceful degradation). Extracted as a free template so the
// firewall contract — including "the done-flag still publishes when the task throws" — is
// unit-testable without instantiating AppController or spawning real std::threads. No Logger
// dependency here: the header stays pure, the caller logs.

#include <exception>
#include <string>
#include <utility>

namespace smatchet {

enum class BackgroundTaskOutcome { Completed, StdException, UnknownException };

/// Run `task` behind an exception firewall. Never lets an exception propagate (which would
/// std::terminate the worker thread's process). On a std::exception, `outWhat` receives
/// `ex.what()`; otherwise it is left untouched.
template <typename TaskFn> BackgroundTaskOutcome RunBackgroundTaskFirewalled(TaskFn&& task, std::string& outWhat) {
    try {
        std::forward<TaskFn>(task)();
    } catch (const std::exception& ex) {
        outWhat = ex.what();
        return BackgroundTaskOutcome::StdException;
    } catch (...) { // catch-all-ok: worker-thread firewall — must not propagate to std::terminate
        return BackgroundTaskOutcome::UnknownException;
    }
    return BackgroundTaskOutcome::Completed;
}

} // namespace smatchet
