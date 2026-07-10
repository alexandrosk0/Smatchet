#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

// SmatchetMergeWatchNotifyServer — Phase 4b of
// docs/plans/shipped/smatchet-merge-watcher.md. Localhost HTTP receiver for the
// merge-watcher daemon's in-app toast notifications.
// Contract:
// - Binds 127.0.0.1 ONLY (hard-coded; no env override). Bug here = local RCE
//   via toast injection — see agents/security-review.md
//   § smatchet-merge-watcher localhost HTTP endpoint.
// - Accepts POST /merge-watch/notify with JSON body
//   {pr: int, state: string, message: string, pr_url: string}.
// - Validates payload server-side; rejects 400 on missing/oversized fields.
// - Posts toast append via MainThreadDispatcher::PostToMainThread (Pillar 2 —
//   HTTP runs on cpp-httplib worker thread; no sync I/O reaches UI thread).
// Owned by AppController; started on Initialize(), stopped at the top of
// ~AppController BEFORE mainThreadDispatcher.BeginShutdown() fires so any
// in-flight POST callbacks observe a live dispatcher.

class IMainThreadPoster; // the server only posts toast plans to the UI thread — see Commands/IMainThreadPoster.h
namespace httplib {
class Server;
}

class SmatchetMergeWatchNotifyServer {
  public:
    SmatchetMergeWatchNotifyServer();
    ~SmatchetMergeWatchNotifyServer();

    /// Start the listener. Binds 127.0.0.1:<port>; spawns the listen thread.
    /// Returns true on bind success; false if port is in use OR server already
    /// running. Per LOCKED design decision 3 (notification channel = Smatchet
    /// toast + Windows native), this is best-effort — failure is logged but
    /// doesn't prevent Smatchet startup. Daemon falls back to Windows native.
    bool Start(IMainThreadPoster& poster, std::uint16_t port = 7679);

    /// Stop the listener + join the thread. Idempotent.
    void Stop();

    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  private:
    /// Register the POST /merge-watch/notify + GET /merge-watch/health routes on
    /// server_. Split out of Start() to keep it under the function-size cap.
    void RegisterRoutes(IMainThreadPoster& poster);

    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<std::thread> listenThread_;
    std::atomic<bool> running_;
};
