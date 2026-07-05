#include "SmatchetMergeWatchNotifyServer.h"

#include "AppController.h"
#include "Logger.h"
#include "MergeWatchNotifyPure.h"
#include "SmatchetToast.h"

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <string>

SmatchetMergeWatchNotifyServer::SmatchetMergeWatchNotifyServer() : running_(false) {}

SmatchetMergeWatchNotifyServer::~SmatchetMergeWatchNotifyServer() { Stop(); }

void SmatchetMergeWatchNotifyServer::RegisterRoutes(AppController& app) {
    // Capture dispatcher by reference — outlives the server per AppController's
    // dtor ordering (Stop() runs before mainThreadDispatcher.BeginShutdown).
    MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;

    server_->Post("/merge-watch/notify", [&dispatcher](const httplib::Request& req, httplib::Response& res) {
        // The whole attacker-reachable decision surface (bounded parse → shape/type
        // validation → state allow-list → sanitize → toast plan) lives in
        // MergeWatchNotifyPure::PlanMergeWatchNotify (gap map Tier 1 #6 — doctested in
        // tests/Core/MergeWatchNotifyPure.test.cpp). This handler owns transport only.
        const MergeWatchNotifyPure::NotifyPlan plan = MergeWatchNotifyPure::PlanMergeWatchNotify(req.body);
        res.status = plan.HttpStatus;
        res.set_content(plan.ResponseBody, "application/json");
        if (!plan.Ok) {
            return;
        }

        // Post to UI thread — dispatcher is thread-safe + bounded.
        const std::string title = plan.Title;
        const std::string message = plan.Message;
        const ToastType type = plan.Type;
        dispatcher.PostToMainThread(
            [title, message, type]() { SmatchetToastManager::Instance().Push(title, message, type, 6000); });
    });

    // Health probe — daemon's smatchet-notify.sh can use to detect endpoint
    // availability without firing a real notification.
    server_->Get("/merge-watch/health", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content("{\"ok\":true}", "application/json");
    });
}

bool SmatchetMergeWatchNotifyServer::Start(AppController& app, std::uint16_t port) {
    if (running_.load(std::memory_order_acquire)) {
        LOG_WARN("SmatchetMergeWatchNotifyServer::Start: already running");
        return false;
    }
    server_ = std::make_unique<httplib::Server>();
    RegisterRoutes(app);

    // Bind 127.0.0.1 ONLY. cpp-httplib's bind_to_port returns true on success.
    if (!server_->bind_to_port("127.0.0.1", port)) {
        LOG_WARN("SmatchetMergeWatchNotifyServer::Start: bind 127.0.0.1:%u failed (in use?)",
                 static_cast<unsigned>(port));
        server_.reset();
        return false;
    }

    // #987: cap httplib's worker pool. The default task queue spawns
    // max(8, hardware_concurrency()-1) threads — a 47-thread creation burst on a
    // 48-core box — and one failed std::thread ctor under full build load threw
    // std::system_error out of the listen thread -> std::terminate. A localhost
    // endpoint receiving rare single POSTs needs 2 workers, not 47.
    server_->new_task_queue = [] {
        return new httplib::ThreadPool(2); // httplib owns the queue
    };

    // #987 review HIGH-1: join a stale listen thread before re-spawning. If a
    // previous listen thread died on its own (exception / early return) WITHOUT
    // Stop() being called, running_ is already false but listenThread_ is still
    // set and joinable — reassigning it below would destroy a joinable
    // std::thread → std::terminate, the very crash class this server hardens
    // against. (Stop() already joins+resets on the normal path; this covers the
    // self-death path.)
    if (listenThread_ && listenThread_->joinable()) {
        listenThread_->join();
    }
    listenThread_.reset();

    // running_ set BEFORE the spawn so the listen thread's own running_=false on
    // death is the last write (no post-spawn store to race it); the ctor-throw
    // catch below resets it on the failure path.
    running_.store(true, std::memory_order_release);
    // Spawn listen-loop thread. listen_after_bind blocks until Stop().
    // #987 review HIGH-2: the std::thread CONSTRUCTOR itself throws
    // std::system_error when the OS can't create a thread (the same -j48-build
    // pressure that motivated this fix). Guard it so a spawn failure degrades
    // gracefully instead of escaping Start() to std::terminate.
    try {
        listenThread_ = std::make_unique<std::thread>([this]() {
            // #987 / Pillar 3: an exception escaping a thread entry-point is
            // std::terminate. The notify server is non-critical — log, stop the
            // server so its bound socket fast-fails (callers get
            // connection-refused instead of hanging in the kernel backlog —
            // review MEDIUM-2), and mark it down; never kill the app.
            try {
                if (server_) {
                    server_->listen_after_bind();
                }
            } catch (const std::exception& e) {
                LOG_ERROR("SmatchetMergeWatchNotifyServer: listen thread died: %s", e.what());
            } catch (...) {
                LOG_ERROR("SmatchetMergeWatchNotifyServer: listen thread died: unknown exception");
            }
            if (server_) {
                server_->stop();
            }
            running_.store(false, std::memory_order_release);
        });
    } catch (const std::exception& e) {
        LOG_WARN("SmatchetMergeWatchNotifyServer::Start: listen thread spawn failed (%s); "
                 "notify server disabled this session",
                 e.what());
        running_.store(false, std::memory_order_release);
        server_->stop();
        server_.reset();
        listenThread_.reset();
        return false;
    }
    LOG_INFO("SmatchetMergeWatchNotifyServer: listening on 127.0.0.1:%u", static_cast<unsigned>(port));
    return true;
}

void SmatchetMergeWatchNotifyServer::Stop() {
    if (server_) {
        server_->stop();
    }
    if (listenThread_ && listenThread_->joinable()) {
        listenThread_->join();
    }
    listenThread_.reset();
    server_.reset();
    running_.store(false, std::memory_order_release);
}
