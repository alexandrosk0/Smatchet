#ifndef SMATCHET_COMMANDS_IAPP_THREADING_H
#define SMATCHET_COMMANDS_IAPP_THREADING_H

// Narrow threading facet of AppController (fan-in Phase 6 T4). Covers the
// worker/post-back idiom — launch a joined background task, then marshal the
// result onto the UI thread — used by Ui TUs and plugin helpers that need
// nothing else from AppController. Extends IMainThreadPoster so a single
// reference serves both halves of the idiom and converts implicitly at any
// existing IMainThreadPoster& call site. Lives in the Commands/ layer next to
// its base: the Interfaces/ facets are rank-0 and must not include upward.

#include "Commands/IMainThreadPoster.h"

#include <functional>

class IAppThreading : public IMainThreadPoster {
  public:
    ~IAppThreading() override = default;

    /// Spawn `task` on a tracked background thread — joined at shutdown, never
    /// detached (the no-detach lint forbids raw std::thread::detach). Post
    /// results back to the UI thread via `PostToMainThread` inside `task`.
    virtual void LaunchBackgroundTask(std::function<void()> task) = 0;
};

#endif // SMATCHET_COMMANDS_IAPP_THREADING_H
