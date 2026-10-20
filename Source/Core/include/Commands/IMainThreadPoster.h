#ifndef SMATCHET_COMMANDS_IMAIN_THREAD_POSTER_H
#define SMATCHET_COMMANDS_IMAIN_THREAD_POSTER_H

// Pure interface that lets command-helper templates (Commands/MainThreadDispatch.h)
// hop work onto the UI thread WITHOUT including AppController.h — severing the
// Commands -> AppController include back-edge (DAG-ify core-include-dag, Phase 2).
// AppController implements this by delegating to its `mainThreadDispatcher` member.
// The interface lives in the Commands/ layer so MainThreadDispatch.h depends only
// on a same-layer header. Dependency-light: <functional> + <utility> only — no Ui/GL include.

#include <functional>
#include <utility>

class IMainThreadPoster {
  public:
    virtual ~IMainThreadPoster() = default;

    /// True iff the calling thread is the UI thread (the one that called Initialize).
    virtual bool IsOnUiThread() const = 0;

    /// Post `fn` to run on the UI thread at the next dispatcher drain. Safe from any
    /// thread; a no-op once shutdown has begun (see MainThreadDispatcher).
    virtual void PostToMainThread(std::function<void()> fn) = 0;

    /// Post a completion (releases a caller's in-flight latch). MainThreadDispatcher keeps completions
    /// through a droppable-task overflow; only its own completion cap evicts one. Defaults to
    /// PostToMainThread, droppable, for posters without a completion path.
    virtual void PostCompletionToMainThread(std::function<void()> fn) { PostToMainThread(std::move(fn)); }

    /// True when something drains the posted tasks while a command waits: the frame loop of the running application.
    /// False for the one-shot in-process CLI (`Smatchet.exe cmd ...` in a build without MCP), which dispatches the
    /// command on the thread that booted the application and exits right after, with no frame ever drawn. A command
    /// that starts UI-thread work from a worker and waits for it must then pump the queue itself
    /// (`DrainMainThreadOnce`), or the work never runs and the process exits with it undone. Defaults to true.
    virtual bool HasFrameLoop() const { return true; }

    /// Runs the posted tasks once on the calling thread, which must be the UI thread. Only meaningful when
    /// `HasFrameLoop()` is false; a no-op by default.
    virtual void DrainMainThreadOnce() {}

    /// False once the host has begun shutting down and posts are no-ops. A worker waiting for an outcome that a
    /// UI-thread post delivers (cache.recreate) stops waiting then: the post will never run. Safe from any
    /// thread. Defaults to true.
    virtual bool AcceptsPosts() const { return true; }
};

#endif // SMATCHET_COMMANDS_IMAIN_THREAD_POSTER_H
