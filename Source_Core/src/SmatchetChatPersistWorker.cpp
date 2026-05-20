#include "SmatchetChatPersistWorker.h"

#if defined(SMATCHET_WITH_AI)

#include "LocalCacheManager.h"
#include "Logger.h"
#include "MainThreadDispatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace smatchet {
namespace ai {
namespace chat_persist {

namespace {

constexpr std::size_t kQueueSoftCap = 1024;
constexpr std::chrono::milliseconds kStopDrainBudget{250};

struct WorkerState {
    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;
    std::deque<Op> queue;
    bool running = false;
    std::atomic<bool> shouldStop{false};
    // Pointers borrowed from the caller. Cleared on Stop so a late Enqueue racing past
    // the running check cannot dereference a dead LCM.
    LocalCacheManager* lcm = nullptr;
    MainThreadDispatcher* dispatcher = nullptr;
    OnAppendCallback onAppend;
};

WorkerState& State() {
    static WorkerState s;
    return s;
}

void ProcessOne(WorkerState& s, Op op) {
    // Caller already snapshotted these under the lock; revalidate so a concurrent Stop
    // that nulled the pointers between pop_front and ProcessOne degrades to a drop
    // instead of a segfault.
    LocalCacheManager* lcm = s.lcm;
    if (!lcm) {
        return;
    }
    switch (op.kind) {
    case OpKind::Append: {
        const std::int64_t rowId = lcm->AppendChatMessage(op.message);
        if (rowId > 0 && s.onAppend && s.dispatcher) {
            const std::size_t idx = op.messageIndex;
            const std::int64_t capturedRowId = rowId;
            OnAppendCallback cb = s.onAppend;
            s.dispatcher->PostToMainThread([cb, idx, capturedRowId]() { cb(idx, capturedRowId); });
        }
        break;
    }
    case OpKind::UpdatePin:
        lcm->UpdateChatMessagePin(op.rowId, op.pinned);
        break;
    case OpKind::ClearAll:
        lcm->ClearChatMessages();
        break;
    case OpKind::Trim:
        lcm->TrimChatMessages(op.trimCap);
        break;
    }
}

void WorkerLoop() {
    auto& s = State();
    for (;;) {
        Op op;
        {
            std::unique_lock<std::mutex> lk(s.mtx);
            s.cv.wait(lk, [&]() { return s.shouldStop.load(std::memory_order_acquire) || !s.queue.empty(); });
            if (s.queue.empty() && s.shouldStop.load(std::memory_order_acquire)) {
                return;
            }
            op = std::move(s.queue.front());
            s.queue.pop_front();
        }
        try {
            ProcessOne(s, op);
        } catch (const std::exception& ex) {
            LOG_WARN("chat_persist: op kind=%d threw %s", static_cast<int>(op.kind), ex.what());
        } catch (...) {
            LOG_WARN("chat_persist: op kind=%d threw unknown exception", static_cast<int>(op.kind));
        }
    }
}

} // namespace

void Start(LocalCacheManager& lcm, MainThreadDispatcher& dispatcher, OnAppendCallback onAppend) {
    auto& s = State();
    std::thread launched;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.running) {
            LOG_WARN("chat_persist::Start called while already running — ignoring");
            return;
        }
        s.lcm = &lcm;
        s.dispatcher = &dispatcher;
        s.onAppend = std::move(onAppend);
        s.shouldStop.store(false, std::memory_order_release);
        s.queue.clear();
        s.running = true;
    }
    // Construct the thread outside the lock so it cannot deadlock against the
    // WorkerLoop's first lock acquisition on slow scheduling.
    launched = std::thread(WorkerLoop);
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.thread = std::move(launched);
    }
    LOG_INFO("chat_persist: worker started");
}

void Stop() {
    auto& s = State();
    std::thread workerToJoin;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.running) {
            return;
        }
        s.shouldStop.store(true, std::memory_order_release);
    }
    s.cv.notify_all();

    // Best-effort drain within budget. Poll-sleep rather than condvar-wait so the
    // worker thread retains exclusive control over the queue mutex during drain.
    const auto deadline = std::chrono::steady_clock::now() + kStopDrainBudget;
    for (;;) {
        bool empty = false;
        {
            std::lock_guard<std::mutex> lk(s.mtx);
            empty = s.queue.empty();
        }
        if (empty || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.queue.empty()) {
            LOG_WARN("chat_persist: drain budget exhausted with %zu ops pending; dropping",
                     static_cast<std::size_t>(s.queue.size()));
            s.queue.clear();
        }
        workerToJoin = std::move(s.thread);
        s.running = false;
        s.lcm = nullptr;
        s.dispatcher = nullptr;
        s.onAppend = nullptr;
    }
    s.cv.notify_all();
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
    LOG_INFO("chat_persist: worker stopped");
}

void Enqueue(Op op) {
    auto& s = State();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.running) {
            return;
        }
        if (op.kind == OpKind::Trim) {
            // Collapse: drop every pending Trim so only the latest cap survives.
            // Use the erase-remove idiom — std::remove_if is the C++14-canonical
            // form per cppcheck `useStlAlgorithm` over the raw `it = erase(it)` loop.
            s.queue.erase(
                std::remove_if(s.queue.begin(), s.queue.end(), [](const Op& o) { return o.kind == OpKind::Trim; }),
                s.queue.end());
        }
        if (s.queue.size() >= kQueueSoftCap) {
            // Drop the oldest non-Trim op. `std::find_if` is the cppcheck-canonical
            // shape; the raw for-loop tripped `useStlAlgorithm`.
            auto victim =
                std::find_if(s.queue.begin(), s.queue.end(), [](const Op& o) { return o.kind != OpKind::Trim; });
            if (victim != s.queue.end()) {
                s.queue.erase(victim);
            } else {
                // Every queued op is Trim — collapse them all (the new Trim about
                // to be pushed is the latest cap; older ones are stale anyway).
                s.queue.clear();
            }
            LOG_WARN("chat_persist: queue saturated, dropped oldest op (size=%zu)",
                     static_cast<std::size_t>(s.queue.size()));
        }
        s.queue.push_back(std::move(op));
    }
    // Notify unconditionally — we always reach here with a fresh queue entry; the
    // earlier `notify` flag was dead code (cppcheck `knownConditionTrueFalse`).
    s.cv.notify_one();
}

} // namespace chat_persist
} // namespace ai
} // namespace smatchet

#endif // SMATCHET_WITH_AI
