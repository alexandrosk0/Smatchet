#ifndef SMATCHET_COMMANDS_IMAIN_THREAD_POSTER_H
#define SMATCHET_COMMANDS_IMAIN_THREAD_POSTER_H

// Pure interface that lets command-helper templates (Commands/MainThreadDispatch.h)
// hop work onto the UI thread WITHOUT including AppController.h — severing the
// Commands -> AppController include back-edge (DAG-ify core-include-dag, Phase 2).
// AppController implements this by delegating to its `mainThreadDispatcher` member.
// The interface lives in the Commands/ layer so MainThreadDispatch.h depends only
// on a same-layer header. Dependency-light: <functional> only — no Ui/GL include.

#include <functional>

class IMainThreadPoster {
  public:
    virtual ~IMainThreadPoster() = default;

    /// True iff the calling thread is the UI thread (the one that called Initialize).
    virtual bool IsOnUiThread() const = 0;

    /// Post `fn` to run on the UI thread at the next dispatcher drain. Safe from any
    /// thread; a no-op once shutdown has begun (see MainThreadDispatcher).
    virtual void PostToMainThread(std::function<void()> fn) = 0;
};

#endif // SMATCHET_COMMANDS_IMAIN_THREAD_POSTER_H
