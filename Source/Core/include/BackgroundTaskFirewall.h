#pragma once

// BackgroundTaskFirewall — the pure exception-containment half of AppController's
// background-worker launch (issue #1081). An exception escaping a worker-thread
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
/// `ex.what()` (best-effort); otherwise it is left untouched.
template <typename TaskFn> BackgroundTaskOutcome RunBackgroundTaskFirewalled(TaskFn&& task, std::string& outWhat) {
    try {
        std::forward<TaskFn>(task)();
    } catch (const std::exception& ex) {
        // Best-effort what() capture: the string assignment itself can throw
        // (std::bad_alloc under memory pressure) and would escape this catch,
        // defeating the firewall — swallow it and report with an empty what.
        try {
            const char* msg = ex.what();
            outWhat = (msg != nullptr) ? msg : "";
        } catch (...) { // catch-all-ok: worker-thread firewall — must not propagate to std::terminate
            outWhat.clear();
        }
        return BackgroundTaskOutcome::StdException;
    } catch (...) { // catch-all-ok: worker-thread firewall — must not propagate to std::terminate
        return BackgroundTaskOutcome::UnknownException;
    }
    return BackgroundTaskOutcome::Completed;
}

} // namespace smatchet
